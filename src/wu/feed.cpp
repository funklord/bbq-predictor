#include "wu/feed.h"

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTimer>

#include "wu/client.h"
#include "wu/key_source.h"
#include "met/nowcast.h"
#include "wu/reader.h"

namespace {

/*
 * How long each product stays fresh, in seconds (project.md sec 2.5).
 *
 * Matched to how fast the data behind it actually moves, measured in
 * sec 2.6 rather than guessed: the station reports about every five
 * minutes, the nowcast steps in quarter hours, and the hourly forecast
 * is hourly. Asking faster than the source changes buys nothing and
 * spends somebody else's quota.
 *
 * The observed band is deliberately slower than the station's own
 * cadence. It lags by up to twenty-odd minutes anyway (sec 3.9.1), so
 * a tighter interval would mostly re-download the same rows; the
 * current band is what keeps the present sharp.
 */
int freshness_s(bbq_wu_product product) {
	switch (product) {
	case bbq_wu_product::current_station:
	case bbq_wu_product::current_point:
		return 5 * 60;
	case bbq_wu_product::observed:
		return 10 * 60;
	case bbq_wu_product::nowcast:
		return 15 * 60;
	case bbq_wu_product::hourly:
		return 60 * 60;
	}

	return 15 * 60;
}

bbq_series read_for(bbq_wu_product product, const QJsonDocument &document) {
	switch (product) {
	case bbq_wu_product::observed:
		return bbq_wu_read_observed(document);
	case bbq_wu_product::current_station:
		return bbq_wu_read_current_station(document);
	case bbq_wu_product::current_point:
		return bbq_wu_read_current_point(document);
	case bbq_wu_product::nowcast:
		return bbq_wu_read_nowcast(document);
	case bbq_wu_product::hourly:
		return bbq_wu_read_hourly(document);
	}

	return bbq_series();
}

} // namespace

bbq_wu_feed::bbq_wu_feed(QObject *parent) : QObject(parent) {
	m_net = new QNetworkAccessManager(this);
	m_keys = new bbq_wu_key_source(m_net, this);
	m_client = new bbq_wu_client(m_net, m_keys, this);
	m_met = new bbq_met_client(m_net, this);

	/*
	 * MET Norway serves the nowcast band where it can (sec 2.9). Five
	 * minute steps against WU's fifteen, a rate already in mm/h, no key
	 * and no terms broken to read it -- and it replaces the one band
	 * whose WU endpoint is exercised by none of WU's own pages, which
	 * sec 2.6.4 flags as the least safe thing in the project.
	 *
	 * A failure here is not fatal and not even reported as a band
	 * failure: the WU nowcast is requested instead, so the graph
	 * degrades to what it drew before rather than losing the band.
	 */
	connect(m_met, &bbq_met_client::ready, this,
	        [this](const QJsonDocument &document) {
		bbq_series series = bbq_met_read_nowcast(document);

		if (!series.is_empty()) {
			series.set_fetched_utc(QDateTime::currentSecsSinceEpoch());
			m_composite.set_series(std::move(series));
			emit updated();
		}

		finish_one();
	});

	/*
	 * A radar failure is silent. The band is a bonus where MET reaches
	 * and absent elsewhere, so announcing it as a failed band would put
	 * a permanent error on the display of everybody outside its
	 * coverage. The ordinary nowcast is fetched independently and is
	 * unaffected.
	 */
	connect(m_met, &bbq_met_client::failed, this, [this](const QString &) {
		finish_one();
	});

	connect(m_client, &bbq_wu_client::ready, this,
	        [this](bbq_wu_product product, const QJsonDocument &document) {
		bbq_series series = read_for(product, document);
		series.set_fetched_utc(QDateTime::currentSecsSinceEpoch());

		const bool was_observed = product == bbq_wu_product::observed;
		const bool need_geocode = !m_have_geocode;

		m_composite.set_series(std::move(series));

		/*
		 * The station supplies the coordinate the forecast bands need
		 * (sec 2.6.7.1), so they wait for it rather than for a lookup.
		 */
		if (was_observed && need_geocode) {
			const QJsonArray rows = document.object()
			                                .value(QStringLiteral("observations"))
			                                .toArray();
			if (!rows.isEmpty()) {
				const QJsonObject first = rows.first().toObject();
				if (first.contains(QStringLiteral("lat"))) {
					m_latitude = first.value(QStringLiteral("lat")).toDouble();
					m_longitude = first.value(QStringLiteral("lon")).toDouble();
					m_have_geocode = true;
					start_forecast_bands();
				}
			}
		}

		emit updated();
		finish_one();
	});

	connect(m_client, &bbq_wu_client::failed, this,
	        [this](bbq_wu_product product, const QString &reason) {
		const QString name = QString::fromLatin1(bbq_wu_product_name(product));
		emit band_failed(name, reason);
		finish_one();
	});
}

