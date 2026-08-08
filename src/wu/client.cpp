#include "wu/client.h"

#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "wu/key_source.h"

namespace {

const char *const api_host = "https://api.weather.com";

/*
 * Metric and English, requested explicitly.
 *
 * The units parameter is a REQUEST and not a guarantee, so whatever
 * reads these responses converts explicitly anyway rather than trusting
 * the flag -- a silent unit error is a graph that is wrong and looks
 * fine, which is the failure this project keeps naming.
 */
const char *const common_query = "units=m&language=en-US&format=json";

} // namespace

const char *bbq_wu_product_name(bbq_wu_product product) {
	switch (product) {
	case bbq_wu_product::observed:
		return "observed";
	case bbq_wu_product::current_station:
		return "current";
	case bbq_wu_product::current_point:
		return "current";
	case bbq_wu_product::nowcast:
		return "nowcast";
	case bbq_wu_product::hourly:
		return "hourly";
	}

	return "unknown";
}

bbq_wu_client::bbq_wu_client(QNetworkAccessManager *net,
                             bbq_wu_key_source *keys, QObject *parent)
        : QObject(parent), m_net(net), m_keys(keys) {
	/*
	 * Connected ONCE, here, and each handler drains the whole queue.
	 *
	 * The previous shape made a pair of connections per queued request
	 * and let each lambda tear down only its own, so whichever fired
	 * left the other alive -- and the survivor either re-sent a request
	 * that had already failed or failed one that had already succeeded.
	 * Sec 2.3.1 has the full account; the point of this shape is that
	 * there is no per-request bookkeeping left to get wrong.
	 */
	connect(m_keys, &bbq_wu_key_source::acquired, this, [this]() {
		/*
		 * Taken before the loop, so a resend that somehow queued again
		 * would wait for the next signal instead of being drained by
		 * the iteration that queued it.
		 */
		const QList<pending> ready_to_go = m_waiting;
		m_waiting.clear();

		for (const pending &request : ready_to_go) {
			send(request.product, request.path, request.query,
			     request.may_retry);
		}
	});

	connect(m_keys, &bbq_wu_key_source::failed, this,
	        [this](const QString &reason) {
		const QList<pending> abandoned = m_waiting;
		m_waiting.clear();

		for (const pending &request : abandoned) {
			emit failed(request.product, tr("no API key: %1").arg(reason));
		}
	});
}

void bbq_wu_client::fetch_hourly(double latitude, double longitude) {
	send(bbq_wu_product::hourly, QStringLiteral("/v3/wx/forecast/hourly/15day"),
	     QStringLiteral("geocode=%1,%2").arg(latitude).arg(longitude), true);
}

void bbq_wu_client::fetch_nowcast(double latitude, double longitude) {
	send(bbq_wu_product::nowcast,
	     QStringLiteral("/v3/wx/forecast/fifteenminute"),
	     QStringLiteral("geocode=%1,%2").arg(latitude).arg(longitude), true);
}

void bbq_wu_client::fetch_observed(const QString &station_id,
                                   const QString &date) {
	/*
	 * history/all rather than history/hourly: 288 rows a day against
	 * 24, which makes the observed band the finest of the three at five
	 * minutes (sec 2.6). The coarser one exists and is not used here.
	 */
	send(bbq_wu_product::observed, QStringLiteral("/v2/pws/history/all"),
	     QStringLiteral("stationId=%1&date=%2").arg(station_id, date), true);
}

void bbq_wu_client::fetch_current_station(const QString &station_id) {
	const QString query = QStringLiteral("stationId=%1").arg(station_id);
	send(bbq_wu_product::current_station,
	     QStringLiteral("/v2/pws/observations/current"), query, true);
}

void bbq_wu_client::fetch_current_point(double latitude, double longitude) {
	QString query = QStringLiteral("geocode=");
	query += QString::number(latitude);
	query += QStringLiteral(",");
	query += QString::number(longitude);
	send(bbq_wu_product::current_point,
	     QStringLiteral("/v3/wx/observations/current"), query, true);
}

void bbq_wu_client::send(bbq_wu_product product, const QString &path,
                         const QString &query, bool may_retry) {
	if (!m_keys->has_key()) {
		/*
		 * Queued, not connected. The handlers in the constructor drain
		 * this list, so a request waits exactly once however many other
		 * requests are waiting beside it.
		 */
		pending request;
		request.product = product;
		request.path = path;
		request.query = query;
		request.may_retry = may_retry;
		m_waiting.append(request);

		m_keys->acquire();
		return;
	}

	const QUrl url(QStringLiteral("%1%2?apiKey=%3&%4&%5")
	                       .arg(QString::fromLatin1(api_host), path,
	                            m_keys->key(), query,
	                            QString::fromLatin1(common_query)));

	QNetworkRequest request(url);
	QNetworkReply *reply = m_net->get(request);

	connect(reply, &QNetworkReply::finished, this,
	        [this, reply, product, path, query, may_retry]() {
		reply->deleteLater();

		const int status = reply->attribute(
		                            QNetworkRequest::HttpStatusCodeAttribute)
		                           .toInt();

		/*
		 * A rotation, seen from out here. Throw the key away and try
		 * once -- once, because a retry loop against somebody else's
		 * quota is how a scraped key gets a pattern noticed and closed
		 * (sec 2.5).
		 */
		if ((status == 401 || status == 403) && may_retry) {
			m_keys->invalidate();
			send(product, path, query, false);
			return;
		}

		if (reply->error() != QNetworkReply::NoError) {
			emit failed(product, reply->errorString());
			return;
		}

		const QByteArray body = reply->readAll();

		/*
		 * An empty body is not a malformed one, and saying so matters.
		 *
		 * This is what an unknown or silent station looks like: HTTP
		 * 204 with nothing in it. Reported as a parse error it reads
		 * as "the service is broken" when it means "there is no data
		 * for what you asked", and those want completely different
		 * reactions from whoever sees the message -- one is a bug to
		 * chase and the other is a station to re-pick (sec 2.6.6).
		 */
		if (body.isEmpty()) {
			emit failed(product, tr("no data (HTTP %1) -- for the observed "
			                        "band this usually means the station "
			                        "is unknown or has reported nothing")
			                             .arg(status));
			return;
		}

		QJsonParseError parse_error;
		const QJsonDocument document = QJsonDocument::fromJson(body,
		                                                       &parse_error);

		if (document.isNull()) {
			emit failed(product, tr("malformed response: %1")
			                             .arg(parse_error.errorString()));
			return;
		}

		emit ready(product, document);
	});
}
