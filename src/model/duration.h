#ifndef BBQ_MODEL_DURATION_H
#define BBQ_MODEL_DURATION_H

#include <QString>
#include <QtGlobal>

/*
 * A span of time, in the units a reader thinks in (project.md
 * sec 16.45).
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

#endif
