#ifndef BBQ_COMPOSITE_H
#define BBQ_COMPOSITE_H

#include <QTimeZone>
#include <QtGlobal>

#include <vector>

#include "model/series.h"

/*
 * One resolved reading, with the series it came from.
 *
 * The series pointer is the point of this type. Sec 3.4 refuses to
 * merge the bands precisely so that a value can always say where it
 * came from -- which band, which provider, and how old.
 *
 * Both pointers borrow from the bbq_composite that produced them and
 * are invalidated by adding to it.
 */
struct bbq_reading {
	const bbq_sample *sample = nullptr;
	const bbq_series *series = nullptr;

	bool is_valid() const { return sample != nullptr; }
};

/*
 * The bands, held together and resolved on demand (project.md sec 3.4).
 *
 * NOT a merge. Flattening would be easier to draw and would destroy the
 * one thing this project cannot lose: provenance. Three separate
 * requirements need it and none survives a flatten -- sec 2.4 shows
 * staleness per band, sec 2.6.6 reports which band is absent, and
 * sec 2.7 has bands that may come from different providers.
 *
 * A flattened array cannot say "the observed band is missing". It can
 * only produce a gap, and a gap meaning "not configured" would be drawn
 * identically to one meaning "the station died" and to one meaning "it
 * was not raining".
 *
 * The cost of keeping them apart is nothing: the bands are a few
 * hundred samples each.
 */
class bbq_composite {
public:
	/* Replaces any series already present for the same band. */
	void set_series(bbq_series series);

	const std::vector<bbq_series> &all() const { return m_series; }
	bool is_empty() const { return m_series.empty(); }

	const bbq_series *band(bbq_band band) const;
	bool has_band(bbq_band band) const { return band_series(band) != nullptr; }

	/*
	 * Which of the three bands are not present. Named rather than
	 * inferred from a hole in the drawing (sec 2.6.6).
	 */
	std::vector<bbq_band> missing_bands() const;

	/*
	 * The reading for an instant, taken from the highest-priority band
	 * that actually covers it (sec 3.3).
	 *
	 * Resolution happens here, at the point of use, rather than once
	 * into a merged array -- that is what keeps the provenance on every
	 * value instead of only on the whole graph.
	 */
	bbq_reading at(qint64 when_utc) const;

	/*
	 * The reading from the band that OWNS an instant, which is not
	 * always the finest band covering it (sec 3.18).
	 *
	 * A sharpening band refines one quantity and does not describe the
	 * weather on its own: MET Norway's radar nowcast carries
	 * air_temperature on its first step and precipitation_rate alone on
	 * the other twenty-two. Letting it win a column outright therefore
	 * took the temperature away wherever it reached, which is not what
	 * a band described as a bonus is supposed to do.
	 *
	 * So ownership skips it, and `at()` keeps its old meaning for
	 * callers that want the finest answer available.
	 */
	bbq_reading owner_at(qint64 when_utc) const;

	/* Coverage across every band; both zero when empty. */
	qint64 begin_utc() const;
	qint64 end_utc() const;

	/*
	 * The oldest successful fetch across the bands present, or zero if
	 * any of them has never been fetched.
	 *
	 * The OLDEST, deliberately. A display showing the newest would go
	 * on looking fresh while a band quietly stopped updating, which is
	 * the sec 2.4 failure exactly.
	 */
	qint64 oldest_fetch_utc() const;

	/*
	 * The clock to label this graph in (sec 3.12.1).
	 *
	 * Measured bands are asked first, because their zone is the
	 * station's own IANA name and a forecast band can offer only the
	 * UTC offset it happened to be issued with -- the same clock most
	 * of the year, and the wrong one across a daylight-saving change.
	 *
	 * Invalid when nothing said, which the caller reads as "use the
	 * viewer's" rather than as an error.
	 */
	QTimeZone zone() const;

private:
	const bbq_series *band_series(bbq_band band) const;

	std::vector<bbq_series> m_series;
};

#endif
