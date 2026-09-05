#ifndef BBQ_STORE_HISTORY_H
#define BBQ_STORE_HISTORY_H

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <vector>

#include "model/series.h"

/*
 * The permanent record, and the forecast queue that is not permanent
 * (project.md sec 12).
 *
 * Observations are kept for ever because they are the only thing here
 * that cannot be fetched again. Forecasts are kept only until the
 * observation for the time they predicted arrives, at which point they
 * contribute one error to the statistics and are deleted -- which is
 * what stops a permanent store from growing without limit.
 */

/*
 * A lead time, bucketed. A one-hour prediction and a ten-day one are
 * not the same claim (sec 12.3), so every statistic is stratified by
 * this and never averaged across it.
 *
 * The buckets are named by their UPPER bound, and the last one catches
 * everything beyond the second week.
 */
enum class bbq_lead_bucket {
	hour,
	three_hours,
	six_hours,
	twelve_hours,
	day,
	two_days,
	four_days,
	week,
	beyond,
};

bbq_lead_bucket bbq_lead_bucket_for(qint64 lead_s);
const char *bbq_lead_bucket_name(bbq_lead_bucket bucket);

/*
 * A lead time that stands for the whole bucket, used when a bias has to
 * be a smooth function of lead rather than a step (project.md sec 12.9).
 * The middle of the bucket's range, and an arbitrary but reasonable
 * distance into the open-ended last one.
 */
qint64 bbq_lead_bucket_centre_s(bbq_lead_bucket bucket);

/*
 * A weather station the program has heard of (project.md sec 13).
 *
 * Discovered from a coordinate or a place search and then KEPT, so the
 * list a user picks from grows rather than being re-derived every time
 * they move. Distance is from wherever it was last discovered and is a
 * hint for ordering, not an identity: the same station found from two
 * places is one row.
 */
struct bbq_station {
	QString id;
	QString name;
	double latitude = 0.0;
	double longitude = 0.0;
	double distance_km = -1.0;

	/*
	 * Pinned stations are FETCHED, and sparingly: the backfill only,
	 * on its six-hour interval, which is four requests a day each and
	 * exactly what verification needs. The watched station -- the one
	 * being looked at -- is fetched at the ordinary cadence and is a
	 * separate idea from this flag.
	 */
	bool pinned = false;

	qint64 first_seen_utc = 0;
	qint64 last_seen_utc = 0;
};

/*
 * What the store knows about one band's error at one lead time, for one
 * quantity. Sums rather than samples, so the table is a fixed size
 * however many years pass (sec 12.1).
 */
struct bbq_verification {
	int count = 0;

	/*
	 * Signed, so this is the mean error -- the bias, and the "deviation
	 * factor" the correction is built from.
	 */
	double bias = 0.0;

	/*
	 * Unsigned, and NOT decoration. Bias can sit at zero while a
	 * forecast is wildly wrong in both directions, so a store that
	 * recorded only bias would report a useless forecast as a perfect
	 * one (sec 12.3).
	 */
	double mean_absolute_error = 0.0;
	double root_mean_square_error = 0.0;
};

/*
 * One probability bin of a reliability curve (project.md sec 12.4).
 *
 * The question it answers is the one a percentage forecast can actually
 * be held to: of all the times this band said forty percent, how often
 * did it rain? A well-calibrated forecaster's bins land on the diagonal.
 */
struct bbq_reliability_bin {
	int probability_bin = 0;
	int count = 0;
	int rain_count = 0;

	/* The bin's nominal probability, 0..1, and what actually happened. */
	double forecast() const { return probability_bin / 10.0; }
	double observed() const {
		return count > 0 ? static_cast<double>(rain_count) / count : 0.0;
	}
};

/*
 * The Brier score for a band at a lead time, with the reference it has
 * to be read against.
 *
 * A raw Brier score means nothing on its own: 0.1 is excellent in a dry
 * climate and poor in a changeable one. The baseline is the score a
 * forecaster gets by ignoring the weather and always predicting the
 * observed base rate, and the skill is how much better than that this
 * band did -- zero being no better than knowing nothing, and one being
 * perfect.
 */
struct bbq_brier {
	int count = 0;
	double score = 0.0;
	double baseline = 0.0;
	double base_rate = 0.0;

	double skill() const {
		return baseline > 0.0 ? 1.0 - (score / baseline) : 0.0;
	}
};

class bbq_history {
public:
	bbq_history();
	~bbq_history();

	/*
	 * Opens, and creates the schema if it is not there. `path` is for
	 * tests; the empty default puts the file beside the other
	 * application data, which is NOT where the INI lives -- settings are
	 * preferences and this is measurement (sec 12.2).
	 */
	bool open(const QString &path = QString());

	/*
	 * Fold the write-ahead log back into the database file.
	 *
	 * WAL means a committed row can live entirely in `history.sqlite-wal`
	 * with the main file holding almost nothing -- measured on a phone at
	 * 4096 bytes of database against 3.9 MB of log, every observation in
	 * the log. SQLite is perfectly happy with that and reads the pair as
	 * one, so nothing is at risk while both files stay together.
	 *
	 * What it costs is that the database file ALONE is not the archive,
	 * and everything that copies one file thinks it is: a backup, a file
	 * manager, a pull off a device. Such a copy is silently empty rather
	 * than obviously broken, which is the worst way for it to fail
	 * (project.md sec 16.8).
	 *
	 * So this runs when the application stops being looked at, leaving
	 * the file self-contained between sessions.
	 */
	bool checkpoint();
	bool is_open() const { return m_open; }
	QString location() const { return m_path; }
	QString last_error() const { return m_last_error; }

