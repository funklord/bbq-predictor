#include "model/composite.h"

#include <algorithm>

void bbq_composite::set_series(bbq_series series) {
	for (bbq_series &existing : m_series) {
		if (existing.band() == series.band()) {
			existing = std::move(series);
			return;
		}
	}

	m_series.push_back(std::move(series));
}

const bbq_series *bbq_composite::band_series(bbq_band band) const {
	for (const bbq_series &series : m_series) {
		if (series.band() == band) {
			return &series;
		}
	}

	return nullptr;
}

const bbq_series *bbq_composite::band(bbq_band band) const {
	return band_series(band);
}

std::vector<bbq_band> bbq_composite::missing_bands() const {
	/*
	 * The radar and extended bands are deliberately absent from this
	 * list.
	 *
	 * Both are enhancements: they sharpen bands that already have a
	 * source rather than supplying one that would otherwise be blank.
	 * The radar band is a bonus where MET Norway reaches and simply
	 * absent elsewhere, and the extended band fills in past the edge of
	 * what Weather Underground answers for. Reporting either missing
	 * would put a complaint on the display for something nobody asked
	 * for and nothing depends on -- and sec 2.6.6's point is that
	 * "missing" should mean something.
	 */
	const bbq_band every[] = {
		bbq_band::observed,
		bbq_band::current,
		bbq_band::nowcast,
		bbq_band::hourly,
	};

	std::vector<bbq_band> missing;
	for (bbq_band candidate : every) {
		const bbq_series *series = band_series(candidate);

		/*
		 * Present but empty counts as missing. A band that answered
		 * with nothing in it is not a band the graph can draw, and
		 * calling it present would put the honest report one step
		 * further from the truth.
		 */
		if (series == nullptr || series->is_empty()) {
			missing.push_back(candidate);
		}
	}

	return missing;
}

bbq_reading bbq_composite::at(qint64 when_utc) const {
	bbq_reading best;
	int best_priority = 0;

	for (const bbq_series &series : m_series) {
		const bbq_sample *sample = series.at(when_utc);
		if (sample == nullptr) {
			continue;
		}

		/*
		 * Strictly greater, so the first band added wins a tie. There
		 * are no ties among the declared priorities today; this only
		 * fixes the behaviour if two bands are ever given the same
		 * number, rather than leaving it to vector order.
		 */
		if (!best.is_valid() || series.priority() > best_priority) {
			best.sample = sample;
			best.series = &series;
			best_priority = series.priority();
		}
	}

	return best;
}

bbq_reading bbq_composite::owner_at(qint64 when_utc) const {
	bbq_reading best;
	int best_priority = 0;

	for (const bbq_series &series : m_series) {
		/*
		 * The one skip, and the whole point of this function. Named
		 * rather than inferred from whether a particular sample
		 * happens to carry a temperature: the radar band's FIRST step
		 * does carry one, so a data-driven test would hand it the
		 * column for five minutes in every two hours and take it back
		 * again -- ownership flickering with the clock.
		 */
		if (series.band() == bbq_band::nowcast_fine) {
			continue;
		}

		const bbq_sample *sample = series.at(when_utc);
		if (sample == nullptr) {
			continue;
		}

		if (!best.is_valid() || series.priority() > best_priority) {
			best.sample = sample;
			best.series = &series;
			best_priority = series.priority();
		}
	}

	return best;
}

bbq_sample bbq_composite::resolved_at(qint64 when_utc) const {
	bbq_reading base = owner_at(when_utc);
	if (!base.is_valid()) {
		/*
		 * Radar alone covers this instant. Its rain is better than
		 * nothing, and refusing it would trade a missing temperature
		 * for a missing shower.
		 */
		base = at(when_utc);
	}

	if (!base.is_valid()) {
		return bbq_sample();
	}

	bbq_sample composed = *base.sample;

	const bbq_series *fine = band_series(bbq_band::nowcast_fine);
	if (fine != nullptr && base.series != fine) {
		const bbq_sample *sharp = fine->at(when_utc);
		if (sharp != nullptr && sharp->precip_rate.has_value()) {
			composed.precip_rate = sharp->precip_rate;
		}
	}

	return composed;
}

qint64 bbq_composite::begin_utc() const {
	qint64 earliest = 0;

	for (const bbq_series &series : m_series) {
		if (series.is_empty()) {
			continue;
		}

		if (earliest == 0 || series.begin_utc() < earliest) {
			earliest = series.begin_utc();
		}
	}

	return earliest;
}

qint64 bbq_composite::end_utc() const {
	qint64 latest = 0;

	for (const bbq_series &series : m_series) {
		if (series.is_empty()) {
			continue;
		}

		if (series.end_utc() > latest) {
			latest = series.end_utc();
		}
	}

	return latest;
}

QTimeZone bbq_composite::zone() const {
	const bbq_band preference[] = {
		bbq_band::observed,
		bbq_band::current,
		bbq_band::hourly,
		bbq_band::nowcast,
		bbq_band::nowcast_fine,
		bbq_band::extended,
	};

	for (bbq_band band : preference) {
		const bbq_series *series = band_series(band);
		if (series != nullptr && series->zone().isValid()) {
			return series->zone();
		}
	}

	return QTimeZone();
}

qint64 bbq_composite::oldest_fetch_utc() const {
	qint64 oldest = 0;

	for (const bbq_series &series : m_series) {
		const qint64 fetched = series.fetched_utc();

		/*
		 * Never fetched poisons the answer rather than being skipped.
		 * "One of these bands has never loaded" is not a freshness a
		 * display should round up to the others'.
		 */
		if (fetched == 0) {
			return 0;
		}

		if (oldest == 0 || fetched < oldest) {
			oldest = fetched;
		}
	}

	return oldest;
}
