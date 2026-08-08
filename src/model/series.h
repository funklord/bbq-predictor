#ifndef BBQ_SERIES_H
#define BBQ_SERIES_H

#include <QString>
#include <QTimeZone>

#include <cstddef>
#include <utility>
#include <vector>

#include "model/sample.h"

/*
 * Which band a series carries (project.md sec 3).
 *
 * A band is not a provider. The same graph may take its nowcast from
 * one service and its hourly forecast from another (sec 2.7), so the
 * two travel separately on bbq_series below.
 */
enum class bbq_band {
	observed,
	current,

	/*
	 * A radar nowcast: finer than the ordinary one and much shorter.
	 *
	 * Its own band rather than a better provider for the existing one,
	 * because the two cover different DISTANCES into the future --
	 * about two hours at five minutes against seven at fifteen.
	 * Substituting one for the other traded four hours of quarter-hour
	 * data for hourly, which is a regression dressed as an upgrade.
	 */
	nowcast_fine,

	nowcast,

	/*
	 * Hourly, from a provider that needs no key, reaching sixteen days.
	 *
	 * Ranked below the hourly band rather than above it: it is the same
	 * cadence, so sec 2.7 gives the tie to WU. What it covers is what
	 * WU does not -- the observed band's lag behind now, the sixteenth
	 * day, and everything if the scrape stops answering.
	 */
	extended,

	/*
	 * This program's opinion about somebody else's forecast: a band
	 * corrected by its own measured bias (project.md sec 12.5).
	 *
	 * Ranked below everything so it can never win a column if it is ever
	 * put in a composite. It is drawn as an overlay rather than resolved
	 * against the others, because a corrected number is not a number
	 * anybody reported and must not be able to stand in for one.
	 */
	corrected,

	hourly,
};

/*
 * How long a current observation is taken to hold (project.md sec 3.9).
 *
 * The one place this project knowingly extends a measurement past the
 * moment it was taken, and it is capped precisely so that it can never
 * quietly cover an outage: a reading older than this stops covering
 * anything, and the hole is drawn instead of papered over.
 *
 * Fifteen minutes because that is the width of the gap it exists to
 * close -- the nowcast begins at the next quarter-hour boundary, so
 * nothing longer is ever needed and anything longer would only let a
 * stale reading masquerade as a fresh one.
 */
const int bbq_current_validity_s = 900;

/*
 * Declared precedence (project.md sec 3.3).
 *
 * A TABLE, and it must not become a computation. The tempting rule --
 * whichever band has the finest resolution wins -- is emergent, and
 * would change the graph's meaning silently on the day a provider
 * adjusted its cadence, with nothing in the tree recording that a
 * different source had started winning.
 *
 * observed > nowcast > hourly. The first comparison is the load-bearing
 * one: measured beats forecast always and regardless of resolution,
 * because an observation is what happened.
 */
int bbq_band_priority(bbq_band band);

/* Stable lowercase identifier, for display and for settings keys. */
const char *bbq_band_name(bbq_band band);

/*
 * One band's worth of samples from one provider.
 *
 * Carries its own provenance -- which band, which provider, when it was
 * fetched -- because the composite refuses to merge these away
 * (sec 3.4). Staleness is reported per band (sec 2.4) and an absent
 * band is named rather than left as a hole (sec 2.6.6); neither is
 * possible once several series have been flattened into one array.
 */
class bbq_series {
public:
	bbq_series() = default;
	bbq_series(bbq_band band, QString provider);

	bbq_band band() const { return m_band; }
	const QString &provider() const { return m_provider; }
	int priority() const { return bbq_band_priority(m_band); }

	/*
	 * When this band was last fetched successfully, epoch seconds UTC;
	 * zero means never. Sec 2.4 puts this on the display, because the
	 * scrape will break and its failure mode is a graph that keeps
	 * drawing yesterday's curve while looking perfectly healthy.
	 */
	qint64 fetched_utc() const { return m_fetched_utc; }
	void set_fetched_utc(qint64 fetched_utc);

	/*
	 * The clock at the place this data describes, invalid when the
	 * provider did not say.
	 *
	 * A property of the location rather than of a sample, and it lives
	 * here because the series is what knows where it came from. Sec
	 * 3.12.1: a forecast for somewhere else labelled in the reader's
	 * own timezone is a graph about the wrong hours of the day.
	 */
	const QTimeZone &zone() const { return m_zone; }
	void set_zone(const QTimeZone &zone);

	/*
	 * Takes the samples and puts them in ascending time order.
	 *
	 * The sort is not a tidy-up. One of the endpoints returns its rows
	 * newest-first (sec 2.6.2), and plotting that unreversed draws a
	 * mirror image of the last day which still looks like weather.
	 * Sorting on the way in means no reader has to remember, and a
	 * provider that changes its mind about ordering cannot break the
	 * graph.
	 */
	void set_samples(std::vector<bbq_sample> samples);

	const std::vector<bbq_sample> &samples() const { return m_samples; }
	bool is_empty() const { return m_samples.empty(); }
	std::size_t size() const { return m_samples.size(); }

	/* Coverage; both zero when empty. */
	qint64 begin_utc() const;
	qint64 end_utc() const;

	/*
	 * The typical distance between successive samples, in seconds, or
	 * zero when there are fewer than two.
	 *
	 * The median rather than the mean, so that one gap in a day of
	 * five-minute observations cannot drag the answer upwards and hide
	 * every other gap behind it.
	 */
	/*
	 * The median stride between samples. Computed once when the samples
	 * are set rather than on each call: has_gap_after needs it, and the
	 * graph asks that question once per pixel column, so recomputing it
	 * meant an allocation and a median per column per repaint.
	 */
	int nominal_step_s() const { return m_nominal_step_s; }

	/*
	 * The sample covering this instant, or nullptr where the series
	 * does not reach or has a hole.
	 */
	const bbq_sample *at(qint64 when_utc) const;

	/*
	 * Indices [first, last) of every sample overlapping [from, to).
	 * Both zero when nothing does.
	 *
	 * Drawing needs this and probing cannot replace it. Sec 3.5 says
	 * rain is downsampled by MAXIMUM, and a maximum requires every
	 * sample in the pixel column -- asking for the value at the middle
	 * of the column would sample rather than downsample, and would drop
	 * exactly the five-minute downpour the rule exists to keep.
	 */
	std::pair<std::size_t, std::size_t> range(qint64 from, qint64 to) const;

	/*
	 * Whether the series is discontinuous between sample `index` and
	 * the one after it (project.md sec 3.6). A gap is drawn as a break
	 * and never interpolated across: joining two points across an hour
	 * of missing data draws a line that is not a measurement, through a
	 * period nobody has any information about.
	 *
	 * False for the last sample, which has no "after".
	 */
	bool has_gap_after(std::size_t index) const;

private:
	int compute_nominal_step_s() const;

	bbq_band m_band = bbq_band::hourly;
	QString m_provider;
	qint64 m_fetched_utc = 0;
	int m_nominal_step_s = 0;
	qint64 m_max_duration_s = 0;
	QTimeZone m_zone;
	std::vector<bbq_sample> m_samples;
};

#endif