void bbq_wu_feed::start_auto_refresh() {
	if (m_timer != nullptr) {
		return;
	}

	m_timer = new QTimer(this);
	connect(m_timer, &QTimer::timeout, this, &bbq_wu_feed::tick);

	/*
	 * A one-minute heartbeat that decides nothing on its own -- every
	 * band's own interval is checked against the clock, so the tick
	 * rate only bounds how late a refresh can be, never how often one
	 * happens.
	 */
	m_timer->start(60 * 1000);
}

void bbq_wu_feed::stop_auto_refresh() {
	if (m_timer != nullptr) {
		m_timer->stop();
	}
}

bool bbq_wu_feed::due(bbq_wu_product product, qint64 now_utc) const {
	const qint64 last = m_attempted.value(static_cast<int>(product), 0);
	if (last == 0) {
		return true;
	}

	return now_utc - last >= freshness_s(product);
}

void bbq_wu_feed::attempt(bbq_wu_product product, qint64 now_utc) {
	m_attempted.insert(static_cast<int>(product), now_utc);
	++m_outstanding;

	switch (product) {
	case bbq_wu_product::observed: {
		const QString stamp = QStringLiteral("yyyyMMdd");
		const QString today = QDate::currentDate().toString(stamp);
		m_client->fetch_observed(m_station_id, today);
		break;
	}
	case bbq_wu_product::current_station:
		m_client->fetch_current_station(m_station_id);
		break;
	case bbq_wu_product::current_point:
		m_client->fetch_current_point(m_latitude, m_longitude);
		break;
	case bbq_wu_product::nowcast:
		m_client->fetch_nowcast(m_latitude, m_longitude);
		break;
	case bbq_wu_product::hourly:
		m_client->fetch_hourly(m_latitude, m_longitude);
		break;
	}
}

void bbq_wu_feed::tick() {
	/*
	 * Never overlap. A slow round must not have a second one stacked on
	 * top of it, and skipping costs at most one minute.
	 */
	if (m_outstanding > 0) {
		return;
	}

	const qint64 now = QDateTime::currentSecsSinceEpoch();

	if (!m_station_id.isEmpty()) {
		if (due(bbq_wu_product::observed, now)) {
			attempt(bbq_wu_product::observed, now);
		}
		if (due(bbq_wu_product::current_station, now)) {
			attempt(bbq_wu_product::current_station, now);
		}
	}

	if (m_have_geocode) {
		if (due(bbq_wu_product::nowcast, now)) {
			attempt(bbq_wu_product::nowcast, now);
		}
		if (due(bbq_wu_product::hourly, now)) {
			attempt(bbq_wu_product::hourly, now);
		}
		if (m_station_id.isEmpty() && due(bbq_wu_product::current_point, now)) {
			attempt(bbq_wu_product::current_point, now);
		}
	}
}

void bbq_wu_feed::set_station(const QString &station_id) {
	m_station_id = station_id;
}

void bbq_wu_feed::set_geocode(double latitude, double longitude) {
	m_latitude = latitude;
	m_longitude = longitude;
	m_have_geocode = true;
}

void bbq_wu_feed::finish_one() {
	--m_outstanding;
	if (m_outstanding <= 0) {
		m_outstanding = 0;
		emit settled();
	}
}

void bbq_wu_feed::start_forecast_bands() {
	m_outstanding += 3;
	m_met->fetch_nowcast(m_latitude, m_longitude);
	m_client->fetch_nowcast(m_latitude, m_longitude);
	m_client->fetch_hourly(m_latitude, m_longitude);
}

void bbq_wu_feed::refresh() {
	if (m_outstanding > 0) {
		return;
	}

	if (m_station_id.isEmpty() && !m_have_geocode) {
		emit band_failed(QStringLiteral("all"),
		                 tr("no station pinned and no geocode set"));
		emit settled();
		return;
	}

	if (!m_station_id.isEmpty()) {
		m_outstanding += 2;
		const QString today =
		        QDate::currentDate().toString(QStringLiteral("yyyyMMdd"));
		m_client->fetch_observed(m_station_id, today);
		m_client->fetch_current_station(m_station_id);
	}

	if (m_have_geocode) {
		start_forecast_bands();

		/*
		 * Only without a station: the station's current endpoint
		 * carries a real rain rate where this one carries none
		 * (sec 3.9.2).
		 */
		if (m_station_id.isEmpty()) {
			m_outstanding += 1;
			m_client->fetch_current_point(m_latitude, m_longitude);
		}
	}
}
