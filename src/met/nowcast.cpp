#include "met/nowcast.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimeZone>
#include <QUrl>

#include <vector>

namespace {

const char *const provider_name = "met.no";

/*
 * A truthful User-Agent, and the contrast with the WU path is the point
 * rather than an accident.
 *
 * MET Norway requires clients to identify themselves with something
 * that can be contacted, and blocks the generic ones. That is a
 * condition of a service given away for free, so it is met honestly --
 * where sec 2.2's scraper wears a browser's agent because the whole
 * approach there is already the compromised one.
 *
 * A provider reached legitimately gets a truthful agent. The key_source
 * comment says the same thing from the other side.
 */
const char *const agent =
	"bbq-predictor/0.1 (https://vibes.se; funklord@vibes.se)";

const char *const nowcast_url =
	"https://api.met.no/weatherapi/nowcast/2.0/complete";

double number_or(const QJsonObject &object, const char *key, bool *found) {
	const QJsonValue value = object.value(QString::fromLatin1(key));
	*found = value.isDouble();
	return *found ? value.toDouble() : 0.0;
}

} // namespace

bbq_met_client::bbq_met_client(QNetworkAccessManager *net, QObject *parent)
        : QObject(parent), m_net(net) {
}

void bbq_met_client::fetch_nowcast(double latitude, double longitude) {
	QString target = QString::fromLatin1(nowcast_url);
	target += QStringLiteral("?lat=");
	target += QString::number(latitude, 'f', 4);
	target += QStringLiteral("&lon=");
	target += QString::number(longitude, 'f', 4);

	QNetworkRequest request((QUrl(target)));
	request.setHeader(QNetworkRequest::UserAgentHeader,
	                  QString::fromLatin1(agent));

	QNetworkReply *reply = m_net->get(request);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			emit failed(reply->errorString());
			return;
		}

		QJsonParseError parse_error;
		const QByteArray body = reply->readAll();
		const QJsonDocument document = QJsonDocument::fromJson(body,
		                                                       &parse_error);

		if (document.isNull()) {
			emit failed(tr("malformed response: %1")
			                    .arg(parse_error.errorString()));
			return;
		}

		emit ready(document);
	});
}

bbq_series bbq_met_read_nowcast(const QJsonDocument &response) {
	bbq_series series(bbq_band::nowcast_fine, QString::fromLatin1(provider_name));

	if (!response.isObject()) {
		return series;
	}

	const QJsonObject properties =
	        response.object().value(QStringLiteral("properties")).toObject();
	const QJsonArray points =
	        properties.value(QStringLiteral("timeseries")).toArray();

	if (points.isEmpty()) {
		return series;
	}

	std::vector<bbq_sample> samples;
	samples.reserve(points.size());

	for (const QJsonValue &value : points) {
		const QJsonObject point = value.toObject();

		const QString stamp = point.value(QStringLiteral("time")).toString();
		const QDateTime when = QDateTime::fromString(stamp, Qt::ISODate);
		if (!when.isValid()) {
			/*
			 * One unreadable timestamp discards the band, for the same
			 * reason the WU readers do it: a series short by an
			 * arbitrary sample draws a gap that means nothing.
			 */
			return bbq_series(bbq_band::nowcast_fine,
			                  QString::fromLatin1(provider_name));
		}

		const QJsonObject data = point.value(QStringLiteral("data")).toObject();
		const QJsonObject instant =
		        data.value(QStringLiteral("instant")).toObject();
		const QJsonObject details =
		        instant.value(QStringLiteral("details")).toObject();

		bbq_sample sample;
		sample.start_utc = when.toSecsSinceEpoch();

		bool found = false;

		const double temperature = number_or(details, "air_temperature", &found);
		if (found) {
			sample.temperature = temperature;
		}

		/* Already mm/h, which is what the model stores (sec 3.2). */
		const double rate = number_or(details, "precipitation_rate", &found);
		if (found) {
			sample.precip_rate = rate;
		}

		/* Metres per second here, kilometres per hour in the model. */
		const double wind = number_or(details, "wind_speed", &found);
		if (found) {
			sample.wind_kph = wind * 3.6;
		}

		samples.push_back(sample);
	}

	/*
	 * Durations from the real spacing, as everywhere else. MET steps at
	 * five minutes for the first hour and then coarsens, so a fixed
	 * step would be wrong for the tail of the very band being read.
	 */
	for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
		const qint64 stride = samples[i + 1].start_utc - samples[i].start_utc;
		samples[i].duration_s = static_cast<int>(stride);
	}

	if (samples.size() >= 2) {
		const std::size_t last = samples.size() - 1;
		samples[last].duration_s = samples[last - 1].duration_s;
	} else if (!samples.empty()) {
		samples[0].duration_s = 300;
	}

	/* UTC throughout, and the response says so with a Z. */
	series.set_zone(QTimeZone(QTimeZone::UTC));
	series.set_samples(std::move(samples));
	return series;
}
