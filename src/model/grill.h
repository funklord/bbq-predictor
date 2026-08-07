#ifndef BBQ_GRILL_H
#define BBQ_GRILL_H

#include <QTimeZone>
#include <QtGlobal>

#include <vector>

#include "model/composite.h"

/*
 * Scoring the forecast for grilling weather (project.md sec 7).
 *
 * The feature the project is named after, deferred from its first
 * commit with a note that what makes a window good was the interesting
 * part and nobody had stated it. These are the answers, and they are
 * PREFERENCES rather than findings -- somebody said what they like, and
 * a different cook would set them differently.
 *
 * That distinction is why they are gathered in one struct instead of
 * scattered through the arithmetic. Everything measured in this project
 * was checked against a real response; none of this can be, and the two
 * should not be hard to tell apart when reading the code.
 */
struct bbq_grill_policy {
	/*
	 * Warmer is always better, so there is no upper bound at all -- the
	 * score rises to full marks and stays there. Only cold counts
	 * against a window.
	 */
	double cold_zero_c = 5.0;
	double warm_enough_c = 25.0;

	/*
	 * Rain is a graded penalty rather than a veto: light drizzle is
	 * survivable and a downpour is not, and the difference is worth
	 * keeping.
	 */
	double rain_ruins_mm_h = 2.0;

	/*
	 * Chance counts for less than rate, deliberately. A dry hour
	 * forecast at eighty percent is a poor bet, but the rate is the
	 * thing being promised and the chance is the confidence around it.
	 */
	double chance_weight = 0.5;

	/* Wind steals heat, blows smoke, and carries embers. */
	double wind_fine_kph = 15.0;
	double wind_ruins_kph = 45.0;

	/*
	 * Local hours. Late afternoon into the evening is the point of the
	 * exercise; the small hours score low however good the weather is.
	 */
	int prime_from_hour = 16;
	int prime_to_hour = 21;
	int usable_from_hour = 11;
	int usable_to_hour = 23;

	/* A window must reach this to be offered at all. */
	double good_enough = 0.5;

	/*
	 * Two hours to be worth offering, three to be worth preferring.
	 * Both were asked for, and they are not in conflict: the first is a
	 * floor and the second is a ranking.
	 */
	int minimum_s = 2 * 3600;
	int preferred_s = 3 * 3600;
};

/* A contiguous stretch of weather worth lighting a fire in. */
struct bbq_window {
	qint64 start_utc = 0;
	qint64 end_utc = 0;

	/* Mean and worst score across the window, both 0 to 1. */
	double score = 0.0;
	double worst = 0.0;

	/* Ranking score: the mean, tempered by how long it lasts. */
	double rank = 0.0;

	int duration_s() const { return static_cast<int>(end_utc - start_utc); }
};

/*
 * Score one instant, 0 to 1, or -1 where no band covers it.
 *
 * The factors MULTIPLY rather than average, so a window has to be
 * decent in every respect instead of trading a downpour against a
 * pleasant temperature. An averaging score would happily recommend
 * grilling in the rain because it was warm.
 */
double bbq_grill_score(const bbq_composite &composite, const QTimeZone &zone,
                       qint64 when_utc, const bbq_grill_policy &policy);

/*
 * Every window worth offering between two instants, best first.
 *
 * Gaps in the data end a window rather than being scored through: a
 * stretch nobody has a forecast for is not a stretch anybody should be
 * told to grill in (sec 3.6, and the same reasoning).
 */
std::vector<bbq_window> bbq_grill_windows(const bbq_composite &composite,
                                          const QTimeZone &zone, qint64 from,
                                          qint64 to,
                                          const bbq_grill_policy &policy);

#endif
