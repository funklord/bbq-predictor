#ifndef BBQ_WU_FEED_H
#define BBQ_WU_FEED_H

#include <QHash>
#include <QObject>
#include <QString>

#include "model/composite.h"
#include "store/history.h"
#include "met/nowcast.h"
#include "openmeteo/forecast.h"
#include "wu/client.h"

class QNetworkAccessManager;
class QTimer;
class bbq_wu_key_source;
class bbq_met_client;
class bbq_openmeteo_client;

/*
 * Keeps a bbq_composite fed from Weather Underground.
 *
 * The orchestration the diagnostic does by hand, made reusable and
 * asynchronous: no event loop of its own, so a window can own one and
 * stay responsive while the bands arrive.
 *
 * Bands land independently and the composite is updated as each does,
 * which is what sec 2.6.6 requires -- a dead station must not delay or
 * suppress the forecast bands, and an absent band is reported by name
 * rather than left as a hole.
 */
class bbq_wu_feed : public QObject {
	Q_OBJECT

public:
	explicit bbq_wu_feed(QObject *parent = nullptr);

	/*
	 * The pinned station (sec 2.6.5). Empty is a normal state: the
	 * forecast bands are geocode-keyed and need no station at all.
	 */
	void set_station(const QString &station_id);
	QString station() const { return m_station_id; }

	/*
	 * The forecast point. Derived from the station when one is pinned
	 * and no override is set (sec 2.6.7), so calling this is the
	 * override rather than the normal path.
	 */
	/*
	 * The forecast point. `pinned` says where it came from, and the
	 * distinction is load-bearing rather than informational: a pinned
	 * coordinate was chosen by configuration (an override, or --geocode)
	 * and belongs to no station, while an unpinned one is the cache
	 * derived from whichever station was being read at the time.
	 *
	 * Changing the station therefore discards an unpinned coordinate and
	 * keeps a pinned one. Sec 2.6.7.3.
	 */
	void set_geocode(double latitude, double longitude, bool pinned = true);

	bool has_geocode() const { return m_have_geocode; }

	void refresh();

	/*
	 * Re-fetch each band on its own schedule (project.md sec 2.5).
	 *
	 * Per band rather than one interval for everything, because the
	 * bands do not change at the same speed: the station reports every
	 * five minutes and the fifteen-day hourly forecast does not change
	 * between breakfast and lunch. Refetching all of them on the
	 * fastest of those schedules would be four times the requests for
	 * no extra information -- against somebody else's quota, on a
	 * scraped key, which sec 2.5 is explicit about.
	 */
	/*
	 * Open the permanent store (project.md sec 12). The path is for
	 * tests; empty takes the real one. A feed whose store will not open
	 * still works -- it simply forgets, and says so.
	 */
	bool open_history(const QString &path = QString());
	const bbq_history &history() const { return m_history; }
	QString history_error() const { return m_history_error; }

	/*
	 * What the graph is looking at. The observed band is served from the
	 * store rather than from the last fetch (sec 12.8), so panning into
	 * last month is the same operation as looking at this afternoon.
	 *
	 * Reloads only when the view leaves what is already in memory, and
	 * then takes a margin either side, because this is called on every
	 * mouse move during a drag.
	 */
	void set_view_range(qint64 from_utc, qint64 to_utc);

	/*
	 * The bias-corrected overlay for a range (sec 12.5). Empty until
	 * enough forecasts have been checked to justify moving a curve.
	 */
	bbq_series corrected_forecast(qint64 from_utc, qint64 to_utc) const;

	void start_auto_refresh();
	void stop_auto_refresh();

	const bbq_composite &composite() const { return m_composite; }
	bool is_busy() const { return m_outstanding > 0; }

signals:
	/* A band arrived and the composite changed. */
	void updated();

	/* A band did not arrive. Named, because absent must not read as empty. */
	void band_failed(const QString &band, const QString &reason);

	/* Nothing further is outstanding, however it went. */
	void settled();

	/* How many queued forecasts were checked off in this round. */
	void verified(int count);

	/*
	 * The station said where it is (sec 2.6.7.1), so the derivation can
	 * be cached. Emitted rather than written here: the model does not
	 * know where settings live and should not learn.
	 */
	void geocode_derived(double latitude, double longitude);

private:
	void start_forecast_bands();

	/*
	 * The radar and extended bands are not Weather Underground's and so
	 * are not in bbq_wu_product, which is what the schedule is keyed by.
	 * They carry their own last-attempted stamps rather than being
	 * squeezed into an enum that means something else.
	 */
	/*
	 * Yesterday's observations, which today's request cannot supply
	 * (sec 12.13). Separate because the two ask for different days on
	 * very different schedules: today's answer changes, yesterday's
	 * cannot.
	 */
	/*
	 * Forget how fresh the location-dependent bands are, because the
	 * location has moved (sec 2.6.7.5). Freshness answers "has the
	 * weather here changed since we asked", and "here" has.
	 */
	void forget_location_freshness();

	void attempt_backfill(qint64 now_utc);
	void attempt_radar(qint64 now_utc);
	void attempt_extended(qint64 now_utc);
	void finish_one();
	void tick();
	bool due(bbq_wu_product product, qint64 now_utc) const;
	void attempt(bbq_wu_product product, qint64 now_utc);

	QNetworkAccessManager *m_net;
	bbq_wu_key_source *m_keys;
	bbq_wu_client *m_client;
	bbq_met_client *m_met;
	bbq_openmeteo_client *m_open;

	bbq_composite m_composite;
	QString m_station_id;
	double m_latitude = 0.0;
	double m_longitude = 0.0;
	bool m_have_geocode = false;
	bool m_geocode_pinned = false;
	void load_observations();

	bbq_history m_history;
	QString m_history_error;
	qint64 m_observed_fetched_utc = 0;
	qint64 m_view_from = 0;
	qint64 m_view_to = 0;
	qint64 m_loaded_from = 0;
	qint64 m_loaded_to = 0;

	qint64 m_backfill_attempted = 0;
	qint64 m_radar_attempted = 0;
	qint64 m_extended_attempted = 0;
	int m_outstanding = 0;

	QTimer *m_timer = nullptr;

	/*
	 * When each product was last ATTEMPTED, not when it last succeeded.
	 *
	 * Attempts, because a band whose endpoint is failing would
	 * otherwise be retried on every tick -- its success time never
	 * advances, so it would look permanently due. Backing off on the
	 * band's own interval is the polite behaviour and costs nothing
	 * when everything is working.
	 *
	 * Staleness on the display still reads the SUCCESS times off the
	 * series (sec 2.4), so a band failing quietly still shows as old.
	 */
	QHash<int, qint64> m_attempted;
};

#endif
