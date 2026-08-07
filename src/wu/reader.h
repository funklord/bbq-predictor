#ifndef BBQ_WU_READER_H
#define BBQ_WU_READER_H

#include <QJsonDocument>
#include <QString>

#include "model/series.h"

/*
 * Turns Weather Underground's responses into this project's own series
 * (project.md sec 2.7, sec 3).
 *
 * The translation direction matters. The internal series is the
 * project's, and WU translates into it exactly as any later provider
 * will -- WU is privileged by priority, not by the data model. Nothing
 * downstream of here should be able to tell which service the samples
 * came from except by asking the series who its provider is.
 *
 * This is where sec 2.6.2's traps are paid for, and each of them
 * produces a plausible wrong graph rather than an error if missed:
 *
 *   - the nowcast has no UTC field at all, only local time with an
 *     offset;
 *   - rain arrives as three different quantities and leaves as one;
 *   - sample spacing is irregular, so durations come from real
 *     timestamps rather than an assumed cadence.
 *
 * Every reader returns an empty series on malformed input rather than a
 * partly-populated one. A series with half its samples is worse than no
 * series: sec 2.6.6 can report an absent band honestly, but it cannot
 * report a band that is quietly short.
 */

/*
 * /v2/pws/history/all -- row-oriented, 5-minute-ish, measured.
 *
 * Temperature comes from metric.tempAvg, which is a mean across the
 * interval rather than a reading at its start. That differs from the
 * forecast bands, where the value is a point, and the difference is
 * noted rather than hidden -- it is small, it is real, and a future
 * reader deserves to know the model is carrying two slightly different
 * things in one field.
 */
bbq_series bbq_wu_read_observed(const QJsonDocument &response);

/*
 * /v3/wx/forecast/fifteenminute -- column-oriented, 15 minutes, 7 hours.
 *
 * The band with the finest forecast resolution is the one with no
 * validTimeUtc. Its validTimeLocal carries an explicit offset, so it
 * converts without needing the station's zone.
 */
bbq_series bbq_wu_read_nowcast(const QJsonDocument &response);

/*
 * /v3/wx/forecast/hourly/15day -- column-oriented, hourly, 15 days.
 *
 * qpf is an accumulation over the step and becomes a rate here.
 */
bbq_series bbq_wu_read_hourly(const QJsonDocument &response);

#endif