	/*
	 * The known stations (sec 13).
	 *
	 * Remembering is an upsert that PRESERVES the pinned flag, because
	 * discovery happens repeatedly -- every time a coordinate moves --
	 * and a rediscovery must not quietly unpin something the user
	 * chose. The name and coordinate are refreshed; `first_seen_utc` is
	 * not.
	 */
	bool remember_station(const bbq_station &station);

	/*
	 * Where discovery last ran, or false when it never has.
	 *
	 * Kept beside the station list rather than in the INI because it is
	 * a record of what was found and not a preference, and because an
	 * origin that disagreed with the list beside it would decline a
	 * discovery the list needed (sec 15.7.4).
	 */
	bool discovery_origin(double *latitude, double *longitude) const;
	bool set_discovery_origin(double latitude, double longitude,
	                          qint64 ran_utc);
	bool set_station_pinned(const QString &id, bool pinned);

	/*
	 * Nearest first where a distance is known, then the rest by name, so
	 * a list built from one place still reads sensibly from another.
	 */
	std::vector<bbq_station> stations() const;
	std::vector<bbq_station> pinned_stations() const;

	/* Every measurement in a series is archived. Re-storing is harmless. */
	int record_observations(const QString &station, const bbq_series &series);

	/*
	 * Queue a forecast for checking. One is kept per band, per valid
	 * time, per lead bucket -- the first seen -- so re-forecasting the
	 * same hour on every refresh does not store it hundreds of times
	 * (sec 12.6).
	 */
	int record_forecast(const QString &station, const bbq_series &series,
	                    qint64 issued_utc);

	/*
	 * Every station with something queued, whether or not anybody is
	 * watching it.
	 *
	 * verify() and expire() are per station, so somebody has to say
	 * which. Asking the queue itself is the only answer that cannot go
	 * stale: a station stops appearing here the moment its last row is
	 * scored or dropped, and one that was watched last month still
	 * appears while its forecasts are outstanding (sec 14.5).
	 */
	QStringList stations_with_pending() const;

	/*
	 * Check what can be checked: every queued forecast whose valid time
	 * now has an observation. Returns how many were verified. Each one
	 * folds into the statistics and is then deleted.
	 */
	int verify(const QString &station);

	/*
	 * Drop queued forecasts whose valid time is far enough past that no
	 * observation is coming. Without this every outage leaks rows for
	 * ever (sec 12.6).
	 */
	int expire(const QString &station, qint64 now_utc);

	/*
	 * Write a verification row directly, replacing whatever was there.
	 *
	 * For seeding a scratch store so the correction can be exercised
	 * before a month of real weather has gone by, and for any future
	 * import. It is NOT how the applet accumulates anything -- verify()
	 * is -- and whoever calls this is responsible for not aiming it at
	 * the real archive.
	 */
	bool set_verification(const QString &station, bbq_band band,
	                      const QString &quantity, bbq_lead_bucket bucket,
	                      int count, double bias, double mean_absolute_error,
	                      double root_mean_square_error);

	/* Diagnostic and import, as set_verification above. */
	bool set_reliability(const QString &station, bbq_band band,
	                     bbq_lead_bucket bucket, int probability_bin, int count,
	                     int rain_count, double sum_square_error);

	bbq_brier brier(const QString &station, bbq_band band,
	                bbq_lead_bucket bucket) const;

	std::vector<bbq_reliability_bin> reliability(const QString &station,
	                                             bbq_band band,
	                                             bbq_lead_bucket bucket) const;

	bbq_verification verification(const QString &station, bbq_band band,
	                              const QString &quantity,
	                              bbq_lead_bucket bucket) const;

	/* Read a window back out. This is what panning into the past reads. */
	bbq_series observations(const QString &station, qint64 from,
	                        qint64 to) const;

	qint64 earliest_observation(const QString &station) const;

	/*
	 * The newest observation for `station` inside [from, to), or 0 when
	 * the store holds none in that window.
	 *
	 * It exists so a day's completeness can be asked BEFORE a request is
	 * spent on it (sec 15.7.1). The same question was already being
	 * answered after the reply arrived, which is too late to decline the
	 * fetch -- and the archive upserts, so re-fetching a day it already
	 * holds whole changes no row and leaves no trace that it happened.
	 */
	qint64 newest_observation(const QString &station, qint64 from,
	                          qint64 to) const;
	int observation_count(const QString &station) const;

	/*
	 * How many scored samples this station has, across every band, lead
	 * and quantity.
	 *
	 * Asked so the record line can tell "nothing has been checked yet"
	 * apart from "nothing has been checked AT THIS LEAD yet"
	 * (sec 16.14). The two look identical from the window's own bucket
	 * and are opposite answers to somebody wondering whether the
	 * archive is doing anything.
	 */
	int verified_count(const QString &station) const;
	int pending_count(const QString &station) const;

	/*
	 * The same, for one band. Distinguishing them matters because the
	 * corrected band is queued alongside the raw ones (sec 12.19), and
	 * a total cannot say which of them arrived.
	 */
	int pending_count(const QString &station, bbq_band band) const;

private:
	bool exec(const QString &statement);
	bool create_schema();

	bool m_open = false;
	QString m_path;
	QString m_connection;
	mutable QString m_last_error;
};

#endif
