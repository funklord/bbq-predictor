#ifndef BBQ_STORE_HISTORY_H
#define BBQ_STORE_HISTORY_H

#include <QString>
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
	bool is_open() const { return m_open; }
	QString location() const { return m_path; }
	QString last_error() const { return m_last_error; }

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

	bbq_verification verification(const QString &station, bbq_band band,
	                              const QString &quantity,
	                              bbq_lead_bucket bucket) const;

	/* Read a window back out. This is what panning into the past reads. */
	bbq_series observations(const QString &station, qint64 from,
	                        qint64 to) const;

	qint64 earliest_observation(const QString &station) const;
	int observation_count(const QString &station) const;
	int pending_count(const QString &station) const;

private:
	bool exec(const QString &statement);
	bool create_schema();

	bool m_open = false;
	QString m_path;
	QString m_connection;
	mutable QString m_last_error;
};

#endif
