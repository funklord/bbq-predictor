#include "model/grill.h"

#include <QDateTime>

#include <algorithm>

namespace {

/* Linear ramp from zero at `low` to one at `high`, clamped. */
double ramp_up(double value, double low, double high) {
	if (high <= low) {
		return value >= high ? 1.0 : 0.0;
	}

	const double t = (value - low) / (high - low);
	return std::max(0.0, std::min(1.0, t));
}

/* Linear fall from one at `good` to zero at `bad`, clamped. */
double ramp_down(double value, double good, double bad) {
	if (bad <= good) {
		return value <= good ? 1.0 : 0.0;
	}

	const double t = (value - good) / (bad - good);
	return std::max(0.0, std::min(1.0, 1.0 - t));
}

double hour_factor(int hour, const bbq_grill_policy &policy) {
	if (hour >= policy.prime_from_hour && hour <= policy.prime_to_hour) {
		return 1.0;
	}

	if (hour >= policy.usable_from_hour && hour <= policy.usable_to_hour) {
		return 0.6;
	}

	/*
	 * Not zero. Somebody grilling at two in the morning has reasons of
	 * their own, and a score of nothing would hide genuinely fine
	 * weather rather than merely rank it low.
	 */
	return 0.15;
}

} // namespace

double bbq_grill_score(const bbq_composite &composite, const QTimeZone &zone,
                       qint64 when_utc, const bbq_grill_policy &policy) {
	const bbq_reading reading = composite.at(when_utc);
	if (!reading.is_valid()) {
		return -1.0;
	}

	/*
	 * RESOLVED, not the winning band's raw sample (sec 3.18).
	 *
	 * at() returns the finest band covering the instant, and for the
	 * next two hours that is the radar nowcast, which carries
	 * precipitation and no temperature on twenty-two of its
	 * twenty-three steps. The rule below treats an absent temperature
	 * as neutral -- correct when nothing knows it, wrong here, where
	 * the hourly and nowcast bands know it perfectly well and were
	 * simply outranked.
	 *
	 * The effect was a score for the nearest two hours computed almost
	 * entirely from rain: a cold dry evening scored as warm, and the
	 * best window could open in it. That is the recommendation this
	 * program exists to make, so it is the worst place in the tree for
	 * a field to go missing quietly.
	 */
	const bbq_sample sample = composite.resolved_at(when_utc);
	double score = 1.0;

	/*
	 * Warmer is always better, so this only ever climbs. A missing
	 * temperature is treated as neutral rather than as cold: absent is
	 * not the same as freezing, and guessing the pessimistic direction
	 * would bury windows for want of a field.
	 */
	if (sample.temperature.has_value()) {
		score *= ramp_up(*sample.temperature, policy.cold_zero_c,
		                 policy.warm_enough_c);
	}

	if (sample.precip_rate.has_value()) {
		score *= ramp_down(*sample.precip_rate, 0.0, policy.rain_ruins_mm_h);
	}

	/*
	 * Chance tempers the score rather than setting it, so a confident
	 * dry hour beats a doubtful one without a forecast of no rain being
	 * overruled by the probability attached to it.
	 */
	if (sample.precip_chance.has_value()) {
		const double doubt = (*sample.precip_chance / 100.0) * policy.chance_weight;
		score *= std::max(0.0, 1.0 - doubt);
	}

	if (sample.wind_kph.has_value()) {
		score *= ramp_down(*sample.wind_kph, policy.wind_fine_kph,
		                   policy.wind_ruins_kph);
	}

	/* The clock at the fire, not the reader's (sec 3.12.1). */
	QDateTime local = QDateTime::fromSecsSinceEpoch(when_utc);
	if (zone.isValid()) {
		local = QDateTime::fromSecsSinceEpoch(when_utc, zone);
	}

	score *= hour_factor(local.time().hour(), policy);
	return score;
}

std::vector<bbq_window> bbq_grill_windows(const bbq_composite &composite,
                                          const QTimeZone &zone, qint64 from,
                                          qint64 to,
                                          const bbq_grill_policy &policy) {
	std::vector<bbq_window> windows;

	if (to <= from) {
		return windows;
	}

	/*
	 * Ten minutes. Fine enough that a window's edges are not rounded to
	 * something misleading, coarse enough that fifteen days is a few
	 * thousand steps rather than a hundred thousand -- and bounded by
	 * construction, since the count is the span divided by the stride.
	 */
	const qint64 stride = 600;

	bool open = false;
	qint64 start = 0;
	double total = 0.0;
	double worst = 0.0;
	int count = 0;

	const auto close_window = [&](qint64 end) {
		if (!open) {
			return;
		}

		open = false;

		if (end - start < policy.minimum_s || count == 0) {
			return;
		}

		bbq_window window;
		window.start_utc = start;
		window.end_utc = end;
		window.score = total / count;
		window.worst = worst;

		/*
		 * Longer is better up to the preferred length and no better
		 * after it. A six-hour window is not twice the afternoon a
		 * three-hour one is; it is the same afternoon with more of it
		 * spare.
		 */
		const double length = static_cast<double>(end - start);
		const double reach = std::min(1.0, length / policy.preferred_s);
		window.rank = window.score * (0.6 + 0.4 * reach);

		windows.push_back(window);
	};

	for (qint64 t = from; t <= to; t += stride) {
		const double score = bbq_grill_score(composite, zone, t, policy);

		/*
		 * An uncovered instant ends the window rather than scoring
		 * zero. A stretch nobody has a forecast for is not a stretch to
		 * recommend, and it is not one to condemn either.
		 */
		if (score < 0.0) {
			close_window(t);
			continue;
		}

		if (score < policy.good_enough) {
			close_window(t);
			continue;
		}

		if (!open) {
			open = true;
			start = t;
			total = 0.0;
			worst = score;
			count = 0;
		}

		total += score;
		worst = std::min(worst, score);
		++count;
	}

	close_window(to);

	const auto by_rank = [](const bbq_window &left, const bbq_window &right) {
		return left.rank > right.rank;
	};
	std::sort(windows.begin(), windows.end(), by_rank);

	return windows;
}
