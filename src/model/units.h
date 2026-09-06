#ifndef BBQ_MODEL_UNITS_H
#define BBQ_MODEL_UNITS_H

#include <QString>
#include <QtGlobal>

/*
 * Quantities said in the units a reader thinks in (project.md
 * sec 16.45, sec 16.46).
 *
 * One file rather than one each, and named for what it holds: both are
 * the same fault caught twice, a number that is correct in a unit
 * chosen once and wrong at the other end of its range.
 */

/*
 * A span of time.
 *
 * Written because the feed said "ISTOCK877 has not reported for 1248
 * minutes" on the phone. That is twenty and a bit hours, and nobody
 * reads it as that: the number is correct, the unit is fixed, and the
 * message stops being legible the moment the span outgrows it.
 *
 * The thresholds are the ones GraphWidget.describe already uses for the
 * home-screen widget's own staleness line -- minutes under an hour,
 * hours under two days, days beyond -- so the two surfaces of one
 * program do not disagree about how long a while is. That file is Java
 * and cannot share this, which is why the agreement is stated here
 * rather than assumed.
 */
QString bbq_describe_duration(qint64 seconds);

/*
 * A distance, in metres while it is short.
 *
 * The station list printed `QString::number(km, 'f', 1)`, so a station
 * 340 m away read as "0.3 km" and one under fifty metres read as
 * **0.0 km** -- which looks like missing data rather than a near
 * neighbour. That is not a corner: with no location fix the geocode is
 * back-filled from the watched station's own position, so discovery
 * returns that station at zero and the list says so.
 *
 * Rounded to ten metres, because the underlying figure comes from a
 * provider's own arithmetic over coordinates it rounded first, and a
 * metre of precision would be invented here.
 */
QString bbq_describe_distance(double km);

#endif
