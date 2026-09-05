#include "wu/feed.h"

#include <QDate>
#include <QDateTime>

#include <cmath>
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
#include "model/correction.h"

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

	/*
	 * Discovery is not on this table (sec 13). It is asked for when the
	 * coordinate moves or when somebody searches, not on a heartbeat,
	 * and due() is never consulted for it. A long interval here is a
	 * backstop rather than a schedule.
	 */
	case bbq_wu_product::nearby:
	case bbq_wu_product::place_search:
		return 24 * 3600;

	/*
	 * Pinned history keeps its own schedule, per station, tracked in
	 * m_pinned_attempted rather than by product (sec 14.4). due() is
	 * never consulted for it -- one interval shared by every pinned
	 * station would let the first one fetched suppress the rest.
	 */
	case bbq_wu_product::observed_pinned:
		return 6 * 3600;
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
 * Yesterday's observations, asked for on their own schedule (sec 12.13).
 *
 * Weather Underground's PWS history is an ARCHIVE, and today is not in
 * it yet: asking for today returns the first couple of rows of the
 * local day and nothing since, whatever the hour. Measured against the
 * same station on the same afternoon --
 *
 *     date=today       2 samples, 00:04..00:15 local
 *     date=yesterday   288 samples, a full day at five minutes
 *
 * -- which is the 288 rows a day the client's own comment promises.
 *
 * Six hours because yesterday cannot change. Four requests a day is
 * enough to catch up after a restart and to notice a midnight, and the
 * rule this project follows is to ask no faster than the source moves.
 */
const int backfill_freshness_s = 6 * 3600;

/*
 * How far short of a finished day's end its last observation may fall
 * before the day is called incomplete (sec 12.13.1). Three hours is
 * wider than any ordinary reporting cadence and far narrower than the
 * 17-hour hole that made the check necessary.
 */
const int backfill_short_s = 3 * 3600;

/*
 * How far a fix must land from the last discovery before the station
 * list can have changed (sec 15.7.4).
 *
 * One kilometre, and the number comes from the fix rather than from the
 * stations: the locator asks for NonSatellitePositioningMethods on
 * purpose, so a cell-tower fix carries error of this order itself. A
 * tighter threshold would be tripped by the noise in two readings taken
 * without moving at all, which is the failure this exists to stop.
 */
const double discovery_move_km = 1.0;

/*
 * And discovery runs on the calendar as well, however still the reader
 * has been (sec 16.20).
 *
 * The movement gate answers "can the list have changed because I
 * moved", and that is not the only way it changes: a neighbour puts up
 * a station, and somebody who never leaves their garden would never
 * hear of it. A week is chosen so the cost is one request in seven days
 * against a scraped key -- unmeasurable beside the ordinary cadence --
 * while the list cannot go stale for ever.
 */
const qint64 discovery_stale_s = 7 * 24 * 3600;

/*
 * Great-circle distance. Written here because nothing else in the tree
 * computes one -- `distance_km` on a station is Weather Underground's
 * own number, measured from the point THEY were asked about, which is
 * the question this is not asking.
 */
