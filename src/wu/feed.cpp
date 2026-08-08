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
#include "openmeteo/forecast.h"
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

/*
 * The two bands that are not Weather Underground's, and their own
 * intervals. MET republishes its radar nowcast about every five minutes
 * and Open-Meteo's extended band is hourly, so these match the same rule
 * the table above follows: ask no faster than the source changes.
 */
const int radar_freshness_s = 10 * 60;
const int extended_freshness_s = 60 * 60;

/*
 * Never attempted counts as overdue. Shared by every band so that the
 * one thing they all have to agree about is written once.
 */
bool overdue(qint64 last_utc, int interval_s, qint64 now_utc) {
	return last_utc == 0 || now_utc - last_utc >= interval_s;
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

	/*
	 * Every request gets a deadline, and without one there was no way
	 * back from a stalled connection.
	 *
	 * A reply that never finishes never calls finish_one, so
	 * m_outstanding never returns to zero -- and tick() refuses to start
	 * a round while anything is outstanding, by design, so that one
	 * stalled socket killed auto-refresh for the life of the process.
	 * The graph froze, the tray went red at two hours and stayed red,
	 * and nothing recovered short of a restart. Measured against a
	 * server that accepts and never answers: forty-five seconds, six
	 * requests, not one completion.
	 *
	 * Thirty seconds is long enough for a slow mobile link -- which
	 * sec 11 makes a real case rather than a hypothetical one -- and a
	 * timeout arrives as an ordinary reply error, so every band's
	 * existing failure path already handles it.
	 *
	 * Set on the manager rather than per request: all four providers
	 * share this one, so this is the only place it can be said once.
	 */
	m_net->setTransferTimeout(30 * 1000);
	m_keys = new bbq_wu_key_source(m_net, this);
	m_client = new bbq_wu_client(m_net, m_keys, this);
	m_met = new bbq_met_client(m_net, this);
	m_open = new bbq_openmeteo_client(m_net, this);

	/*
	 * Open-Meteo covers the ground neither other provider reaches at a
	 * useful resolution -- quarter-hourly out to a week, where the
	 * choice was previously hourly or nothing (sec 2.10). Silent on
	 * failure for the same reason the radar band is: it sharpens bands
	 * that already have a source.
	 */
	connect(m_open, &bbq_openmeteo_client::ready, this,
	        [this](const QJsonDocument &document) {
		bbq_series series = bbq_openmeteo_read(document);

		if (!series.is_empty()) {
			series.set_fetched_utc(QDateTime::currentSecsSinceEpoch());
			m_composite.set_series(std::move(series));
			emit updated();
		}

		finish_one();
	});

	connect(m_open, &bbq_openmeteo_client::failed, this,
	        [this](const QString &) { finish_one(); });

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
					emit geocode_derived(m_latitude, m_longitude);
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
	if (m_timer == nullptr) {
		m_timer = new QTimer(this);
		connect(m_timer, &QTimer::timeout, this, &bbq_wu_feed::tick);
	}

	/*
	 * Restartable. The earlier shape returned early whenever the timer
	 * existed, so a stop could never be undone -- latent rather than
	 * live, since nothing calls stop today, but a one-way switch is not
	 * what the pair of names promises.
	 */
	if (m_timer->isActive()) {
		return;
	}

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
	return overdue(last, freshness_s(product), now_utc);
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
		if (overdue(m_radar_attempted, radar_freshness_s, now)) {
			attempt_radar(now);
		}
		if (overdue(m_extended_attempted, extended_freshness_s, now)) {
			attempt_extended(now);
		}
	}
}

void bbq_wu_feed::set_station(const QString &station_id) {
	if (station_id == m_station_id) {
		return;
	}

	m_station_id = station_id;

	/*
	 * A coordinate derived from the OLD station does not describe this
	 * one, so it goes (sec 2.6.7.4).
	 *
	 * Keeping it was worse than stale. refresh() would fire the forecast
	 * bands at the previous station's garden while the observed band
	 * read the new one -- the two-places-on-one-axis failure sec 2.6.7
	 * exists to prevent -- and because a geocode was still held, the
	 * observed handler's "derive one" branch never ran, so the new
	 * station's coordinate was never learned and never re-cached. The
	 * settings layer already dropped its copy on the same edit; this is
	 * the in-memory half of that.
	 *
	 * A pinned coordinate survives, because it was never the station's
	 * to begin with.
	 */
	if (!m_geocode_pinned) {
		m_have_geocode = false;
	}
}

void bbq_wu_feed::set_geocode(double latitude, double longitude, bool pinned) {
	m_latitude = latitude;
	m_longitude = longitude;
	m_have_geocode = true;
	m_geocode_pinned = pinned;
}

void bbq_wu_feed::finish_one() {
	--m_outstanding;
	if (m_outstanding <= 0) {
		m_outstanding = 0;
		emit settled();
	}
}

void bbq_wu_feed::attempt_radar(qint64 now_utc) {
	m_radar_attempted = now_utc;
	++m_outstanding;
	m_met->fetch_nowcast(m_latitude, m_longitude);
}

void bbq_wu_feed::attempt_extended(qint64 now_utc) {
	m_extended_attempted = now_utc;
	++m_outstanding;
	m_open->fetch(m_latitude, m_longitude);
}

/*
 * Every fetch goes through an attempt, and an attempt is what records
 * the time. Firing a request without recording it leaves the band
 * looking as though it had never been asked for, so the next heartbeat
 * asks again -- which is exactly what used to happen sixty seconds
 * after every launch.
 */
void bbq_wu_feed::start_forecast_bands() {
	const qint64 now = QDateTime::currentSecsSinceEpoch();

	attempt_radar(now);
	attempt_extended(now);
	attempt(bbq_wu_product::nowcast, now);
	attempt(bbq_wu_product::hourly, now);
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

	const qint64 now = QDateTime::currentSecsSinceEpoch();

	if (!m_station_id.isEmpty()) {
		attempt(bbq_wu_product::observed, now);
		attempt(bbq_wu_product::current_station, now);
	}

	if (m_have_geocode) {
		start_forecast_bands();

		/*
		 * Only without a station: the station's current endpoint
		 * carries a real rain rate where this one carries none
		 * (sec 3.9.2).
		 */
		if (m_station_id.isEmpty()) {
			attempt(bbq_wu_product::current_point, now);
		}
	}
}
