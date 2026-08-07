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
	const bbq_band every[] = {
		bbq_band::observed,
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