double km_between(double from_latitude, double from_longitude,
                  double to_latitude, double to_longitude) {
	const double radius_km = 6371.0;
	const double to_radians = M_PI / 180.0;

	const double lat1 = from_latitude * to_radians;
	const double lat2 = to_latitude * to_radians;
	const double dlat = (to_latitude - from_latitude) * to_radians;
	const double dlon = (to_longitude - from_longitude) * to_radians;

	const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
	                 std::cos(lat1) * std::cos(lat2) * std::sin(dlon / 2) *
	                         std::sin(dlon / 2);

	return radius_km * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

/*
 * How far short of the day's end its newest observation falls.
 *
 * Shared by the skip that DECLINES a fetch (sec 15.7.1) and the
 * complaint that a reply left a hole, so the two cannot drift into
 * disagreeing about what whole means -- a skip that declined a day the
 * complaint would then call short would be a hole nobody could fill.
 */
qint64 day_short_by(qint64 newest_utc, const QDate &day) {
	const qint64 ends =
	        QDateTime(day.addDays(1), QTime(0, 0)).toSecsSinceEpoch();
	return ends - newest_utc;
}

/*
 * How far behind the clock a still-running day's newest observation may
 * fall before the station is called quiet (sec 12.13.3). Measured with
 * the encoding fix in place, a healthy station is about three minutes
 * behind; forty-five is well clear of that and well inside the
 * seventy-eight minutes that went unremarked.
 */
const int station_quiet_s = 45 * 60;

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

	/*
	 * Never reached: the client diverts discovery answers to their own
	 * signals precisely so they cannot arrive here as a band. Listed so
	 * that adding a product still fails the switch rather than falling
	 * through to an empty series, which would be a band that silently
	 * never draws.
	 */
	case bbq_wu_product::observed_pinned:
		return bbq_wu_read_observed(document);

	case bbq_wu_product::nearby:
	case bbq_wu_product::place_search:
		break;
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
			m_history.record_forecast(m_station_id, series,
			                          QDateTime::currentSecsSinceEpoch());
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
			m_history.record_forecast(m_station_id, series,
			                          QDateTime::currentSecsSinceEpoch());
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

	/*
	 * Discovery lands in the STORE, not in the composite (sec 13). A
	 * station list is not weather: nothing on the graph changes, and
	 * what the user gains is a list to choose from that outlives this
	 * run.
	 */
	connect(m_client, &bbq_wu_client::stations_ready, this,
	        [this](const QJsonDocument &document) {
		const qint64 now = QDateTime::currentSecsSinceEpoch();
		const std::vector<bbq_wu_nearby> found = bbq_wu_read_nearby(document);

		int kept = 0;
		for (const bbq_wu_nearby &one : found) {
			bbq_station station;
			station.id = one.id;
			station.name = one.name;
			station.latitude = one.latitude;
			station.longitude = one.longitude;
			station.distance_km = one.distance_km;
			station.first_seen_utc = now;
			station.last_seen_utc = now;

			if (m_history.remember_station(station)) {
				++kept;
			}
		}

		/*
		 * The origin moves only now, on an answer -- see the members'
		 * comment. `kept` is not required to be non-zero: a coordinate
		 * with genuinely no stations near it is a real answer, and
		 * re-asking it on every fix is the waste this exists to stop.
		 */
		if (m_discovery_outstanding) {
			m_history.set_discovery_origin(m_discovery_latitude,
			                               m_discovery_longitude, now);
			m_discovery_outstanding = false;
		}

		emit stations_discovered(kept);
		finish_one();
	});

	connect(m_client, &bbq_wu_client::places_ready, this,
	        [this](const QJsonDocument &document) {
		emit places_found(bbq_wu_read_places(document));
		finish_one();
	});

	connect(m_client, &bbq_wu_client::ready, this,
	        [this](bbq_wu_product product, const QJsonDocument &document) {
		/*
		 * A pinned station's history goes to the STORE and nowhere
		 * else (sec 14.4). It is not this location's weather: putting
		 * it in the composite would draw another town's measurements
		 * on the graph.
		 */
		if (product == bbq_wu_product::observed_pinned) {
			const QString whose = m_pinned_in_flight;
			m_pinned_in_flight.clear();

			if (!whose.isEmpty()) {
				bbq_series measured = read_for(product, document);
				if (!measured.is_empty()) {
					note_partial_store(
					        int(measured.size()),
					        m_history.record_observations(whose, measured));
				}
			}

			finish_one();
			dispatch_pinned();
			return;
		}

		bbq_series series = read_for(product, document);
		series.set_fetched_utc(QDateTime::currentSecsSinceEpoch());

		const bool was_observed = product == bbq_wu_product::observed;
		const bool need_geocode = !m_have_geocode;

		/*
		 * Into the store on the way past (sec 12).
		 *
		 * Only the observed band is archived as measurement, and the
		 * `current` band deliberately is NOT -- which is a correctness
		 * matter rather than an omission. A current reading carries the
		 * declared validity of sec 3.9, and storing it with that span
		 * would put a band of priority 300 across minutes that were
		 * never measured, overruling the forecasts that sec 3.3 ranks
		 * above it precisely so its extension stays harmless. Nothing
		 * is lost: the station's own history reports the same reading
		 * on the next observed fetch, with an honest duration.
		 */
		if (was_observed) {
			check_day_is_whole(series);
			note_partial_store(
			        int(series.size()),
			        m_history.record_observations(m_station_id, series));
			m_observed_fetched_utc = QDateTime::currentSecsSinceEpoch();
		} else if (product != bbq_wu_product::current_station &&
		           product != bbq_wu_product::current_point) {
			m_history.record_forecast(m_station_id, series,
			                          QDateTime::currentSecsSinceEpoch());
		}

		m_composite.set_series(std::move(series));

		if (was_observed) {
			/* Replaces what was just set, from the store's fuller answer. */
			load_observations();
		}

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
		/*
		 * A pinned station that fails releases the queue (sec 14.4).
		 *
		 * Without this the slot stays occupied for the life of the
		 * process and every other pinned station waits behind a request
		 * that already ended -- the same shape as the stalled socket
		 * that killed auto-refresh, in a smaller room. It is also not a
		 * band failure: nothing on the display depends on it, and
		 * announcing it would put another station's trouble on the
		 * watched station's status line.
		 */
		if (product == bbq_wu_product::observed_pinned) {
			m_pinned_in_flight.clear();
			finish_one();
			dispatch_pinned();
			return;
		}

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

	/*
	 * Discovery is requested directly rather than through attempt(),
	 * because it is driven by the coordinate moving or by somebody
	 * typing, not by a schedule. Reaching here would have incremented
	 * m_outstanding above and then sent nothing, which is the leak that
	 * stops the heartbeat for the life of the process.
	 */
	/*
	 * Neither is scheduled through attempt(): discovery is driven by a
	 * move or by typing, and pinned history is dispatched one station
	 * at a time by its own queue (sec 14.4). Reaching here would have
	 * incremented m_outstanding and sent nothing.
	 */
	case bbq_wu_product::nearby:
	case bbq_wu_product::place_search:
	case bbq_wu_product::observed_pinned:
		--m_outstanding;
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
		if (overdue(m_backfill_attempted, backfill_freshness_s, now)) {
			attempt_backfill(now);
		}

		/*
		 * Pinned stations, asked for on the heartbeat rather than at
		 * startup: a launch already has everything else outstanding,
		 * and these are the least urgent thing the program fetches.
		 */
		queue_pinned(now);

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

	/*
	 * The backfill is due again, whatever the clock says (sec 12.13).
	 *
	 * Its six-hour interval is a statement about the DATA -- yesterday
	 * cannot change -- and that reasoning does not survive the station
	 * changing, because it is a different station's yesterday. Left
	 * alone, a station edit inside the window would fetch today's two
	 * rows for the new station and none of the day behind it, so
	 * verification for it would start a day late and look like the
	 * starvation of sec 12.13 all over again.
	 *
	 * Changing the station is exactly when somebody most wants the new
	 * one to have a history: it is what you do after discovering the
	 * old one was reporting rubbish.
	 */
	m_backfill_attempted = 0;

	/*
	 * AND ITS MEASUREMENTS GO WITH IT (sec 14.8).
	 *
	 * The argument above is about a coordinate and applies unchanged to
	 * data: observations belonging to the old station do not describe
	 * this one. Nothing dropped them, so the observed band sat in the
	 * composite until a fetch for the new station happened to replace
	 * it -- and where that fetch fails, which is the ordinary case on a
	 * train, the window never closes. The graph then draws the previous
	 * station's thermometer under the new station's name.
	 *
	 * Worse than stale, for the same reason as the geocode: the band
	 * still carries the OLD fetch time, so sec 2.4's staleness check
	 * reports it healthy. A wrong number that looks fresh outranks
	 * every honest one on the same axis.
	 *
	 * Replaced with an empty series rather than left out, because
	 * present-but-empty is what sec 2.6.6 counts as missing -- so the
	 * display says the observed band is absent, which is true, instead
	 * of saying nothing.
	 *
	 * Only where one is actually held. Setting an empty band on a feed
	 * that never had one would invent an absence to report, and the
	 * first station set at startup is exactly that case.
	 *
	 * EVERY band, not just the observed one (sec 14.8.1). The first
	 * version of this dropped observations alone and left the identical
	 * fault in five others -- including the worst of them, since
	 * `current` is fetched by station id and outranks everything at the
	 * present instant, so a stale one answers "what is it doing now"
	 * with another station's thermometer.
	 *
	 * The forecast bands are fetched by COORDINATE rather than by
	 * station, and they go for a consequential reason rather than the
	 * same one: this function has just dropped the geocode they were
	 * fetched for, so they describe a place the feed no longer claims.
	 * Where the geocode was PINNED it is not dropped, it was never the
	 * station's, and neither are they.
	 */
	std::vector<bbq_band> stale = {bbq_band::observed, bbq_band::current};

	if (!m_geocode_pinned) {
		stale.push_back(bbq_band::nowcast_fine);
		stale.push_back(bbq_band::nowcast);
		stale.push_back(bbq_band::extended);
		stale.push_back(bbq_band::hourly);
	}

	for (bbq_band band : stale) {
		if (m_composite.has_band(band)) {
			m_composite.set_series(bbq_series(band, QString()));
		}
	}

	m_observed_fetched_utc = 0;
	m_loaded_from = 0;
	m_loaded_to = 0;

	/*
	 * AND THIS STATION HAS NEVER BEEN ASKED (sec 14.9).
	 *
	 * The freshness record is kept per PRODUCT, and the question it
	 * answers -- may this be fetched again yet -- is per STATION for
	 * these two. refresh() papers over the difference by asking for
	 * both unconditionally, which is why the gap went unnoticed; but
	 * refresh() declines while a round is outstanding, and changing
	 * station is exactly what somebody does while watching a slow one.
	 * The next heartbeat then finds the OLD station was asked a minute
	 * ago and declines too, so the new station's measurements arrive an
	 * interval late, ruled fresh on the strength of a question about
	 * somewhere else.
	 *
	 * Only the two that are asked for by station id. The coordinate
	 * bands are the geocode's business and are handled where it is
	 * dropped.
	 */
	m_attempted.remove(static_cast<int>(bbq_wu_product::observed));
	m_attempted.remove(static_cast<int>(bbq_wu_product::current_station));
}

void bbq_wu_feed::forget_location_freshness() {
	/*
	 * Only the bands that are asked for BY COORDINATE. The observed and
	 * current-station products are asked for by station id, so a moved
	 * coordinate says nothing about them -- they are reset where the
	 * STATION changes instead, and the backfill has its own reset there
	 * for its own reason.
	 *
	 * This comment used to say they needed no reset at all, on the
	 * grounds that refresh() asks for them unconditionally. That is
	 * true and was not enough: refresh() declines while a round is
	 * outstanding, and sec 14.9 is what that cost.
	 */
	m_attempted.remove(static_cast<int>(bbq_wu_product::nowcast));
	m_attempted.remove(static_cast<int>(bbq_wu_product::hourly));
	m_attempted.remove(static_cast<int>(bbq_wu_product::current_point));

	m_radar_attempted = 0;
	m_extended_attempted = 0;
}

void bbq_wu_feed::set_geocode(double latitude, double longitude, bool pinned) {
	/*
	 * A MOVE invalidates the forecast bands (sec 2.6.7.5).
	 *
	 * Their freshness intervals say how long the weather at a place
	 * stays worth re-reading; they say nothing about a different place.
	 * Without this, changing the station left the graph showing the old
	 * town's forecast until each band's timer ran out on its own -- up
	 * to fifteen minutes for the nowcast and a full HOUR for the hourly
	 * band, while the observed band beside it already described
	 * somewhere else. That is the two-places-on-one-axis failure sec
	 * 2.6.7 exists to prevent, arriving by the clock rather than by the
	 * coordinate.
	 *
	 * Compared before it is stored, and only a real move counts: this is
	 * also called every time a coordinate is DERIVED from an observed
	 * response, which is most fetches, and resetting there would refetch
	 * every band every time.
	 */
	const bool moved = m_have_geocode &&
	                   (m_latitude != latitude || m_longitude != longitude);

	m_latitude = latitude;
	m_longitude = longitude;
	m_have_geocode = true;
	m_geocode_pinned = pinned;

	if (moved) {
		forget_location_freshness();
	}
}

int bbq_wu_feed::record_corrected(qint64 now_utc) {
	if (!m_history.is_open() || m_station_id.isEmpty()) {
		return 0;
	}

	/*
	 * Over the composite's own coverage rather than the view's.
	 *
	 * corrected_forecast() is normally asked for whatever is on screen,
	 * which is right for drawing and wrong for archiving: what gets
	 * stored would then depend on where somebody had dragged the graph.
	 * The forecast horizon is what the bands cover, so that is what is
	 * queued.
	 */
	const qint64 from = qMax(now_utc, m_composite.begin_utc());
	const qint64 to = m_composite.end_utc();

	if (to <= from) {
		return 0;
	}

	const bbq_series corrected = bbq_corrected_forecast(
	        m_composite, m_history, m_station_id, from, to, now_utc);

	if (corrected.is_empty()) {
		return 0;
	}

	return m_history.record_forecast(m_station_id, corrected, now_utc);
}

int bbq_wu_feed::verify_all() {
	if (!m_history.is_open()) {
		return 0;
	}

	/*
	 * DRIVEN BY THE QUEUE, not by what is being watched (sec 14.5).
	 *
	 * Asking the queue who has rows reaches three cases with one rule:
	 * the watched station, a pinned one whose observations are arriving
	 * precisely so that it can be scored, and one that is neither -- a
	 * queue somebody moved away from, which under the old rule was never
	 * scored AND never expired, so it leaked for ever.
	 *
	 * The clock is read once. Reading it per station would let a sweep
	 * that crosses a second expire two stations against two different
	 * cutoffs, which is a difference nobody could ever reproduce.
	 */
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	int checked = 0;

	for (const QString &station : m_history.stations_with_pending()) {
		checked += m_history.verify(station);
		m_history.expire(station, now);
	}

	return checked;
}

void bbq_wu_feed::finish_one() {
	--m_outstanding;
	if (m_outstanding <= 0) {
		m_outstanding = 0;

		/*
		 * Checked when a round settles rather than on a timer of its
		 * own: a round is exactly when new observations have arrived,
		 * so it is the only moment anything new can be verifiable.
		 */
		const int checked = verify_all();

		/*
		 * The correction is queued AFTER the scoring, so it is built
		 * from the statistics this round produced rather than the
		 * previous one's (sec 12.20.4).
		 *
		 * This ran first, on the stated reasoning that the raw bands
		 * and the correction should wait on the same observations. They
		 * do either way -- the raw bands were recorded when their
		 * responses arrived, earlier in the round, and nothing about
		 * this order changes what any of them is scored against. The
		 * reason was wrong and the order it justified cost a round: a
		 * bucket crossing the twenty-pairing minimum during verify_all
		 * produced no correction until the round after.
		 */
		record_corrected(QDateTime::currentSecsSinceEpoch());
		if (checked > 0) {
			emit verified(checked);
		}

		emit settled();
	}
}

void bbq_wu_feed::discover_stations() {
	if (!m_have_geocode) {
		return;
	}

	m_discovery_latitude = m_latitude;
	m_discovery_longitude = m_longitude;
	m_discovery_outstanding = true;

	++m_outstanding;
	m_client->fetch_nearby(m_latitude, m_longitude);
}

void bbq_wu_feed::discover_stations_at(double latitude, double longitude) {
	m_discovery_latitude = latitude;
	m_discovery_longitude = longitude;
	m_discovery_outstanding = true;

	++m_outstanding;
	m_client->fetch_nearby(latitude, longitude);
}

double bbq_wu_feed::moved_since_discovery(double latitude,
                                          double longitude) const {
	double was_latitude = 0.0;
	double was_longitude = 0.0;

	/*
	 * Never discovered, or a store that cannot say. Both read as "must
	 * discover": declining on the strength of not knowing would leave a
	 * fresh install with an empty station list for ever.
	 */
	if (!m_history.discovery_origin(&was_latitude, &was_longitude)) {
		return -1.0;
	}

	return km_between(was_latitude, was_longitude, latitude, longitude);
}

bool bbq_wu_feed::discover_stations_if_moved(double latitude,
                                            double longitude) {
	const double moved = moved_since_discovery(latitude, longitude);

	if (moved >= 0.0 && moved < discovery_move_km) {
		/*
		 * Still here, so ask whether it has simply been long enough.
		 * A zero stamp is an origin written before this was recorded
		 * and reads as "long ago" rather than "just now", which is the
		 * safe direction: one extra discovery, once.
		 */
		const qint64 ran = m_history.discovery_ran_utc();
		const qint64 now = QDateTime::currentSecsSinceEpoch();

		if (ran != 0 && now - ran < discovery_stale_s) {
			return false;
		}
	}

	discover_stations_at(latitude, longitude);
	return true;
}

void bbq_wu_feed::search_places(const QString &query) {
	if (query.trimmed().isEmpty()) {
		return;
	}

	++m_outstanding;
	m_client->fetch_places(query);
}

void bbq_wu_feed::queue_pinned(qint64 now_utc) {
	/*
	 * The decision lives in pinned_worth_fetching, which is public so a
	 * headless test can watch it (sec 15.7.2). This is the wiring: if
	 * the queue stopped being built from that list, the skip would be a
	 * correct function nothing called.
	 */
	for (const QString &id : pinned_worth_fetching()) {
		if (m_pinned_queue.contains(id) || m_pinned_in_flight == id) {
			continue;
		}

		const qint64 last = m_pinned_attempted.value(id, 0);
		if (overdue(last, backfill_freshness_s, now_utc)) {
			m_pinned_queue.append(id);
		}
	}

	dispatch_pinned();
}

void bbq_wu_feed::dispatch_pinned() {
	if (!m_pinned_in_flight.isEmpty() || m_pinned_queue.isEmpty()) {
		return;
	}

	m_pinned_in_flight = m_pinned_queue.takeFirst();
	m_pinned_attempted.insert(m_pinned_in_flight,
	                          QDateTime::currentSecsSinceEpoch());
	++m_outstanding;

	/*
	 * Yesterday, for the reason sec 12.13 gives: today is not in the
	 * archive this endpoint serves. A pinned station is kept for its
	 * history, so today's two rows would be the wrong day to ask for.
	 */
	const QString stamp = QStringLiteral("yyyyMMdd");
	const QString yesterday = QDate::currentDate().addDays(-1).toString(stamp);
	m_client->fetch_observed_pinned(m_pinned_in_flight, yesterday);
}

void bbq_wu_feed::note_partial_store(int given, int stored) {
	/*
	 * THE STORE'S OWN FAILURES WERE INVISIBLE (sec 12.13.2).
	 *
	 * `record_observations` returns how many rows it wrote and every
	 * caller discarded it, while a failed insert set `last_error` that
	 * nothing read after the store was opened. So a write that lost
	 * half a day looked exactly like one that lost nothing: the fetch
	 * succeeded, the band was drawn, and the rows were simply not
	 * there.
	 *
	 * Observations are the case where the two numbers MUST agree --
	 * every sample is an upsert keyed on its own timestamp, so nothing
	 * legitimately collapses. Forecasts are deliberately kept one per
	 * band and bucket, so fewer stored than given is ordinary there and
	 * this is not applied to them.
	 */
	if (!m_history.is_open() || stored == given) {
		return;
	}

	emit band_failed(QStringLiteral("observed"),
	                 tr("the store took %1 of %2 observations: %3")
	                         .arg(stored)
	                         .arg(given)
	                         .arg(m_history.last_error()));
}

void bbq_wu_feed::check_day_is_whole(const bbq_series &measured) {
	/*
	 * A SHORT ANSWER IS NOT AN ERROR, and that is the problem
	 * (sec 12.13.1).
	 *
	 * Sec 2.6.5.1 cost an archive and several hours: a stale cache
	 * variant returned 78 observations where the day held 288, and
	 * nothing could tell. Every band answered, every status was 200, and
	 * 78 rows parse exactly as well as 288 -- so the only evidence was a
	 * store that quietly stopped growing.
	 *
	 * The check is on TIME rather than on count, because stations report
	 * at whatever cadence they like: 96 rows a day is a normal station
	 * and 288 is another, and a threshold on rows would have to know
	 * which. A day that has ended is different -- whatever the cadence,
	 * its last observation should be near its end.
	 *
	 * Reported through band_failed, which is what puts it on the status
	 * line. That line is what finally diagnosed sec 2.6.5.1 after logcat
	 * had been read twice, and a truncated day is exactly the thing it
	 * exists to say out loud.
	 */
	if (measured.is_empty()) {
		return;
	}

	if (!m_backfill_day.isValid()) {
		/*
		 * TODAY'S fetch, where the question is different (sec 12.13.3).
		 *
		 * A day still in progress cannot be short -- it has not
		 * finished -- so what matters is whether the station is still
		 * reporting. Measured on the night this was written:
		 * ISTOCK877's newest observation sat at 02:04 while a
		 * neighbouring station was current to 03:19, and nothing said
		 * so. sec 2.4's staleness check answers "when did we last
		 * FETCH", which stays healthy while a quiet station is fetched
		 * faithfully and returns the same rows every time.
		 */
		const qint64 newest = measured.samples().back().start_utc;
		const qint64 behind = QDateTime::currentSecsSinceEpoch() - newest;

		if (behind > station_quiet_s) {
			emit band_failed(
			        QStringLiteral("observed"),
			        tr("%1 has not reported for %2 minutes")
			                .arg(m_station_id)
			                .arg(behind / 60));
		}

		return;
	}

	const QDate asked = m_backfill_day;
	m_backfill_day = QDate();

	const qint64 last = measured.samples().back().start_utc;
	const qint64 short_by = day_short_by(last, asked);

	if (short_by <= backfill_short_s) {
		return;
	}

	emit band_failed(QStringLiteral("observed"),
	                 tr("%1 returned only %2 rows, ending %3 hours before the "
	                    "day did -- the archive has a hole in it")
	                         .arg(asked.toString(Qt::ISODate))
	                         .arg(measured.size())
	                         .arg(short_by / 3600));
}

QDate bbq_wu_feed::backfill_day_wanted(const QString &station) const {
	const QDate day = QDate::currentDate().addDays(-1);

	/*
	 * Neither of these can be established, and declining on the
	 * strength of not knowing would turn an unopened store into a
	 * silent refusal to ever backfill. So they answer "ask".
	 */
	if (!m_history.is_open() || station.isEmpty()) {
		return day;
	}

	const qint64 begins = QDateTime(day, QTime(0, 0)).toSecsSinceEpoch();
	const qint64 ends =
	        QDateTime(day.addDays(1), QTime(0, 0)).toSecsSinceEpoch();
	const qint64 newest = m_history.newest_observation(station, begins, ends);

	/*
	 * Nothing at all for that day is the case the backfill EXISTS for,
	 * so it is emphatically worth asking.
	 */
	if (newest == 0) {
		return day;
	}

	if (day_short_by(newest, day) <= backfill_short_s) {
		return QDate();
	}

	return day;
}

QStringList bbq_wu_feed::pinned_worth_fetching() const {
	QStringList worth;

	if (!m_history.is_open()) {
		return worth;
	}

	for (const bbq_station &one : m_history.pinned_stations()) {
		/*
		 * The watched station is fetched properly and far more often;
		 * queueing it here as well would spend a request to learn what
		 * it already knows.
		 */
		if (one.id == m_station_id || one.id.isEmpty()) {
			continue;
		}

		/*
		 * The same skip as the watched station's, against the same day:
		 * a pinned station is fetched for yesterday only, so one
		 * already held whole has nothing to ask for (sec 15.7.1). This
		 * is where it costs most -- every pinned station was another
		 * request per launch.
		 */
		if (!backfill_day_wanted(one.id).isValid()) {
			continue;
		}

		worth.append(one.id);
	}

	return worth;
}

void bbq_wu_feed::attempt_backfill(qint64 now_utc) {
	if (m_station_id.isEmpty()) {
		return;
	}

	/*
	 * DECLINE A DAY THE STORE ALREADY HOLDS WHOLE (sec 15.7.1).
	 *
	 * The stamp is still set, so this is not re-asked on every beat.
	 * Without the skip a launch spent a request on yesterday whatever
	 * the archive held, and the archive upserts -- so the re-fetch
	 * changed no row and left no trace that it had happened. The cost
	 * lands on somebody else's quota (sec 2.5), which is the reason it
	 * is worth declining rather than merely tidy.
	 */
	const QDate wanted = backfill_day_wanted(m_station_id);
	if (!wanted.isValid()) {
		m_backfill_attempted = now_utc;
		return;
	}

	m_backfill_attempted = now_utc;
	++m_outstanding;

	/*
	 * The same product as today's request, deliberately. The response
	 * handler archives whatever arrives and then rebuilds the series
	 * from the STORE rather than from the reply, so two answers for two
	 * days accumulate instead of replacing one another -- which is the
	 * property that makes this a two-line change rather than a new band.
	 */
	const QString stamp = QStringLiteral("yyyyMMdd");
	const QDate day = wanted;

	/*
	 * Remembered so the answer can be checked against the question
	 * (sec 12.13.1). A day that has ENDED should be answered with
	 * observations reaching its end, and nothing else knows which day
	 * was asked for by the time the reply arrives.
	 */
	m_backfill_day = day;
	m_client->fetch_observed(m_station_id, day.toString(stamp));
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

		/*
		 * Here as well as in tick(), because tick() refuses to run
		 * while anything is outstanding and a launch has everything
		 * outstanding. A phone app that is opened, looked at and
		 * backgrounded may never reach an idle heartbeat at all, so a
		 * backfill that only happened there would never happen on the
		 * device it matters most on. The interval still governs: a
		 * fresh process has never asked, which is overdue by
		 * definition.
		 */
		if (overdue(m_backfill_attempted, backfill_freshness_s, now)) {
			attempt_backfill(now);
		}
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

bool bbq_wu_feed::open_history(const QString &path) {
	if (!m_history.open(path)) {
		m_history_error = m_history.last_error();
		return false;
	}

	return true;
}

void bbq_wu_feed::set_view_range(qint64 from_utc, qint64 to_utc) {
	m_view_from = from_utc;
	m_view_to = to_utc;

	/*
	 * Reload only when the view has left what is in memory. This is
	 * called on every mouse move of a drag, and a database query per
	 * frame is exactly the kind of thing sec 14.1 is about.
	 */
	if (m_loaded_to > m_loaded_from && from_utc >= m_loaded_from &&
	    to_utc <= m_loaded_to) {
		return;
	}

	load_observations();
}

void bbq_wu_feed::load_observations() {
	if (!m_history.is_open() || m_station_id.isEmpty()) {
		return;
	}

	if (m_view_to <= m_view_from) {
		/* Nothing has said what is being looked at yet. */
		const qint64 now = QDateTime::currentSecsSinceEpoch();
		m_view_from = now - 24 * 3600;
		m_view_to = now + 24 * 3600;
	}

	/*
	 * A margin either side, so a drag crosses loaded ground for a while
	 * before it needs the database again.
	 */
	const qint64 span = m_view_to - m_view_from;
	m_loaded_from = m_view_from - span;
	m_loaded_to = m_view_to + span;

	bbq_series stored =
	        m_history.observations(m_station_id, m_loaded_from, m_loaded_to);

	if (stored.is_empty()) {
		return;
	}

	/*
	 * Stamped with when the band was last FETCHED, not when it was read
	 * back. Reading from disk is not freshness, and sec 2.4's staleness
	 * check would otherwise report a stale band as new every time the
	 * view moved.
	 */
	stored.set_fetched_utc(m_observed_fetched_utc);

	const bbq_series *live = m_composite.band(bbq_band::observed);
	if (live != nullptr) {
		stored.set_zone(live->zone());
	}

	m_composite.set_series(std::move(stored));
}

bbq_series bbq_wu_feed::corrected_forecast(qint64 from_utc,
                                           qint64 to_utc) const {
	return bbq_corrected_forecast(m_composite, m_history, m_station_id,
	                              from_utc, to_utc,
	                              QDateTime::currentSecsSinceEpoch());
}
