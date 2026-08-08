#include "model/series.h"

#include <algorithm>

namespace {

/*
 * How much bigger than the nominal step a stride has to be before it
 * counts as a gap (project.md sec 3.6).
 *
 * A guess, and labelled as one there too. The observed band arrives at
 * 288 and 306 seconds against a nominal 300, so some tolerance is
 * required or every other sample would be a break; 1.5 clears that
 * comfortably while still catching a genuinely missing sample. Replace
 * this with whatever real data makes necessary rather than defending
 * the number.
 */
const double gap_factor = 1.5;

} // namespace

int bbq_band_priority(bbq_band band) {
	/*
	 * Declared, per sec 3.3. Higher wins. The gaps between the numbers
	 * are deliberate: another band inserted later should be given a
	 * number here by somebody deciding where it belongs, not computed
	 * from how finely it samples.
	 */
	switch (band) {
	case bbq_band::observed:
		return 300;
	/* Above the ordinary nowcast where they overlap; it is finer. */
	case bbq_band::nowcast_fine:
		return 250;

	case bbq_band::nowcast:
		return 200;

	/*
	 * Below the nowcast, and this is not a contradiction of "measured
	 * beats forecast" -- it is that rule read precisely.
	 *
	 * The rule governs an instant a band GENUINELY covers, and a
	 * current observation genuinely covers only the moment it was
	 * taken. The rest of its span is the declared extension in
	 * bbq_current_validity_s, and those minutes were never measured.
	 * A forecast made FOR them beats a reading stretched INTO them.
	 *
	 * Ranking it here makes the extension provably harmless: current
	 * paints only where no other band reaches, which is exactly the
	 * hole it was added to fill and nothing else.
	 */
	case bbq_band::current:
		return 150;

	/*
	 * BELOW the hourly band, not above it.
	 *
	 * It outranked hourly while it was believed to be quarter-hourly.
	 * It is hourly too (sec 2.10.4), so the tie goes to WU by sec 2.7's
	 * ordering, and this band does what its name says instead: it
	 * covers what WU does not reach -- the observed band's lag behind
	 * now, the sixteenth day, and the whole forecast if the scrape ever
	 * stops answering.
	 */
	case bbq_band::extended:
		return 90;

	case bbq_band::hourly:
		return 100;
	}

	return 0;
}

const char *bbq_band_name(bbq_band band) {
	switch (band) {
	case bbq_band::observed:
		return "observed";
	case bbq_band::current:
		return "current";
	case bbq_band::nowcast_fine:
		return "radar";
	case bbq_band::nowcast:
		return "nowcast";
	case bbq_band::extended:
		return "extended";
	case bbq_band::hourly:
		return "hourly";
	}

	return "unknown";
}

bbq_series::bbq_series(bbq_band band, QString provider)
        : m_band(band), m_provider(std::move(provider)) {
}

void bbq_series::set_fetched_utc(qint64 fetched_utc) {
	m_fetched_utc = fetched_utc;
}

void bbq_series::set_zone(const QTimeZone &zone) {
	m_zone = zone;
}

void bbq_series::set_samples(std::vector<bbq_sample> samples) {
	m_samples = std::move(samples);

	const auto by_start = [](const bbq_sample &left, const bbq_sample &right) {
		return left.start_utc < right.start_utc;
	};

	std::sort(m_samples.begin(), m_samples.end(), by_start);
	m_nominal_step_s = compute_nominal_step_s();
}

qint64 bbq_series::begin_utc() const {
	if (m_samples.empty()) {
		return 0;
	}

	return m_samples.front().start_utc;
}

qint64 bbq_series::end_utc() const {
	if (m_samples.empty()) {
		return 0;
	}

	return m_samples.back().end_utc();
}

int bbq_series::compute_nominal_step_s() const {
	if (m_samples.size() < 2) {
		return 0;
	}

	std::vector<qint64> strides;
	strides.reserve(m_samples.size() - 1);

	for (std::size_t i = 1; i < m_samples.size(); ++i) {
		const qint64 stride = m_samples[i].start_utc - m_samples[i - 1].start_utc;
		strides.push_back(stride);
	}

	const std::size_t middle = strides.size() / 2;
	std::nth_element(strides.begin(), strides.begin() + middle, strides.end());
	return static_cast<int>(strides[middle]);
}

const bbq_sample *bbq_series::at(qint64 when_utc) const {
	if (m_samples.empty()) {
		return nullptr;
	}

	/*
	 * The last sample starting at or before the instant. Binary search
	 * because the vector is sorted on the way in, and because the
	 * observed band is 288 samples a day before anything else is drawn.
	 */
	const auto after = std::upper_bound(
	        m_samples.begin(), m_samples.end(), when_utc,
	        [](qint64 value, const bbq_sample &sample) {
		        return value < sample.start_utc;
	        });

	if (after == m_samples.begin()) {
		return nullptr;
	}

	const bbq_sample &candidate = *(after - 1);

	/*
	 * Starting before the instant is not the same as covering it. A
	 * sample whose span ended an hour ago says nothing about now, and
	 * returning it would be interpolation across a gap by another name
	 * (sec 3.6).
	 */
	if (!candidate.covers(when_utc)) {
		return nullptr;
	}

	return &candidate;
}

std::pair<std::size_t, std::size_t> bbq_series::range(qint64 from,
                                                      qint64 to) const {
	if (m_samples.empty() || to <= from) {
		return std::make_pair(std::size_t(0), std::size_t(0));
	}

	/*
	 * Overlap, not containment. A sixty-minute sample straddling a
	 * two-minute column belongs to that column even though it starts
	 * well before it, and dropping it would leave the column empty and
	 * be drawn as a gap that is not there.
	 */
	std::size_t first = 0;
	while (first < m_samples.size() && m_samples[first].end_utc() <= from) {
		++first;
	}

	std::size_t last = first;
	while (last < m_samples.size() && m_samples[last].start_utc < to) {
		++last;
	}

	return std::make_pair(first, last);
}

bool bbq_series::has_gap_after(std::size_t index) const {
	if (index + 1 >= m_samples.size()) {
		return false;
	}

	const int nominal = nominal_step_s();
	if (nominal <= 0) {
		return false;
	}

	const qint64 stride =
	        m_samples[index + 1].start_utc - m_samples[index].start_utc;
	const double threshold = nominal * gap_factor;
	return static_cast<double>(stride) > threshold;
}
