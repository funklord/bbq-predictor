#ifndef BBQ_SWEEP_H
#define BBQ_SWEEP_H

#include <QString>

#include <vector>

#include "graph/interpolate.h"

class bbq_composite;

/*
 * A pinch, replayed without a screen (project.md sec 3.18.6).
 *
 * The view is zoomed from 36 hours to two about a moment held a third of
 * the way across, as fingers would, and every frame is reduced through
 * the graph's own pipeline. For each minute from an hour before that
 * moment to three after, each quantity's column value is followed
 * across the frames; what it does beyond moving steadily from its first
 * value to its last is jitter, and jitter is what a flicker is.
 *
 * Written after four rounds of fixing a flicker by reasoning from code
 * and having the holder report it unchanged from the phone: this is the
 * measurement that found the cause in one run.
 */
struct bbq_sweep_moment {
	qint64 at_utc = 0;
	double jitter = 0.0;
	double low = 0.0;
	double high = 0.0;
};

struct bbq_sweep_quantity {
	QString name;
	double total = 0.0;

	/* The moments that jittered most, worst first. */
	std::vector<bbq_sweep_moment> worst;
};

struct bbq_sweep_result {
	int frames = 0;
	std::vector<bbq_sweep_quantity> quantities;
};

bbq_sweep_result bbq_sweep(const bbq_composite &composite, qint64 about_utc, int width,
                           bbq_interpolation method, int smoothing_s);

QString bbq_sweep_report(const bbq_sweep_result &result, qint64 about_utc);

#endif
