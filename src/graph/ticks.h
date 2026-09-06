#ifndef BBQ_GRAPH_TICKS_H
#define BBQ_GRAPH_TICKS_H

#include <QString>
#include <QtGlobal>

/*
 * How far apart the time axis puts its ticks, and what they say
 * (project.md sec 13, sec 16.52).
 *
 * Its own header because it was unreachable: the graph's .cpp keeps it
 * in an anonymous namespace, so nothing could assert on it -- and
 * `bbq_metrics::tick_step_s`, which this superseded, went on being
 * tested instead. A test named for the behaviour, asserting a field
 * nothing reads.
 */
struct bbq_tick_choice {
	qint64 step_s = 3600;
	QString format = QStringLiteral("HH:mm");
};

/*
 * `wanted` is roughly how many labels will fit, which the caller
 * derives from the plot's width. That is what makes a phone's axis
 * coarser than a desktop's: the same span over fewer pixels asks for
 * fewer labels and lands further up the ladder.
 */
bbq_tick_choice bbq_ticks_for(qint64 span_s, int wanted);

#endif
