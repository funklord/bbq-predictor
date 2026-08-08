#ifndef BBQ_MODEL_CORRECTION_H
#define BBQ_MODEL_CORRECTION_H

#include <QString>
#include <QtGlobal>

#include "model/composite.h"
#include "model/series.h"
#include "store/history.h"

/*
 * The bias-corrected forecast (project.md sec 12.5).
 *
 * Takes what the providers said, subtracts the error they have actually
 * been making at that lead time, and returns the result as its own
 * series. It is never merged into the composite: a corrected number is
 * this program's opinion rather than anything a provider reported, and
 * sec 3.11.3 does not let an opinion stand where a reading would.
 */

/*
 * How many verified comparisons a bucket needs before its bias is
 * allowed to move the curve.
 *
 * A handful of comparisons is noise, and a correction built from noise
 * is worse than none: it would move the forecast by an amount that says
 * more about last Tuesday than about the provider. Below this the band
 * is simply absent, which is the honest way to say "not known yet"
 * rather than drawing an uncorrected line under a corrected label.
 */
const int bbq_correction_minimum = 20;

/*
 * `issued_utc` is the moment the correction is being made FROM -- now,
 * in the running applet. Lead time is measured from it, because that is
 * what decides which bucket's bias applies.
 *
 * Returns an empty series when nothing has been verified enough to act
 * on, which is the normal state of a fresh install.
 */
bbq_series bbq_corrected_forecast(const bbq_composite &composite,
                                  const bbq_history &history,
                                  const QString &station, qint64 from_utc,
                                  qint64 to_utc, qint64 issued_utc,
                                  int minimum_count = bbq_correction_minimum);

#endif
