#include "graph/forecast_graph.h"

#include "graph/simplify.h"
#include "graph/ticks.h"

#include "ui/theme.h"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QEvent>
#include <QGestureEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QRect>
#include <QTime>
#include <QTimeZone>

#include <algorithm>
#include <optional>
#include <vector>

namespace {

/*
 * Only the top margin is fixed. Every other piece of chrome comes from
 * the layout (sec 10), because those are the numbers the two shapes
 * disagree about -- and a constant here would be a third opinion
 * nobody set.
 */
const int margin_top = 10;

/*
 * The rain chance shares the plot now (sec 3.19).
 *
 * It had a 46-pixel panel of its own below the chart, on the reasoning
 * that a percentage has nothing to do with either axis above it and
 * hanging it off one would make a scale mean two things. The second
 * half of that still holds and is why it keeps its own mapping; the
 * first turned out to cost more than it bought, because a strip that
 * size is unreadable on a phone and took a fifth of the height from
 * the series people actually look at.
 *
 * Each series spans the full height on its own scale, which is what
 * WU's forecast chart does -- as against their dashboard, which is
 * what the stacked panel was copying.
 */

/*
 * Full height of the rain trace, in mm/h, and FIXED (sec 3.16).
 *
 * It used to be the largest rate in view, floored at 1.0. That made the
 * height meaningless as a quantity: drizzle at 1 mm/h drew to 45% of
 * the panel exactly as a downpour would, because each was the worst
 * thing on its own screen. Light rain looked alarming, and the only
 * clue was a small "1.0 mm/h" label at the edge that nobody reads
 * before forming an impression.
 *
 * 10 mm/h because that is where the meteorological classes fall: light
 * is under 2.5, moderate runs to about 10, and heavy is past it. So the
 * bottom quarter of the trace is light rain, the middle is moderate,
 * and anything filling the band is genuinely heavy. The picture now
 * means the same thing on every screen and at every zoom.
 *
 * Rates above it clamp rather than rescale, and the edge label gains a
 * "+" so a clipped trace says so -- losing the top of a downpour
 * silently would be the one thing sec 3.5 refuses to do with rain.
 */
const double rain_full_scale_mm_h = 10.0;

/*
 * How far the hour marks reach in from the top and bottom (sec 3.20).
 *
 * 7, then 9, then 12 -- and the length was never the real problem. The
 * marks were drawn in `grid`, which on the dark palette is #33373b
 * against a #16181a ground: luminance 52 against 24, invisible at any
 * alpha and at any length. They are drawn in `axis_text` now, the
 * colour of the labels they stand under, which is what a mark
 * accompanying a label should share.
 */
const double edge_tick_px = 12.0;

/*
 * Scale a font by whichever unit it is actually defined in (sec 3.21).
 *
 * `setPointSizeF(pointSizeF() * k)` is the obvious spelling and it is
 * wrong on Android, where the UI font is sized in PIXELS: `pointSizeF()`
 * returns -1 there, so the product is negative, Qt refuses it with
 *
 *     QFont::setPointSizeF: Point size <= 0 (-1.000000)
 *
 * and the font keeps whatever size it had. The scaling silently did
 * nothing on the one platform whose labels most needed it, and said so
 * on every paint to a log nobody reads.
 */
QFont scaled_font(const QFont &base, double factor) {
	QFont out = base;

	if (out.pointSizeF() > 0.0) {
		out.setPointSizeF(out.pointSizeF() * factor);
		return out;
	}

	if (out.pixelSize() > 0) {
		out.setPixelSize(qMax(1, qRound(out.pixelSize() * factor)));
	}

	return out;
}

/*
 * One pixel column's worth of the composite.
 *
 * Everything the painter needs about a column is decided once, here,
 * rather than re-derived by each pass -- the rain area, the temperature
 * line and the provenance ribbon all walk the same vector.
 */
struct column {
	bool covered = false;
	bool has_temperature = false;
	double temperature = 0.0;
	bool has_rain = false;
	double rain = 0.0;
	bool has_chance = false;
	double chance = 0.0;
	bool has_wind = false;
	double wind = 0.0;
	bbq_band band = bbq_band::hourly;

	/*
	 * The real samples that START in this column, if any. These are the
	 * knots the curve is built from, and the only points that get
	 * marked -- everything else on screen is drawn between them
	 * (sec 3.11.1, sec 3.11.3).
	 */
	bool has_knot = false;

	/*
	 * How many samples started here, which is what decides whether the
	 * marks can honestly be drawn at this zoom (sec 13.2).
	 */
	int knot_count = 0;

	/*
	 * The knot sample's OWN start time, not the column's.
	 *
	 * A column is a couple of minutes wide, so deriving the time from
	 * its position put 05:59 in the readout for a sample that is
	 * stamped 06:00. Small, and exactly the kind of number that looks
	 * measured because everything around it is.
	 */
	qint64 knot_utc = 0;

	/*
	 * And the LAST one, which matters only when the column holds more
	 * than one (sec 16.55). The pair is what lets the readout say it
	 * is describing a range rather than a moment.
	 */
	qint64 knot_last_utc = 0;
	bool knot_has_temperature = false;
	double knot_temperature = 0.0;
	bool knot_has_rain = false;
	double knot_rain = 0.0;
	bool knot_has_chance = false;
	double knot_chance = 0.0;
	bool knot_has_wind = false;
	double knot_wind = 0.0;
};

/* Which quantity a curve is being built for. */
enum class quantity {
	temperature,
	rain,
	chance,
	wind,
};

/*
 * HOW MUCH GROUND A LINE CARRIES WITH IT (project.md sec 16.32).
 *
 * One pixel each side. Enough to separate the ink from whatever wash it
 * crosses, small enough that two lines a couple of pixels apart still
 * read as two lines rather than as one thick one.
 */
const double halo_grow = 2.0;

/*
 * And a wider one for a sample dot (sec 16.32.1).
 *
 * A straight edge loses about a pixel to antialiasing; a disc loses it
 * on a curve, in two dimensions at once, at both the inner and the
 * outer boundary of the ring. At 1.5 the ring had no solid pixel left
 * in it, and the test found nine bare sides on the two knots that sit
 * away from the curve -- the only knots whose ring does any work, since
 * the rest are inside the line's own halo.
 *
 * Two pixels, which is what survives being eaten from both sides.
 */
const double dot_ring_grow = 2.0;

/*
 * How many vertical subpixel positions the sample dot is rendered at
 * (sec 16.92).
 *
 * Eight puts the worst placement error at a sixteenth of a pixel, which
 * is below what the antialiasing itself resolves. Sixteen would halve
 * that and double a cache nobody can see; four would put the error at an
 * eighth, which starts to be visible as the curve scrolls under it.
 */
const int dot_stamp_offsets = 8;

/*
 * A pen that strokes the plot's own ground under a line, so the line is
 * read against a colour this program chose rather than against whatever
 * the weather happened to shade underneath it (sec 16.32).
 *
 * ROUND cap and join, and the cap is not decoration. A flat cap ends the
 * stroke square with the line, so at the last point of a run the halo
 * stops exactly where the ink does and the end of the curve sits on the
 * wash with nothing under it -- one bare side in 1490, found only after
 * the test was made to sweep both layouts. A round cap carries the
 * ground half a width past the ink, which is what an end needs.
 */
QPen halo_pen(const QColor &ground, double line_width) {
	return QPen(ground, line_width + 2.0 * halo_grow, Qt::SolidLine,
	            Qt::RoundCap, Qt::RoundJoin);
}

QColor band_colour(const bbq_graph_palette &palette, bbq_band band) {
	switch (band) {
	case bbq_band::observed:
		return palette.band_observed;
	case bbq_band::current:
		return palette.band_current;
	case bbq_band::nowcast_fine:
		return palette.band_nowcast_fine;
	case bbq_band::nowcast:
		return palette.band_nowcast;
	case bbq_band::extended:
		return palette.band_extended;
	case bbq_band::hourly:
		return palette.band_hourly;
	case bbq_band::corrected:
		return palette.corrected;
	}

	return palette.grid;
}

/*
 * Reduce one column, which is where sec 3.5 lives.
 *
 * Temperature is meaned because it varies smoothly and a mean is what a
 * reader expects of it. Rain takes the MAXIMUM over every sample in the
 * column, never a mean: averaging a five-minute downpour into an hour
 * erases it, and that is the one thing on this graph nobody can afford
 * to lose.
 *
 * The samples are taken from the series that wins the column rather
 * than from all of them, so a column shows one band's account of itself
 * instead of a blend of several (sec 3.7 -- no blending).
 */
column reduce(const bbq_composite &composite, qint64 from, qint64 to) {
	column result;

	const qint64 middle = (from + to) / 2;

	/*
	 * The OWNER, not simply the finest band here (sec 3.18).
	 *
	 * The radar band is five-minute precipitation and nothing else
	 * after its first step, so when it won a column outright it took
	 * the temperature with it -- the line stopped dead wherever radar
	 * reached, with the readout showing rain and a band name and no
	 * degrees. Ownership goes to a band that describes the weather;
	 * radar sharpens the rain below.
	 *
	 * Falling back to at() matters: a column radar alone covers is
	 * still worth drawing rain in, and refusing to would trade a
	 * missing line for a missing shower.
	 */
	bbq_reading reading = composite.owner_at(middle);
	if (!reading.is_valid()) {
		reading = composite.at(middle);
	}
	if (!reading.is_valid()) {
		return result;
	}

	result.covered = true;
	result.band = reading.series->band();

	const bbq_series *winner = reading.series;
	const std::pair<std::size_t, std::size_t> span = winner->range(from, to);
	const std::vector<bbq_sample> &samples = reading.series->samples();

	double temperature_total = 0.0;
	int temperature_count = 0;
	double wind_total = 0.0;
	int wind_count = 0;

	for (std::size_t i = span.first; i < span.second; ++i) {
		/*
		 * Temperature counts a sample only where its START lands in
		 * this column, because sec 3.1 makes it a value AT that
		 * instant. Rain counts every OVERLAPPING sample, because it is
		 * a mean across a span and any span touching the column is part
		 * of the column's answer.
		 *
		 * That difference is the whole reason the staircase survived a
		 * first attempt at fixing it: range() returns overlaps, so an
		 * hourly sample is "in" all thirty of its columns, the mean
		 * branch always found one, and the interpolation below never
		 * ran. Same helper, two questions -- and only one of them was
		 * being asked.
		 */
		const bool after_start = samples[i].start_utc >= from;
		const bool before_end = samples[i].start_utc < to;
		const bool starts_here = after_start && before_end;

		if (starts_here) {
			if (!result.has_knot) {
				result.knot_utc = samples[i].start_utc;
			}
			result.knot_last_utc = samples[i].start_utc;
			result.has_knot = true;
			++result.knot_count;

			if (samples[i].temperature.has_value()) {
				temperature_total += *samples[i].temperature;
				++temperature_count;
			}

			/*
			 * Wind is meaned like temperature rather than maxed like
			 * rain: it is a speed AT the sample's instant (sec 3.1), and
			 * a gust is not what these bands report.
			 */
			if (samples[i].wind_kph.has_value()) {
				wind_total += *samples[i].wind_kph;
				++wind_count;
				result.knot_wind = *samples[i].wind_kph;
				result.knot_has_wind = true;
			}

			/*
			 * Knots take the maximum for rain and chance, matching how
			 * the column itself is reduced (sec 3.5) -- a curve built
			 * from meaned peaks would smooth away the thing the maximum
			 * was protecting.
			 */
			if (samples[i].precip_rate.has_value()) {
				const double v = *samples[i].precip_rate;
				if (!result.knot_has_rain || v > result.knot_rain) {
					result.knot_rain = v;
					result.knot_has_rain = true;
				}
			}

			if (samples[i].precip_chance.has_value()) {
				const double v = *samples[i].precip_chance;
				if (!result.knot_has_chance || v > result.knot_chance) {
					result.knot_chance = v;
					result.knot_has_chance = true;
				}
			}
		}

		if (samples[i].precip_rate.has_value()) {
			const double rate = *samples[i].precip_rate;
			if (!result.has_rain || rate > result.rain) {
				result.rain = rate;
				result.has_rain = true;
			}
		}

		/*
		 * Maximum, like the rate and for the same reason. A column
		 * holding a quarter-hour at eighty percent and three at ten is
		 * a column where it might well rain, and meaning that down to
		 * twenty-eight would hide exactly the spike somebody planning
		 * an afternoon is looking for.
		 */
		if (samples[i].precip_chance.has_value()) {
			const double value = *samples[i].precip_chance;
			if (!result.has_chance || value > result.chance) {
				result.chance = value;
				result.has_chance = true;
			}
		}
	}

	/*
	 * Radar sharpens the rain it is expert in, having been kept from
	 * owning the column above (sec 3.18).
	 *
	 * This is the half that makes the band worth fetching: five-minute
	 * precipitation is a better answer than an hourly mean, and the
	 * point of not letting it own the column was never to ignore it.
	 * It is NOT blending in the sense sec 3.7 forbids -- the column
	 * still reports one band's account of each quantity, and rain has
	 * one source here rather than an average of two.
	 *
	 * Both the drawn value and the knot are replaced, or the trace and
	 * the readout would disagree about the same column.
	 */
	const bbq_series *fine = composite.band(bbq_band::nowcast_fine);
	if (fine != nullptr && fine != winner && !fine->is_empty()) {
		const auto fine_span = fine->range(from, to);
		const std::vector<bbq_sample> &fine_samples = fine->samples();

		for (std::size_t i = fine_span.first; i < fine_span.second; ++i) {
			if (!fine_samples[i].precip_rate.has_value()) {
				continue;
			}

			const double rate = *fine_samples[i].precip_rate;

			if (!result.has_rain || rate > result.rain) {
				result.rain = rate;
				result.has_rain = true;
			}

			const qint64 begins = fine_samples[i].start_utc;
			const bool starts_here = begins >= from && begins < to;
			const bool wetter = rate > result.knot_rain;
			const bool sharper = !result.knot_has_rain || wetter;

			if (starts_here && sharper) {
				result.knot_rain = rate;
				result.knot_has_rain = true;
			}
		}
	}

	if (temperature_count > 0) {
		result.temperature = temperature_total / temperature_count;
		result.has_temperature = true;
		result.knot_temperature = result.temperature;
		result.knot_has_temperature = true;
	} else if (reading.sample->temperature.has_value()) {
		/*
		 * A column narrower than the band's step holds no sample start,
		 * so the value is interpolated between the sample covering it
		 * and the next one.
		 *
		 * Holding the covering sample's value flat across its whole
		 * span was the first attempt, and looking at the picture is
		 * what killed it: the hourly band came out as a staircase,
		 * which asserts that temperature is constant for an hour and
		 * then jumps, and that is simply false. Sec 3.1 says
		 * temperature is the value AT the sample's start -- a point --
		 * so joining consecutive points is reading the model correctly
		 * rather than inventing data. Rain is the opposite and is left
		 * alone: it is a mean ACROSS the span, so flat is what it is.
		 */
		result.temperature = *reading.sample->temperature;
		result.has_temperature = true;

		const std::size_t index = span.first;
		const bool have_next = index + 1 < samples.size();

		/*
		 * Never across a gap. Interpolating toward a sample on the far
		 * side of missing data draws a line through a period nothing
		 * reported on, which is what sec 3.6 forbids.
		 */
		if (have_next && !reading.series->has_gap_after(index)) {
			const bbq_sample &here = samples[index];
			const bbq_sample &next = samples[index + 1];

			if (here.temperature.has_value() && next.temperature.has_value()) {
				const double t0 = static_cast<double>(here.start_utc);
				const double t1 = static_cast<double>(next.start_utc);
				const double middle = static_cast<double>((from + to) / 2);

				if (t1 > t0) {
					double fraction = (middle - t0) / (t1 - t0);
					fraction = std::max(0.0, std::min(1.0, fraction));
					const double a = *here.temperature;
					const double b = *next.temperature;
					result.temperature = a + (b - a) * fraction;
				}
			}
		}
	}

	if (wind_count > 0) {
		result.wind = wind_total / wind_count;
		result.has_wind = true;
	} else if (reading.sample->wind_kph.has_value()) {
		result.wind = *reading.sample->wind_kph;
		result.has_wind = true;
	}

	if (!result.has_rain && reading.sample->precip_rate.has_value()) {
		result.rain = *reading.sample->precip_rate;
		result.has_rain = true;
	}

	if (!result.has_chance && reading.sample->precip_chance.has_value()) {
		result.chance = *reading.sample->precip_chance;
		result.has_chance = true;
	}

	return result;
}

/*
 * Replace each covered column's value with one drawn from a curve
 * through the real samples (project.md sec 3.11).
 *
 * Runs are bounded by coverage, so no method ever draws across a gap
 * (sec 3.6) -- a break in the data stays a break in the curve however
 * smooth the setting is.
 *
 * Columns outside the knot range within a run keep the value reduce()
 * already gave them. Those are the leading and trailing edges where the
 * covering sample started before the run or ends after it, and holding
 * its measured value there is right; extrapolating a curve past its
 * last knot would be inventing rather than interpolating.
 */
struct curve_spec {
	quantity which = quantity::temperature;
	bbq_interpolation method = bbq_interpolation::monotone;
	double smooth_columns = 0.0;
};

void apply_curve(std::vector<column> &cols, const curve_spec &spec) {
	const quantity which = spec.which;
	const bbq_interpolation method = spec.method;
	std::size_t i = 0;

	while (i < cols.size()) {
		if (!cols[i].covered) {
			++i;
			continue;
		}

		std::size_t end = i;
		while (end < cols.size() && cols[end].covered) {
			++end;
		}

		std::vector<bbq_knot> knots;
		for (std::size_t k = i; k < end; ++k) {
			const column &c = cols[k];
			bool present = false;
			double value = 0.0;

			switch (which) {
			case quantity::temperature:
				present = c.knot_has_temperature;
				value = c.knot_temperature;
				break;
			case quantity::rain:
				present = c.knot_has_rain;
				value = c.knot_rain;
				break;
			case quantity::chance:
				present = c.knot_has_chance;
				value = c.knot_chance;
				break;
			case quantity::wind:
				present = c.knot_has_wind;
				value = c.knot_wind;
				break;
			}

			if (present) {
				bbq_knot knot;
				knot.x = static_cast<double>(k);
				knot.y = value;
				knots.push_back(knot);
			}
		}

		/*
		 * Smoothed BEFORE the curve is fitted, and only the copy the
		 * curve is built from. The columns keep their knot_* values, so
		 * the marks and the readout still report what was measured
		 * while the line rounds through it -- which is the visible
		 * disagreement sec 3.11.4 relies on.
		 */
		bbq_smooth(knots, spec.smooth_columns);

		if (knots.size() >= 2) {
			bbq_curve curve;
			curve.set(std::move(knots), method);

			for (std::size_t k = i; k < end; ++k) {
				const double x = static_cast<double>(k);
				if (x < curve.first_x() || x > curve.last_x()) {
					continue;
				}

				double value = curve.at(x);

				switch (which) {
				case quantity::temperature:
					cols[k].temperature = value;
					cols[k].has_temperature = true;
					break;
				case quantity::rain:
					/*
					 * Clamped, because the natural cubic can overshoot
					 * and negative rain is not a thing (sec 3.11.2).
					 */
					cols[k].rain = std::max(0.0, value);
					cols[k].has_rain = true;
					break;
				case quantity::chance:
					cols[k].chance = std::max(0.0, std::min(100.0, value));
					cols[k].has_chance = true;
					break;
				case quantity::wind:
					/* No negative wind, for the same reason as rain. */
					cols[k].wind = std::max(0.0, value);
					cols[k].has_wind = true;
					break;
				}
			}
		}

		i = end;
	}
}

/*
 * Times are labelled in the LOCATION's clock, not the reader's
 * (sec 3.12.1). An invalid zone means nothing said, and the viewer's
 * own is the honest fallback rather than pretending to know.
 */
QDateTime local_time(qint64 when_utc, const QTimeZone &zone) {
	if (zone.isValid()) {
		return QDateTime::fromSecsSinceEpoch(when_utc, zone);
	}

	return QDateTime::fromSecsSinceEpoch(when_utc);
}



/*
 * The palette, per colour scheme (project.md sec 10.4).
 *
 * The DATA colours are identical in both. They are measurements of
 * Weather Underground's own chart (sec 3.8.2), and a measurement does
 * not change because the room got darker -- the temperature red stays
 * WU's red. What changes is the ground it is drawn on and the furniture
 * around it: background, grid, band shading, axis text.
 */
bbq_graph_palette palette_for(Qt::ColorScheme scheme) {
	bbq_graph_palette chosen;
	chosen.background = QColor(0xff, 0xff, 0xff);
	chosen.band_shade = QColor(0xf1, 0xf7, 0xfb);
	chosen.grid = QColor(0xe7, 0xe7, 0xe7);
	chosen.axis_text = QColor(0x4a, 0x4a, 0x4a);
	chosen.temperature = QColor(0xd5, 0x20, 0x2a);
	/*
	 * WU's FORECAST chart, not their dashboard (sec 3.8.3).
	 *
	 * This was #87c403, measured from the dashboard's precipitation
	 * series -- a yellow-green, and correct as a measurement. It was
	 * also unreadable as a meaning: asked what it was twice, the
	 * copyright holder's second answer was "I don't know what the green
	 * one is", which is the only test of a colour that counts.
	 *
	 * The forecast chart is what this graph resembles -- a rain area
	 * with the temperature over it -- and there rain is blue. Changing
	 * it is the holder's instruction and supersedes the measurement
	 * rather than contradicting it: both are WU, and this is the WU
	 * view this chart actually is.
	 */
	chosen.rain = QColor(0x2e, 0x7e, 0xbb);

	/*
	 * WU's own rain-family cyan, taken from the accumulation series on
	 * their dashboard. Their dashboard plots observations and so has no
	 * precipitation-chance panel to measure, which is said out loud
	 * rather than left as an implied measurement: this is a WU colour
	 * used for a WU-adjacent purpose, not one sampled from the thing it
	 * is drawing.
	 */
	chosen.chance = QColor(0x17, 0xaa, 0xdb);
	/*
	 * NOW IS YELLOW (sec 3.20). It was WU's blue, which on a chart whose
	 * rain is now also blue said "another band" rather than "here".
	 * Yellow is the one hue nothing else on this chart uses.
	 *
	 * Amber on a light ground rather than the dark theme's brighter
	 * yellow: the same colour that reads against near-black is nearly
	 * invisible against white, and a marker nobody can see is worse
	 * than one in the wrong hue.
	 *
	 * DEEP amber, and the depth is measured rather than chosen
	 * (sec 16.28). #c88a00 was picked against this palette's white
	 * background, where it manages 2.96:1 -- allowed under the floor as
	 * a rounding difference. It has two other grounds and clears
	 * neither: 2.28:1 inside the grill window, and 1.61:1 on the
	 * home-screen widget's scrim over a dark wallpaper, where the
	 * contrast clamp had to walk it onto a dark olive.
	 *
	 * Anything dark enough for the palest of those is in the same
	 * luminance band as the temperature curve, so the family was chosen
	 * on hue and the depth on arithmetic: the lightest amber at this
	 * hue clearing 3.2:1 against the grill window, which is the darkest
	 * of the three grounds and the one nothing used to check. It clears
	 * 6.53:1 on white, 5.02:1 in the grill window and 3.55:1 on the
	 * widget's worst ground, so no clamp has to move it anywhere.
	 */
	chosen.now_marker = QColor(0x80, 0x55, 0x00);
	chosen.stale_warning = QColor(0xd5, 0x20, 0x2a);
	/*
	 * The wash over a good grilling window, and ITS OWN MAXIMUM ALPHA
	 * (sec 16.29). The cap used to be a literal 80 in the painter,
	 * which made it a third opinion nobody set and put it out of reach
	 * of the palette gate; here it is a palette value like any other,
	 * and the gate reads it from the same place the painter does.
	 *
	 * 80 on a white plot: the composite is #ffdbbf, and every ink drawn
	 * over it clears the floor comfortably.
	 */
	chosen.grill_window = QColor(0xff, 0x8b, 0x33, 80);
	chosen.readout_back = QColor(0x2b, 0x2b, 0x2b);
	chosen.readout_edge = QColor(0x9a, 0x9a, 0x9a);
	chosen.readout_text = QColor(0xf0, 0xf0, 0xf0);
	chosen.corrected = QColor(0x8b, 0x6b, 0xb1);
	chosen.wind = QColor(0x6b, 0x8b, 0x9a);
	/* Dark on a light ground, for the same reason the marker is amber. */
	chosen.day_divider = QColor(0x55, 0x5b, 0x60);
	chosen.band_observed = QColor(0x5b, 0x9f, 0x49);
	chosen.band_current = QColor(0x87, 0xc4, 0x03);
	chosen.band_nowcast_fine = QColor(0x00, 0x53, 0xae);
	chosen.band_nowcast = QColor(0x17, 0xaa, 0xdb);
	/*
	 * Outside WU's measured set on purpose: they plot no equivalent
	 * series, so there is nothing to copy, and every colour they DO use
	 * is already spoken for here.
	 *
	 * It was #5b9f49 for one commit, which is the observed band's
	 * colour -- two bands the same shade in the one strip whose entire
	 * job is saying which band you are looking at. Nothing failed; the
	 * ribbon simply stopped answering its question.
	 */
	chosen.band_extended = QColor(0x8b, 0x6b, 0xb1);
	chosen.band_hourly = QColor(0x9a, 0x9a, 0x9a);

	if (scheme != Qt::ColorScheme::Dark) {
		return chosen;
	}

	/*
	 * Dark. Not an inversion: inverting would take WU's measured red to
	 * a cyan that is nobody's temperature colour. Only the surfaces
	 * move, and they move to a near-black rather than a pure one so the
	 * plot still reads as a panel rather than as a hole in the screen.
	 */
	chosen.background = QColor(0x16, 0x18, 0x1a);
	chosen.band_shade = QColor(0x1e, 0x24, 0x2b);
	chosen.grid = QColor(0x33, 0x37, 0x3b);
	chosen.axis_text = QColor(0xc2, 0xc6, 0xca);

	/*
	 * The two that would otherwise disappear. WU's greens and blues are
	 * chosen against white; on a dark ground the darker end of that set
	 * goes muddy, so those specific entries are lifted rather than the
	 * whole palette being reworked.
	 */
	chosen.band_observed = QColor(0x7a, 0xc8, 0x64);
	chosen.now_marker = QColor(0xff, 0xd4, 0x00);

	/*
	 * The same orange and a THIRD of the alpha (sec 16.29).
	 *
	 * At 80 over near-black the composite is #5f3c22, a warm brown that
	 * Weather Underground's red reads against at 1.89:1 -- so the curve
	 * inside a good grilling window, which is the answer this program
	 * exists to give, was the least legible thing on the plot. No alpha
	 * both clears the floor and leaves a wash anybody can see: the red
	 * needs 24 or under, and at 24 the band is 1.14:1 against the plot.
	 *
	 * So on dark the wash stops carrying the signal and the EDGES carry
	 * it, drawn at full strength where nothing is behind them. 22 is
	 * the most tint the floor allows rather than the least that shows,
	 * because what it has to do now is tint rather than announce.
	 *
	 * 22 and not 23, which also clears at 3.018:1. A gate and a value
	 * that agree to three decimal places agree about nothing: one
	 * rounding change anywhere in the chain and the tightest legal
	 * value becomes the loudest illegal one. 3.028:1 is not more
	 * legible, it is further from the edge of the cliff. The first
	 * draft of this line said 24 and the gate refused it at 2.98,
	 * which is the argument for the margin as much as for the gate.
	 */
	chosen.grill_window = QColor(0xff, 0x8b, 0x33, 22);

	/* The readout was already a dark box; on a dark ground it needs an
	 * edge to stay a box rather than a smudge. */
	chosen.readout_back = QColor(0x2b, 0x2f, 0x33);
	chosen.readout_edge = QColor(0x70, 0x76, 0x7c);
	chosen.day_divider = QColor(0xff, 0xff, 0xff);

	return chosen;
}

} // namespace



std::vector<qint64> bbq_day_boundaries(qint64 from_utc, qint64 to_utc,
                                       const QTimeZone &zone, int cap) {
	std::vector<qint64> found;

	if (to_utc <= from_utc || cap <= 0) {
		return found;
	}

	QDateTime cursor = local_time(from_utc, zone);
	cursor.setTime(QTime(0, 0));

	/* The first midnight at or after the left edge. */
	if (cursor.toSecsSinceEpoch() < from_utc) {
		cursor = cursor.addDays(1);
	}

	while (cursor.toSecsSinceEpoch() < to_utc &&
	       static_cast<int>(found.size()) < cap) {
		found.push_back(cursor.toSecsSinceEpoch());

		/*
		 * addDays, NOT plus 86400 seconds. A day is 23 or 25 hours on
		 * the two changeover nights, so a fixed stride walks off the
		 * boundary on the first of them and stays off it for the rest
		 * of the year -- a fault that would appear twice annually in a
		 * build nobody changed. Extracted from the paint code so that
		 * this can be asserted rather than merely claimed: see
		 * test_view.
		 */
		cursor = cursor.addDays(1);
	}

	return found;
}

bbq_forecast_graph::bbq_forecast_graph(QWidget *parent) : QWidget(parent) {
	/*
	 * 240, and the width is the part that matters (sec 10.6).
	 *
	 * It was 360, which is wider than some screens this runs on. A
	 * widget minimum is a floor under the WINDOW, so on a display
	 * narrower than the floor the window does not shrink to fit -- it
	 * overhangs, and everything past the edge is simply cut off.
	 *
	 * Found on the Fold's cover screen: 840x2289 physical at 420dpi is
	 * 320 logical pixels wide, so a 360-wide floor overhung it by 40 --
	 * about a ninth of the screen. Every control on the right lost its
	 * border, and the rain label "10 mm/h" was cut to "10". Nothing in
	 * the layout was wrong; there was no room for it to be right in.
	 *
	 * 240 sits below any phone screen this is likely to meet, so the
	 * layout decides the width and this floor stops mattering, which is
	 * what a minimum of this kind should do.
	 */
	setMinimumSize(240, 180);

	/* Without this the widget hears the mouse only while a button is down. */
	setMouseTracking(true);

	/*
	 * Pinch, which is what zooming IS on the device this now runs on.
	 *
	 * A wheel is a desktop instrument; a phone has two fingers and no
	 * wheel at all, so without this the zoom built in sec 13 simply does
	 * not exist there. Qt delivers it as a gesture rather than as raw
	 * touch, so the pinch centre and scale arrive already computed.
	 */
	grabGesture(Qt::PinchGesture);

	/*
	 * Weather Underground's own values, measured from their station
	 * dashboard rather than picked (sec 3.8.2).
	 *
	 * The temperature red is #d5202a, which also turns up as a brand
	 * token in their stylesheet -- two independent sightings of the
	 * same value, which is what made it worth trusting.
	 *
	 * Fixed rather than taken from the widget palette, and that is a
	 * real trade recorded in sec 3.8.3: the graph no longer follows the
	 * desktop into dark mode, because a white plot with pale blue hour
	 * bands IS the aesthetic sec 0 asked for.
	 */
	set_theme(bbq_theme::automatic);
}

QSize bbq_forecast_graph::sizeHint() const {
	return QSize(760, 280);
}

void bbq_forecast_graph::set_composite(bbq_composite composite) {
	m_composite = std::move(composite);

	/*
	 * The only thing the window scan depends on, so the only place it
	 * has to be forgotten (sec 16.57). The zone comes out of the
	 * composite too, which is what makes this one line rather than a
	 * set of them.
	 */
	m_windows_valid = false;
	m_windows.clear();

	update();
}

/*
 * Render the sample dot once per subpixel offset (sec 16.92).
 *
 * Cleared rather than rebuilt by the setters that invalidate it, so the
 * cost is paid on the next paint that wants one and not on a theme
 * change that may be followed by another.
 *
 * The stamp is drawn into a transparent premultiplied pixmap, so
 * blitting it composites exactly what drawing the two ellipses would:
 * an opaque interior and an antialiased edge that blends with whatever
 * the dot lands on.
 */
bool bbq_box_meets_polyline(const QRectF &box, const QPolygonF &line) {
	for (const QPointF &point : line) {
		if (point.x() < box.left() || point.x() > box.right()) {
			continue;
		}

		if (point.y() >= box.top() && point.y() <= box.bottom()) {
			return true;
		}
	}

	return false;
}

std::vector<QPixmap> bbq_dot_stamps(const QColor &ring, const QColor &fill,
                                    double radius, double ratio) {
	const double outer = radius + dot_ring_grow;

	/*
	 * Even, so that half of it is a whole pixel and the blit lands on an
	 * integer position. One pixel of margin each side for the
	 * antialiased edge, which reaches beyond the radius.
	 */
	int side = int(std::ceil(2.0 * outer)) + 2;
	if (side % 2 != 0) {
		++side;
	}

	std::vector<QPixmap> stamps;
	stamps.reserve(size_t(dot_stamp_offsets));

	for (int at = 0; at < dot_stamp_offsets; ++at) {
		/*
		 * Sized in DEVICE pixels and told the ratio, so the painter
		 * below works in logical coordinates and rasterises against the
		 * finer grid (sec 16.99). A pixmap left at the default ratio of
		 * 1 is drawn at its pixel size whatever surface it lands on,
		 * which on a HiDPI screen makes the dots about half the size
		 * they should be.
		 */
		QPixmap stamp(int(std::lround(side * ratio)),
		              int(std::lround(side * ratio)));
		stamp.setDevicePixelRatio(ratio);
		stamp.fill(Qt::transparent);

		QPainter into(&stamp);
		into.setRenderHint(QPainter::Antialiasing, true);
		into.setPen(Qt::NoPen);

		const QPointF middle(side / 2.0,
		                     side / 2.0 +
		                             double(at) / double(dot_stamp_offsets));

		into.setBrush(ring);
		into.drawEllipse(middle, outer, outer);
		into.setBrush(fill);
		into.drawEllipse(middle, radius, radius);

		stamps.push_back(stamp);
	}

	return stamps;
}

void bbq_forecast_graph::build_dot_stamps(double ratio) const {
	m_dot_stamps = bbq_dot_stamps(ground_behind(), m_palette.temperature,
	                              m_metrics.sample_radius, ratio);
	m_dot_stamp_ratio = ratio;
}

const std::vector<bbq_window> &bbq_forecast_graph::grill_windows() const {
	if (m_windows_valid) {
		return m_windows;
	}

	/*
	 * SCORED OVER THE DATA, NOT OVER THE VIEWPORT (sec 16.30.1), and
	 * computed ONCE PER COMPOSITE rather than once per frame
	 * (sec 16.57).
	 *
	 * It ran inside paintEvent, so a drag paid for it on every mouse
	 * move. Measured over the fixture: 2.2 ms for three days of span,
	 * 18.8 ms for thirty, 195 ms for a year -- linear in the span, and
	 * the span is the composite's, which grows as the archive fills.
	 * That is this program's purpose, so the cost was one that got
	 * worse the longer it was used.
	 *
	 * Nothing about the view enters it, which is what makes caching
	 * safe: the result is a function of the composite alone, and
	 * set_composite is the only place that changes.
	 */
	m_windows = bbq_grill_windows(m_composite, m_composite.zone(),
	                              m_composite.begin_utc(),
	                              m_composite.end_utc(), bbq_grill_policy());
	m_windows_valid = true;

	return m_windows;
}

void bbq_forecast_graph::set_corrected(bbq_series corrected) {
	m_corrected = std::move(corrected);
	update();
}

void bbq_forecast_graph::set_interpolation(bbq_interpolation method) {
	m_interpolation = method;
	update();
}

void bbq_forecast_graph::set_smoothing(int seconds) {
	m_smoothing_s = seconds;
	update();
}

void bbq_forecast_graph::set_show_windows(bool show) {
	m_show_windows = show;
	update();
}

void bbq_forecast_graph::set_scale_steadiness(int percent) {
	m_scale_steadiness = std::max(0, std::min(100, percent));

	/* Forget the held range, or the old one outlives the setting. */
	m_scale_held = false;
	update();
}

void bbq_forecast_graph::set_theme(bbq_theme theme) {
	m_theme = theme;
	apply_palette();
	update();
}

void bbq_forecast_graph::set_contrast_ground(const QColor &ground,
                                             double floor) {
	m_contrast_ground = ground;
	m_contrast_floor = floor;
	apply_palette();
	update();
}

/*
 * Rebuild the palette from the theme, then lift what has to stay
 * readable if a foreign ground has been named (sec 16.25).
 *
 * Rebuilt from the theme every time rather than clamped in place: a
 * clamp applied to an already-clamped colour would ratchet, and the
 * widget renders every five minutes.
 */
void bbq_forecast_graph::apply_palette() {
	m_palette = palette_for(bbq_theme_scheme(m_theme));
	m_dot_stamps.clear();

	if (!m_contrast_ground.isValid()) {
		return;
	}

	/*
	 * Exactly the inks tool/palette_contrast.py holds to the floor, plus
	 * the now-marker it lets under by two hundredths on light. That
	 * exception was allowed as a rounding difference against the
	 * palette's own background; against a ground this program did not
	 * choose it is not a rounding difference, so it is lifted here.
	 */
	QColor *const must_read[] = {
		&m_palette.axis_text,
		&m_palette.temperature,
		&m_palette.corrected,
		&m_palette.day_divider,
		&m_palette.stale_warning,
		&m_palette.now_marker,
	};

	for (QColor *ink : must_read) {
		*ink = bbq_ensure_contrast(*ink, m_contrast_ground,
		                           m_contrast_floor);
	}
}

void bbq_forecast_graph::set_show_wind(bool show) {
	m_show_wind = show;
	update();
}

void bbq_forecast_graph::set_show_samples(bool show) {
	m_show_samples = show;
	update();
}

void bbq_forecast_graph::set_cursor_column(int column) {
	m_cursor_column = column;
	update();
}

void bbq_forecast_graph::mouseMoveEvent(QMouseEvent *event) {
	if (m_dragging && m_plot.width() > 0) {
		/*
		 * Throw the plot sideways. The seconds under the grab point stay
		 * under it, so the graph tracks the hand rather than scrolling
		 * at some rate of its own.
		 */
		const double per_pixel =
		        static_cast<double>(view_span_s()) / m_plot.width();
		const double moved = event->position().x() - m_drag_x;

		m_view_span_s = view_span_s();
		m_view_from = m_drag_from - static_cast<qint64>(moved * per_pixel);
		m_follow_now = false;

		emit view_changed(view_from_utc(), view_from_utc() + view_span_s());
	}

	const int column = static_cast<int>(event->position().x()) - m_metrics.margin_left;
	m_cursor_column = column;
	update();
	QWidget::mouseMoveEvent(event);
}

void bbq_forecast_graph::leaveEvent(QEvent *event) {
	m_cursor_column = -1;
	update();
	QWidget::leaveEvent(event);
}

void bbq_forecast_graph::set_layout(bbq_layout layout) {
	m_metrics = bbq_metrics_for(layout);
	m_dot_stamps.clear();
	m_before_s = m_metrics.window_before_s;
	m_after_s = m_metrics.window_after_s;
	update();
}

qint64 bbq_forecast_graph::view_span_s() const {
	if (!m_follow_now && m_view_span_s > 0) {
		return m_view_span_s;
	}

	return m_before_s + m_after_s;
}

qint64 bbq_forecast_graph::view_from_utc() const {
	if (!m_follow_now && m_view_span_s > 0) {
		return m_view_from;
	}

	return QDateTime::currentSecsSinceEpoch() - m_before_s;
}

void bbq_forecast_graph::set_view(qint64 from_utc, qint64 span_s) {
	/*
	 * Bounded at both ends. Fifteen minutes is about a pixel per second
	 * at this width and there is nothing finer to look at; ten years is
	 * further back than any station here has data for, and an unbounded
	 * zoom-out turns the whole history into one column of ink.
	 */
	const qint64 shortest = 15 * 60;
	const qint64 longest = 10LL * 365 * 24 * 3600;

	m_view_span_s = std::max(shortest, std::min(longest, span_s));
	m_view_from = from_utc;
	m_follow_now = false;

	emit view_changed(view_from_utc(), view_from_utc() + view_span_s());
	update();
}

void bbq_forecast_graph::follow_now() {
	m_follow_now = true;
	m_view_span_s = 0;

	emit view_changed(view_from_utc(), view_from_utc() + view_span_s());
	update();
}

void bbq_forecast_graph::mousePressEvent(QMouseEvent *event) {
	if (event->button() == Qt::LeftButton && m_plot.width() > 0) {
		m_dragging = true;
		m_drag_x = event->position().x();
		m_drag_from = view_from_utc();
		setCursor(Qt::ClosedHandCursor);
	}

	QWidget::mousePressEvent(event);
}

void bbq_forecast_graph::mouseReleaseEvent(QMouseEvent *event) {
	if (event->button() == Qt::LeftButton && m_dragging) {
		m_dragging = false;
		unsetCursor();
	}

	QWidget::mouseReleaseEvent(event);
}

void bbq_forecast_graph::mouseDoubleClickEvent(QMouseEvent *event) {
	/*
	 * The way back. A view that has been panned into last March needs
	 * one gesture to return, and a mode nobody can find is not one.
	 */
	follow_now();
	QWidget::mouseDoubleClickEvent(event);
}

bool bbq_forecast_graph::event(QEvent *event) {
	if (event->type() != QEvent::Gesture) {
		return QWidget::event(event);
	}

	QGestureEvent *gestures = static_cast<QGestureEvent *>(event);
	QGesture *found = gestures->gesture(Qt::PinchGesture);
	if (found == nullptr || m_plot.width() <= 0) {
		return QWidget::event(event);
	}

	QPinchGesture *pinch = static_cast<QPinchGesture *>(found);

	/*
	 * The same invariant the wheel holds (sec 13.1.1): the moment under
	 * the fingers stays under them. The centre point is where the pinch
	 * is happening, so it is the anchor -- zooming about the middle of
	 * the widget instead would slide the thing being pinched away from
	 * the fingers doing it, which feels broken in a way a screenshot
	 * cannot show.
	 */
	const double where = pinch->centerPoint().x() - m_plot.left();
	const double offset = std::max(0.0, std::min(
	        static_cast<double>(m_plot.width()), where));

	const qint64 span = view_span_s();
	const qint64 from = view_from_utc();
	const double anchor = from + offset * (static_cast<double>(span) / m_plot.width());

	const double scale = pinch->scaleFactor();
	if (scale > 0.0) {
		/*
		 * Fingers apart means see MORE detail, so the span shrinks --
		 * the reciprocal, not the factor itself.
		 */
		set_view(0, static_cast<qint64>(span / scale));

		const double per_pixel =
		        static_cast<double>(m_view_span_s) / m_plot.width();
		m_view_from = static_cast<qint64>(anchor - offset * per_pixel);

		emit view_changed(view_from_utc(), view_from_utc() + view_span_s());
		update();
	}

	gestures->accept(Qt::PinchGesture);
	return true;
}

void bbq_forecast_graph::wheelEvent(QWheelEvent *event) {
	if (m_plot.width() <= 0) {
		QWidget::wheelEvent(event);
		return;
	}

	/*
	 * Zoom about the cursor: whatever moment is under the pointer stays
	 * under it. Zooming about the centre instead means the thing being
	 * looked at slides away exactly when it is being examined.
	 */
	const double offset = std::max(0.0, std::min(
	        static_cast<double>(m_plot.width()),
	        event->position().x() - m_plot.left()));

	const qint64 span = view_span_s();
	const qint64 from = view_from_utc();
	const double anchor = from + offset * (static_cast<double>(span) / m_plot.width());

	const double steps = event->angleDelta().y() / 120.0;
	const double scale = std::pow(1.0 / 1.25, steps);
	const qint64 wanted = static_cast<qint64>(span * scale);

	set_view(0, wanted);

	/* set_view has clamped the span; the anchor is held against it. */
	const double per_pixel = static_cast<double>(m_view_span_s) / m_plot.width();
	m_view_from = static_cast<qint64>(anchor - offset * per_pixel);

	emit view_changed(view_from_utc(), view_from_utc() + view_span_s());
	update();
	event->accept();
}

QString bbq_readout_time_label(qint64 first_utc, qint64 last_utc,
                              int knot_count, bool wide,
                              const QTimeZone &zone) {
	const QDateTime first = local_time(first_utc, zone);

	/*
	 * One sample in the column: its own start time, unchanged. The
	 * reason is in the column struct -- deriving the time from the
	 * cursor's position put 05:59 in the readout for a sample stamped
	 * 06:00.
	 */
	if (knot_count <= 1) {
		return first.toString(wide ? QStringLiteral("ddd HH:mm")
		                           : QStringLiteral("HH:mm"));
	}

	/*
	 * SEVERAL, so say so. Sec 13.2 stops drawing the sample marks once
	 * they would crowd, because dots that merge claim a density of
	 * measurement nobody made -- and one sample's timestamp printed
	 * beside the mean of twenty is the same claim in text, where it is
	 * harder to notice because a single time looks measured.
	 *
	 * A range cannot be misread that way, and unlike the marks the
	 * readout has room to be explicit rather than absent.
	 */
	const QDateTime last = local_time(last_utc, zone);

	if (first.date() == last.date()) {
		return (wide ? first.toString(QStringLiteral("ddd HH:mm"))
		             : first.toString(QStringLiteral("HH:mm"))) +
		       QStringLiteral("-") + last.toString(QStringLiteral("HH:mm"));
	}

	/*
	 * Across days the weekday stops being enough: a column at the
	 * ten-year ceiling spans about eleven days, so "Mon-Mon" would be
	 * true of two Mondays and useful about neither.
	 */
	if (wide) {
		return first.toString(QStringLiteral("d MMM HH:mm")) +
		       QStringLiteral("-") +
		       last.toString(QStringLiteral("d MMM HH:mm"));
	}

	return first.toString(QStringLiteral("d MMM")) + QStringLiteral("-") +
	       last.toString(QStringLiteral("d MMM"));
}

bool bbq_day_furniture_fits(double closest_days_px, double widest_name_px) {
	/*
	 * Fewer than two midnights in view: nothing can crowd, and the
	 * caller passes a negative gap to say so.
	 */
	if (closest_days_px < 0.0) {
		return true;
	}

	/* One name, plus the gap the label already leaves beside a divider. */
	return closest_days_px >= widest_name_px + 6.0;
}

double bbq_readout_box_x(double centre_px, double box_w,
                         double plot_left, double plot_right) {
	double box_x = centre_px - box_w / 2.0;

	/*
	 * RIGHT FIRST, THEN LEFT, and the order is the whole point --
	 * whichever runs last wins when the box cannot fit. See the
	 * header.
	 */
	box_x = std::min(box_x, plot_right - box_w - 2.0);
	box_x = std::max(box_x, plot_left + 2.0);

	return box_x;
}

void bbq_forecast_graph::set_opaque_background(bool opaque) {
	if (m_opaque_background == opaque) {
		return;
	}

	m_opaque_background = opaque;
	update();
}

void bbq_forecast_graph::paintEvent(QPaintEvent *event) {
	QPainter painter(this);

	/*
	 * ANTIALIASED THROUGHOUT, WHICH IS NOT FREE AND IS WORTH IT
	 * (sec 16.89).
	 *
	 * Measured with an interleaved A/B inside one process, in CPU time
	 * so machine load cannot reach it: turning the hint off for the
	 * whole paint saved a THIRD of it -- 26.5ms against 17.7ms -- and
	 * nearly all of that was bought back for nothing, because most of
	 * this chart is axis-aligned and an antialiased vertical rule is a
	 * vertical rule with coverage arithmetic run over it.
	 *
	 * It is on anyway, and the reason is that the saving evaporated
	 * once the shapes underneath were fixed. The expensive antialiased
	 * things were a dashed rule stroked per window, a translucent
	 * rectangle stroked per column and a line stroked per column of
	 * ribbon; with those drawn as the fills they always were, the hint
	 * costs about a tenth of the paint rather than a third. A tenth is
	 * not worth changing how the drawing looks -- thin rules would go
	 * from soft and half-strength to crisp and full-strength, which is
	 * a decision about appearance and not one a speed pass gets to
	 * make in passing.
	 */
	painter.setRenderHint(QPainter::Antialiasing);
	if (m_opaque_background) {
		painter.fillRect(event->rect(), m_palette.background);
	}

	/*
	 * The right gutter is measured, not chosen.
	 *
	 * It holds "1.0 mm/h" and "rain %", and a metric picked to look
	 * narrow clipped the first to "1.0 mm" and then to "1.0 mm/l" --
	 * a units label losing the units, which is the only part of it
	 * carrying information. The layout supplies a floor; the text
	 * decides the rest, the same way the tray icon's digits do.
	 */
	const QFont gutter_font = scaled_font(font(), m_metrics.label_scale);
	const QFontMetrics gutter(gutter_font);
	const int widest = qMax(gutter.horizontalAdvance(QStringLiteral("0.0 mm/h")),
	                        gutter.horizontalAdvance(tr("rain %")));
	/*
	 * On mobile the plot takes the whole width and the axis numbers are
	 * drawn ON TOP of it (project.md sec 10.5).
	 *
	 * A gutter is a frame by another name. On a phone the left margin
	 * and the measured right one together take a tenth of the screen to
	 * hold four short labels, and that tenth is plot -- the difference
	 * between reading an evening and squinting at it. The labels do not
	 * need their own room; they need to be legible, which is a question
	 * about contrast rather than about space.
	 */
	const bool overlay_labels = m_metrics.stack_controls;

	const int margin_right =
	        overlay_labels ? 0 : qMax(m_metrics.margin_right, widest + 10);
	const int margin_left = overlay_labels ? 0 : m_metrics.margin_left;

	/*
	 * ONE PANEL (sec 3.19).
	 *
	 * Rain chance had a strip of its own below the plot, 46 pixels
	 * tall, which made a dry day's forecast three quarters temperature
	 * and a quarter of almost nothing. Every series shares the height
	 * now, each on its own scale, which is what WU's forecast chart
	 * does and what makes a rain area readable at all on a phone.
	 *
	 * Kept as a named zero rather than deleted, because the rest of the
	 * geometry is expressed relative to the stack and reading `0` at
	 * the definition is how the next person sees there is no second
	 * panel any more.
	 */
	const int stack = 0;
	const QRect plot(margin_left, margin_top,
	                 width() - margin_left - margin_right,
	                 height() - margin_top - m_metrics.margin_bottom - stack);
	/*
	 * The same rectangle. Everything positioned below the chart --
	 * the ribbon, the clock, the now marker's foot -- was written
	 * against the lower panel's bottom edge, and that edge is the
	 * plot's now.
	 */
	const QRect chance_plot = plot;

	if (plot.width() < 20 || plot.height() < 20) {
		return;
	}

	m_plot = plot;

	const QTimeZone zone = m_composite.zone();
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	const qint64 from = view_from_utc();
	const qint64 to = from + view_span_s();

	if (m_composite.is_empty()) {
		painter.setPen(m_palette.axis_text);
		painter.drawText(rect(), Qt::AlignCenter,
		                 tr("No forecast data yet."));
		return;
	}


	/* One pass over the columns; every later pass reads this. */
	std::vector<column> columns;
	columns.reserve(plot.width());

	const double seconds_per_pixel =
	        static_cast<double>(to - from) / plot.width();

	for (int x = 0; x < plot.width(); ++x) {
		const qint64 start = from + static_cast<qint64>(x * seconds_per_pixel);
		const qint64 end = from + static_cast<qint64>((x + 1) * seconds_per_pixel);
		columns.push_back(reduce(m_composite, start, end == start ? end + 1 : end));
	}


	curve_spec spec;
	spec.method = m_interpolation;
	spec.smooth_columns = m_smoothing_s / seconds_per_pixel;

	spec.which = quantity::temperature;
	apply_curve(columns, spec);
	spec.which = quantity::rain;
	apply_curve(columns, spec);
	spec.which = quantity::chance;
	apply_curve(columns, spec);
	spec.which = quantity::wind;
	apply_curve(columns, spec);

	/* Scales, from what is actually visible rather than from the whole set. */
	double temperature_low = 0.0;
	double temperature_high = 0.0;
	bool have_temperature = false;

	/*
	 * The rain PEAK in view, which is no longer a scale (sec 3.16).
	 * It is kept only to say when the trace has been clamped.
	 */
	double rain_peak = 0.0;
	double wind_high = 1.0;

	for (const column &c : columns) {
		if (c.has_wind) {
			wind_high = std::max(wind_high, c.wind);
		}

		if (c.has_temperature) {
			if (!have_temperature) {
				temperature_low = c.temperature;
				temperature_high = c.temperature;
				have_temperature = true;
			}
			temperature_low = std::min(temperature_low, c.temperature);
			temperature_high = std::max(temperature_high, c.temperature);
		}
		if (c.has_rain) {
			rain_peak = std::max(rain_peak, c.rain);
		}
	}

	/*
	 * The corrected overlay counts towards the scales, because it is
	 * drawn on them.
	 *
	 * It did not, and a correction large enough to leave the axis range
	 * ran off the top of the plot and was clipped by the widget edge --
	 * a curve that simply stops, which reads as a rendering fault rather
	 * than as a value out of range. Whatever is drawn has to fit; the
	 * alternative is deciding not to draw it, and that is not a decision
	 * a scale should make silently.
	 */
	for (const bbq_sample &sample : m_corrected.samples()) {
		if (sample.start_utc < from || sample.start_utc >= to) {
			continue;
		}

		if (sample.temperature.has_value()) {
			if (!have_temperature) {
				temperature_low = *sample.temperature;
				temperature_high = *sample.temperature;
				have_temperature = true;
			}

			temperature_low = std::min(temperature_low, *sample.temperature);
			temperature_high = std::max(temperature_high, *sample.temperature);
		}

		if (sample.precip_rate.has_value()) {
			rain_peak = std::max(rain_peak, *sample.precip_rate);
		}

		if (sample.wind_kph.has_value()) {
			wind_high = std::max(wind_high, *sample.wind_kph);
		}
	}

	if (!have_temperature) {
		temperature_low = 0.0;
		temperature_high = 1.0;
	}

	if (temperature_high - temperature_low < 4.0) {
		const double middle = (temperature_high + temperature_low) / 2.0;
		temperature_low = middle - 2.0;
		temperature_high = middle + 2.0;
	}

	/*
	 * Hold the scale still (project.md sec 3.14).
	 *
	 * The range above is exactly what is visible, recomputed every
	 * paint -- so dragging slides new extremes in and out and the axis
	 * moves under the curve continuously. That is precise and unreadable:
	 * the eye cannot tell a temperature rising from an axis falling.
	 *
	 * Two things fix it together. The range is rounded OUTWARD to a
	 * quantum, so it changes in visible steps rather than continuously;
	 * and the previous range is kept while it still contains the data
	 * and is not wastefully large, so scrolling within it moves nothing
	 * at all.
	 *
	 * At 0 this is off entirely, because "follows the data exactly" is a
	 * legitimate thing to want and is what every version before this did.
	 */
	if (m_scale_steadiness > 0) {
		const double fraction = m_scale_steadiness / 100.0;
		const double quantum = 1.0 + 4.0 * fraction;

		double stepped_low = std::floor(temperature_low / quantum) * quantum;
		double stepped_high = std::ceil(temperature_high / quantum) * quantum;

		/*
		 * Keep what was there if it still fits and has not become much
		 * bigger than needed -- the second half matters, or a scale
		 * stretched once by a hot afternoon would stay stretched all
		 * week.
		 */
		const bool still_fits = m_scale_held &&
		                        m_scale_low <= temperature_low &&
		                        m_scale_high >= temperature_high;
		const double slack = (stepped_high - stepped_low) + 2.0 * quantum;

		if (still_fits && (m_scale_high - m_scale_low) <= slack) {
			stepped_low = m_scale_low;
			stepped_high = m_scale_high;
		}

		m_scale_low = stepped_low;
		m_scale_high = stepped_high;
		m_scale_held = true;

		temperature_low = stepped_low;
		temperature_high = stepped_high;
	}

	const double temperature_pad = (temperature_high - temperature_low) * 0.12;
	temperature_low -= temperature_pad;
	temperature_high += temperature_pad;

	const auto y_for_temperature = [&](double value) {
		const double span = temperature_high - temperature_low;
		const double t = (value - temperature_low) / span;
		return plot.bottom() - t * plot.height();
	};

	/*
	 * Wind gets its own axis rather than being hung off one of the
	 * others, which is the objection sec 3 records against putting a
	 * percentage on the temperature scale: a scale that means two things
	 * means neither. Its maximum is labelled in the right gutter beside
	 * the rain's.
	 */
	const auto y_for_wind = [&](double value) {
		const double t = value / wind_high;
		return plot.bottom() - t * plot.height();
	};

	const auto y_for_rain = [&](double value) {
		/*
		 * Clamped, so a rate past full scale draws at the top rather
		 * than off the panel. The label says when that has happened.
		 */
		const double t = std::min(1.0, value / rain_full_scale_mm_h);
		return plot.bottom() - t * plot.height();
	};

	/*
	 * Alternating three-hour bands, which is the most recognisable
	 * thing about the WU chart and the cheapest density cue there is:
	 * it gives the eye a ruler without adding a single line.
	 */
	/*
	 * Both the shading and the ticks follow the same chosen step, so the
	 * bands stay one tick wide at every zoom rather than becoming
	 * enormous blocks when the view narrows.
	 */
	const bbq_tick_choice ticks = bbq_ticks_for(to - from, plot.width() / 90);
	const qint64 band_step = ticks.step_s;
	const qint64 first_band = (from / band_step) * band_step;

	/*
	 * TICKS AT THE EDGES, not alternating blocks (sec 3.20).
	 *
	 * The banding was measured from WU's chart and is the cheapest
	 * density cue there is -- but it tints half the plot, and every
	 * other thing drawn on it is then read against two grounds instead
	 * of one. On a dark theme with a rain wash over the top that is
	 * three tints deep before any data is drawn.
	 *
	 * Short marks at the top and bottom edges say the same thing and
	 * leave the middle -- where the data is -- plain. They are drawn
	 * with the time axis below, at the LABELLED hours, so a mark and
	 * the text naming it are the same event; marking the band step as
	 * well was the first attempt and put ticks where nothing was
	 * written.
	 */
	Q_UNUSED(first_band);

	/*
	 * The grilling windows (sec 7), drawn under the data rather than
	 * over it. A recommendation should be the background a reading is
	 * seen against, not something covering it up.
	 */

	if (m_show_windows) {
		/*
		 * SCORED OVER THE DATA, NOT OVER THE VIEWPORT (sec 16.30.1).
		 *
		 * This passed `from` and `to` -- the visible range -- and
		 * bbq_grill_windows only scans what it is given, so a window
		 * running past the edge of the view was closed AT the edge and
		 * reported as starting there. Three things followed, and the
		 * middle one is the one that matters:
		 *
		 *   - the edge rules were drawn at the screen boundary, which
		 *     is the claim the clipping test beside them exists to
		 *     prevent, made upstream of where that check can see it;
		 *   - the window's score and rank were computed from the
		 *     visible part only, so scrolling changed how good the
		 *     program said an afternoon was;
		 *   - and the header, which scores over a fixed three days
		 *     from now, could name a different window from the one the
		 *     plot was shading.
		 *
		 * The answer must not depend on where somebody has scrolled to,
		 * so it is asked over everything the composite covers. That is
		 * at most a week or so at a ten-minute stride, which is cheap,
		 * and it is the same question the header asks.
		 */
		const std::vector<bbq_window> &windows = grill_windows();

		QColor shade = m_palette.grill_window;

		for (const bbq_window &window : windows) {
			const double x0 = plot.left() + (window.start_utc - from) / seconds_per_pixel;
			const double x1 = plot.left() + (window.end_utc - from) / seconds_per_pixel;
			const double left = std::max(x0, static_cast<double>(plot.left()));
			const double right = std::min(x1, static_cast<double>(plot.right()));

			if (right <= left) {
				continue;
			}

			/*
			 * Stronger for a better window, so the ranking is visible
			 * rather than only knowable by hovering. Scaled to the
			 * palette's cap rather than to a literal, and running from
			 * a third of it to all of it -- which on light reproduces
			 * the 26-to-80 this used to hard-code, and on dark stays
			 * under what the temperature curve can be read against.
			 */
			const int cap = m_palette.grill_window.alpha();
			shade.setAlpha(
			        qRound(cap * (1.0 + 2.0 * window.rank) / 3.0));

			const double tall = chance_plot.bottom() - plot.top();
			/*
			 * FLATTENED WHERE THE GROUND IS KNOWN (sec 16.89).
			 *
			 * Nothing is under this but the background fill: the only
			 * other draw above it returns first, on an empty
			 * composite. So where the background is opaque the blend
			 * has one possible answer and is computed once per window
			 * instead of once per pixel.
			 *
			 * Worth 4.4ms of a 17.8ms paint at a sixteen-day view.
			 * Where the background is NOT opaque -- the home-screen
			 * picture, which lets a wallpaper through -- there is no
			 * known ground and the translucent fill stands.
			 */
			painter.fillRect(QRectF(left, plot.top(), right - left, tall),
			                 m_opaque_background
			                         ? bbq_flatten_over(shade, m_palette.background)
			                         : shade);

			/*
			 * THE EDGES, at full strength (sec 16.29).
			 *
			 * A wash cannot say "here" on a dark plot: the alpha that
			 * keeps the curve legible over it is an alpha nobody can
			 * see. An edge has nothing behind it but the ground, so it
			 * can be as strong as it likes, and two rules bracketing a
			 * span say what a fill says.
			 *
			 * Only where the boundary is really there. A window running
			 * off the side of the view has been clipped, and a rule at
			 * the clip would claim it starts where the screen does.
			 *
			 * DASHED, because there are three kinds of vertical rule on
			 * this plot now and two of them are a colour apart. Solid
			 * amber is now, solid white is midnight, and a window's
			 * ends were solid orange -- which is a hue away from now
			 * and nothing else, so telling them apart meant naming the
			 * colours. Dashes are a difference somebody can see without
			 * being told, and they survive the one place colour does
			 * not: the home-screen picture, where a wallpaper shows
			 * through everything at a quarter strength.
			 *
			 * Distinct from the grid, which is dotted, for the same
			 * reason.
			 */
			QColor edge = m_palette.grill_window;
			edge.setAlpha(255);

			const double thick = 2.0;
			const double bottom = plot.top() + tall;

			/*
			 * THE DASHES ARE FILLED, NOT STROKED (sec 16.89).
			 *
			 * Qt::DashLine sends the line through the dash generator,
			 * which turns one rule into a polygon per dash and hands
			 * each to the rasteriser. A window edge is about five
			 * hundred pixels of a twelve-pixel period, so that is
			 * forty-odd polygons per edge and eighty per window --
			 * and at a sixteen-day view this block was 13.5ms of a
			 * 30ms paint, the largest single item in it.
			 *
			 * Every one of those polygons is an upright rectangle two
			 * pixels wide, which is the raster engine's fastest path
			 * when it is asked for directly. So ask for it directly.
			 *
			 * The pattern is Qt's own for DashLine -- {4, 2} in units
			 * of pen width, so 8 on and 4 off at width 2 -- and
			 * FlatCap is what makes a dash exactly its own length
			 * rather than its length plus a cap at each end. Both are
			 * reproduced here rather than approximated, because the
			 * point of the change is that nothing about it is visible.
			 */
			const double dash = 4.0 * thick;
			const double gap = 2.0 * thick;
			const auto dashed_rule = [&](double x) {
				for (double y = plot.top(); y < bottom; y += dash + gap) {
					const double end = std::min(y + dash, bottom);
					painter.fillRect(
					        QRectF(x - thick / 2.0, y, thick, end - y),
					        edge);
				}
			};

			if (x0 >= plot.left()) {
				dashed_rule(x0);
			}
			if (x1 <= plot.right()) {
				dashed_rule(x1);
			}
		}
	}



	/* --- grid and time axis ------------------------------------------- */
	painter.setPen(QPen(m_palette.grid, 1, Qt::DotLine));

	const QFont label_font = scaled_font(font(), m_metrics.label_scale);
	painter.setFont(label_font);

	/*
	 * One way of drawing an edge label, so the two shapes cannot drift.
	 *
	 * Overlaid text needs a ground of its own or it competes with
	 * whatever the curve is doing behind it -- a translucent plate in
	 * the plot's own background colour keeps the label readable without
	 * hiding the data, and costs nothing where there is a gutter to
	 * draw in instead.
	 */
	const auto edge_label = [&](double x, double y, double wide,
	                            Qt::Alignment align, const QString &text) {
		const QRectF box(x, y, wide, 14);

		if (overlay_labels) {
			const QFontMetrics measured(label_font);
			const int text_wide = measured.horizontalAdvance(text);

			double left = box.left();
			if (align.testFlag(Qt::AlignRight)) {
				left = box.right() - text_wide;
			}

			QColor plate = m_palette.background;
			plate.setAlpha(200);

			painter.setPen(Qt::NoPen);
			painter.setBrush(plate);
			painter.drawRect(QRectF(left - 3, box.top(), text_wide + 6,
			                        box.height()));
			painter.setBrush(Qt::NoBrush);
			painter.setPen(m_palette.axis_text);
		}

		painter.drawText(box, align | Qt::AlignVCenter, text);
	};

	/*
	 * Where an edge label sits: inside the plot when there is no gutter,
	 * in the gutter when there is one.
	 */
	const double left_label_x = overlay_labels ? plot.left() + 4 : 2;
	const double left_label_wide =
	        overlay_labels ? 80 : m_metrics.margin_left - 6;
	const Qt::Alignment left_align =
	        overlay_labels ? Qt::AlignLeft : Qt::AlignRight;

	const double right_label_wide = overlay_labels ? 90 : margin_right - 6;
	const double right_label_x = overlay_labels
	                                     ? plot.right() - right_label_wide - 4
	                                     : width() - margin_right + 4;
	const Qt::Alignment right_align =
	        overlay_labels ? Qt::AlignRight : Qt::AlignLeft;


	/*
	 * Say which clock. A graph in somebody else's timezone that does
	 * not admit it is the whole of sec 3.12.1, and the fix is not worth
	 * much if the reader cannot tell it has been applied.
	 *
	 * Computed HERE, well above where it is drawn, because the tick
	 * labels have to know how much room it takes: it shares their row,
	 * and on a phone it shares their space as well.
	 */
	QString clock = tr("local");
	if (zone.isValid()) {
		clock = zone.abbreviation(QDateTime::currentDateTime());
		if (clock.isEmpty()) {
			clock = QString::fromUtf8(zone.id());
		}

		/*
		 * A zone with no name abbreviates to "UTC+02:00", which does
		 * not fit and was clipped to "C+02:00" -- a label that looks
		 * like a typo rather than a truncation. The offset alone says
		 * the same thing in the space available.
		 */
		if (clock.startsWith(QStringLiteral("UTC"))) {
			clock = clock.mid(3);
		}
	}

	/*
	 * How far left a tick label may reach, which is NOT always the plot
	 * edge.
	 *
	 * With a gutter the zone name sits in it, outside the plot, and the
	 * plot edge is the true limit. Edge to edge on a phone there is no
	 * gutter: the zone name is drawn INSIDE the plot, on the same row as
	 * these labels, and a tick that merely cleared plot.left() landed on
	 * top of it -- "CEST" and "11:00" printed over each other as
	 * "CEST |1:00", which reads as a rendering fault rather than as two
	 * labels wanting the same space.
	 *
	 * Measured from the zone string rather than from the 80-pixel box it
	 * is drawn in, so a short name like "CEST" costs one tick label and
	 * not three.
	 */
	const QFontMetrics zone_measured(label_font);
	const double tick_left_limit =
	        overlay_labels
	                ? left_label_x + zone_measured.horizontalAdvance(clock) + 6.0
	                : plot.left();

	const qint64 tick_step = ticks.step_s;
	const qint64 first_tick = ((from / tick_step) + 1) * tick_step;

	std::vector<double> hour_marks;

	for (qint64 t = first_tick; t < to; t += tick_step) {
		const double x = plot.left() + (t - from) / seconds_per_pixel;
		/*
		 * SHORT MARKS AT THE EDGES, not a line through the plot
		 * (sec 3.20). A dotted rule the full height of the chart
		 * crosses every curve on it, and with the banding gone it was
		 * the last thing tinting the middle.
		 *
		 * Collected here and DRAWN LATER, after the series. The rain
		 * area starts at the bottom edge, so a mark drawn with the axis
		 * is under it -- and the lower half of the cue disappeared on
		 * exactly the days it rained. Seven pixels of furniture at the
		 * extreme edge is the one thing worth putting over the data.
		 */
		hour_marks.push_back(x);

		const QString stamp = local_time(t, zone).toString(ticks.format);

		/*
		 * The box is MEASURED from the text rather than assumed.
		 *
		 * It used to be a hardcoded 48 pixels with the stamp centred in
		 * it, so any stamp wider than the box overflowed and was cut at
		 * both ends: "Wed 14:00" came out as "Ved 14:0". "Fri" fits in
		 * 48 and "Wed", "Thu" and "Sat" do not, which is why the fault
		 * looked like it followed the day of the week rather than the
		 * layout -- and why it survived: a third of the labels were
		 * always correct.
		 *
		 * The note this replaces recorded the identical glyph loss at
		 * the right margin -- "Wed 02:00" clipped to "Ned 02:00" -- and
		 * answered it by dropping labels near the edges. That hid the
		 * two labels where it had been noticed and left every clipped
		 * label in the middle of the graph exactly as it was. The
		 * measurement fixes both, so the margin test below uses it too
		 * and now drops a label only when it would genuinely reach past
		 * a margin rather than when it sits within a guessed 24 pixels
		 * of one.
		 */
		const QFontMetrics tick_measured(label_font);
		const double tick_half =
		        tick_measured.horizontalAdvance(stamp) / 2.0 + 2.0;

		/*
		 * A tick label that would reach past either limit is left out.
		 * Dropping one costs nothing -- its neighbour is one step away
		 * and says the same kind of thing.
		 */
		if (x - tick_half < tick_left_limit || x + tick_half > plot.right()) {
			continue;
		}

		painter.setPen(m_palette.axis_text);
		const QRectF label(x - tick_half,
		                   chance_plot.bottom() + m_metrics.ribbon_height + 3,
		                   tick_half * 2.0, 14);
		painter.drawText(label, Qt::AlignCenter, stamp);
	}

	/*
	 * Midnight, in the LOCATION's clock (sec 3.12.1 and sec 3.15).
	 *
	 * Stepped a day at a time through QDateTime rather than by adding
	 * 86400 seconds, because a day is not always 86400 seconds long: on
	 * the two changeover nights it is 23 or 25 hours, and a fixed stride
	 * would walk the divider an hour off the boundary and keep it there
	 * for the rest of the year.
	 */
	const std::vector<qint64> midnights = bbq_day_boundaries(from, to, zone);

	/*
	 * AND DROPPED WHOLESALE ONCE THE DAYS CROWD (sec 16.56).
	 *
	 * Sec 13.2 already stops the sample marks when they would merge,
	 * because dots at twenty to a pixel claim a density of measurement
	 * nobody made. The day furniture had no such rule, and the span
	 * became the reader's to choose in sec 16.28: at a year it is 365
	 * dividers across 780 pixels, which draws the plot as a barcode and
	 * the names as a smear.
	 *
	 * The threshold is not a number picked here. The comment on the
	 * name loop already says a divider without a name "only says
	 * something changed here" -- so the two stand or fall together, and
	 * what decides it is whether a name still fits between one midnight
	 * and the next. Measured from the names actually about to be drawn,
	 * so it follows the font and the display rather than an assumption
	 * about either.
	 *
	 * The axis ticks keep their own labels at every span, so a wide
	 * view still says where it is. Nothing is lost but the hatching.
	 */
	QFont day_font = label_font;
	day_font.setBold(true);
	const QFontMetrics day_measured(day_font);

	double widest_day_name = 0.0;
	double closest_days = -1.0;
	for (std::size_t i = 0; i < midnights.size(); ++i) {
		const QString name =
		        local_time(midnights[i], zone).toString(QStringLiteral("ddd d"));
		widest_day_name = std::max(
		        widest_day_name,
		        static_cast<double>(day_measured.horizontalAdvance(name)));

		if (i > 0) {
			const double gap =
			        (midnights[i] - midnights[i - 1]) / seconds_per_pixel;
			if (closest_days < 0.0 || gap < closest_days) {
				closest_days = gap;
			}
		}
	}

	const bool days_would_crowd =
	        !bbq_day_furniture_fits(closest_days, widest_day_name);

	for (qint64 midnight : days_would_crowd ? std::vector<qint64>() : midnights) {
		const double x = plot.left() + (midnight - from) / seconds_per_pixel;

		/*
		 * WIDE AND QUIET, not thin and dark.
		 *
		 * The first version was a 2px line at the strongest contrast on
		 * the plot, on the reasoning that a boundary must outrank the
		 * ruler it interrupts. That was the wrong axis to argue on: it
		 * made the divider the most emphatic LINE in a picture whose
		 * lines are otherwise data, so it read as a measurement of
		 * something rather than as structure.
		 *
		 * The differentiator is form. The grid is thin and dotted; this
		 * is broad and solid, at a contrast close to the rest of the
		 * furniture, so the eye files it with the background where it
		 * belongs. What actually answers "which day is this" is the
		 * name, which stays bold on its plate -- the band only has to
		 * say where the day starts, and a band is legible at a contrast
		 * far below what a line needs.
		 *
		 * It still runs the full height of both panels, so a day is one
		 * column all the way down.
		 */
		/*
		 * A THIN CUT rather than a six-pixel band (sec 3.20). The band
		 * was legible at low contrast because it was wide; a cut is
		 * legible because it is sharp, and it takes no plot with it.
		 */
		painter.setPen(QPen(m_palette.day_divider, 1.0, Qt::SolidLine,
		                    Qt::FlatCap));
		painter.drawLine(QPointF(x, plot.top()),
		                 QPointF(x, chance_plot.bottom() +
		                                  m_metrics.ribbon_height + 2));
	}



	/* --- rain chance, under everything it might otherwise hide -------- */
	/*
	 * FIRST of the series, because it is an area and the others are
	 * lines (sec 3.19).
	 *
	 * It was drawn last while it had a panel to itself, where nothing
	 * could be hidden behind it. Full height in the shared plot it
	 * would have covered the temperature line on any hour the chance
	 * was high -- which is to say on exactly the hours somebody is
	 * looking at the chart to decide about.
	 *
	 * No background fill: the hour bands are drawn tall enough to cover
	 * the plot and filling over them would erase them, which is what a
	 * first attempt did.
	 */
	QColor chance_fill = m_palette.chance;
	/*
	 * Fainter than it was, twice. At 150 over 46 pixels it was a bar;
	 * over the whole height it is a wash, and a wash that dark competes
	 * with the lines drawn on top of it. 90 was the first answer and
	 * still read heavy once sec 3.20 took the hour bands away -- with a
	 * plain ground behind it, the wash became the only tint left and
	 * carried more weight than it had when it was one of three.
	 */
	chance_fill.setAlpha(60);
	painter.setPen(Qt::NoPen);
	painter.setBrush(chance_fill);

	/*
	 * ONE POLYGON PER RUN, NOT ONE RECTANGLE PER COLUMN (sec 16.89).
	 *
	 * This drew a rectangle a single pixel wide for every column across
	 * the plot. A one-pixel-wide translucent fill is the worst shape
	 * Qt's raster engine has: measured on this machine, narrow
	 * translucent strips cost 18 ns per pixel where the same area drawn
	 * as a wide band costs 1.3, because a short span amortises none of
	 * the blend's per-span setup.
	 *
	 * Adjacent covered columns are contiguous, so the wash is really a
	 * skyline -- one area with a stepped top edge. Drawn that way the
	 * spans the rasteriser produces are as wide as the run, which is
	 * the case that goes fast, and the picture is the same picture
	 * because the steps are the same steps.
	 *
	 * A polygon per RUN rather than one for the whole width, because a
	 * gap in coverage has to stay a gap: joining across it would wash
	 * hours that have no chance to report.
	 *
	 * AND NOT RECTANGLES, WHICH IS THE OBVIOUS NEXT IDEA AND IS TEN
	 * TIMES SLOWER (sec 16.93). Coalescing columns that share a top row
	 * and filling those looks like the same trick the ribbon uses, and
	 * it is not: the ribbon's band changes a handful of times across a
	 * view, while the chance varies continuously, so 792 columns give
	 * 563 runs -- about one and a half columns each. That is a field of
	 * narrow tall translucent strips, which is the worst shape this
	 * engine has and the thing sec 16.89 exists to avoid. Measured: 6.6
	 * ms became 61.7.
	 *
	 * The polygon is already doing the fast thing. Its rasteriser walks
	 * SCANLINES and emits one wide span per row, which is the axis a
	 * hand-written decomposition has to use as well -- and there is no
	 * reason to think a loop here would beat Qt's at it.
	 */
	QPolygonF wash;

	const auto flush_wash = [&]() {
		if (wash.size() >= 4) {
			wash << QPointF(wash.last().x(), chance_plot.bottom());
			painter.drawPolygon(wash);
		}
		wash.clear();
	};

	for (int x = 0; x < plot.width(); ++x) {
		const column &c = columns[x];
		if (!c.covered || !c.has_chance) {
			flush_wash();
			continue;
		}

		/*
		 * Fixed 0..100 rather than scaled to what is visible. A
		 * percentage means the same thing everywhere, and rescaling it
		 * would make a dry day's five percent look like a downpour.
		 */
		const double h = chance_plot.height() * (c.chance / 100.0);
		const double left = chance_plot.left() + x;
		const double top = chance_plot.bottom() - h;

		if (wash.isEmpty()) {
			wash << QPointF(left, chance_plot.bottom());
		}

		wash << QPointF(left, top) << QPointF(left + 1.0, top);
	}

	flush_wash();

	painter.setBrush(Qt::NoBrush);




	/* --- rain, drawn first so the temperature line sits over it -------- */
	/*
	 * A POLYGON PER RUN RATHER THAN ONE QPainterPath (sec 16.89).
	 *
	 * The shape is unchanged -- one point per column along the top of
	 * the trace, dropped to the baseline at each end of a run -- but it
	 * is built as a QPolygonF and handed to drawPolygon.
	 *
	 * QPainterPath is the expensive way to say this: it carries an
	 * element type per point and a painter path's worth of bookkeeping,
	 * and drawPath converts it to polygons before the rasteriser sees
	 * it. A QPolygonF is the same points and none of that.
	 *
	 * IT IS NOT WHY THIS SECTION LOOKED SLOW, and the wrong version of
	 * this comment said it was. The 4.4ms attributed to "rain" was the
	 * sample-dot loop below, which shared a timing mark with it; rain
	 * itself is 76us, because the fixture measured has no rain and the
	 * path enclosed a bounding box ZERO PIXELS TALL. A layer that draws
	 * nothing cannot cost a sixth of a paint, and reading that as a
	 * property of the path rather than of the mark is what sent the
	 * first attempt at this comment wrong.
	 *
	 * So this is a tidying rather than a fix, kept because the shape is
	 * simpler and matches the wash above.
	 */
	QColor rain_fill = m_palette.rain;
	rain_fill.setAlpha(120);
	painter.setPen(Qt::NoPen);
	painter.setBrush(rain_fill);

	QPolygonF rain_run;

	const auto flush_rain = [&](double edge_x) {
		if (rain_run.size() >= 2) {
			rain_run << QPointF(edge_x, plot.bottom());
			painter.drawPolygon(rain_run);
		}
		rain_run.clear();
	};

	for (int x = 0; x < plot.width(); ++x) {
		const column &c = columns[x];
		const double px = plot.left() + x;

		if (!c.covered || !c.has_rain) {
			flush_rain(px);
			continue;
		}

		if (rain_run.isEmpty()) {
			rain_run << QPointF(px, plot.bottom());
		}

		rain_run << QPointF(px, y_for_rain(c.rain));
	}

	flush_rain(plot.right());





	/*
	 * Wind: context for the grilling score rather than a headline, so
	 * it is thin, dotted, and drawn UNDER the lines (sec 3.19.1).
	 *
	 * It said that and was painted after the temperature, which put
	 * dotted holes in the red line wherever the two crossed -- the same
	 * defect as the rain chance directly above, and for the same
	 * reason: an order that was harmless while the series had somewhere
	 * else to be.
	 *
	 * Over the rain areas rather than beneath them, which is the one
	 * place this is not literally "under everything". A dotted line
	 * behind a filled area is most of the way to not being drawn, and
	 * wind is the variable that decides a grilling window as often as
	 * temperature does.
	 */
	if (m_show_wind) {
		QPolygonF wind_run;

		for (int x = 0; x < plot.width(); ++x) {
			const column &c = columns[x];
			if (!c.covered || !c.has_wind) {
				if (wind_run.size() > 1) {
					painter.setPen(QPen(m_palette.wind, 1.0, Qt::DotLine));
					painter.drawPolyline(wind_run);
				}
				wind_run.clear();
				continue;
			}

			wind_run.append(QPointF(plot.left() + x, y_for_wind(c.wind)));
		}

		if (wind_run.size() > 1) {
			painter.setPen(QPen(m_palette.wind, 1.0, Qt::DotLine));
			painter.setBrush(Qt::NoBrush);
			painter.drawPolyline(wind_run);
		}
	}

	/*
	 * Below one sample per pixel the marks are not drawn at all
	 * (sec 13.2).
	 *
	 * Sec 3.11.3 makes a dot mean "a real sample, here". At a zoom where
	 * twenty of them share a pixel they merge into a band of ink that
	 * claims a density of measurement nobody made -- the graph that is
	 * wrong while looking fine. Absent dots say "zoomed out"; smeared
	 * dots say something false.
	 */


	int knot_total = 0;
	for (const column &c : columns) {
		knot_total += c.knot_count;
	}

	const bool marks_would_crowd = knot_total > plot.width();

	if (m_show_samples && !marks_would_crowd) {
		/*
		 * RINGED IN THE GROUND, and drawn BEFORE the curve rather than
		 * after it (sec 16.32.1).
		 *
		 * A halo is destructive -- it lays ground over whatever is
		 * already there -- so a ring drawn after the line cut the line,
		 * and the curve rendered as a row of dots with black collars.
		 * A screenshot showed that; no contrast measurement would have.
		 *
		 * The ring was nearly dropped instead, on the arithmetic that a
		 * knot sits on the curve and the curve's own halo already
		 * reaches past it. That is true of the knots ON the curve and
		 * false of the ones that matter: the line is smoothed, so a
		 * knot can sit well away from it, and those are exactly the
		 * samples worth marking. Two of them landed straight on the
		 * rain wash with nothing under them.
		 *
		 * Moving the marks under the line costs nothing. Where a knot
		 * and the curve agree the line covers the dot, and that is the
		 * case where the dot was saying what the line already said.
		 */
		painter.setPen(Qt::NoPen);

		/*
		 * Rebuilt when the screen changes under the window as well as
		 * when the palette does: a stamp carries the ratio it was drawn
		 * at, and dragging a window between a HiDPI screen and an
		 * ordinary one changes that without touching either setter.
		 */
		/*
		 * THE RATIO COMES FROM THE DEVICE TRANSFORM (sec 16.99).
		 *
		 * Measured, after two wrong guesses. Painting this widget into
		 * an image with a device pixel ratio of 2:
		 *
		 *     painter.deviceTransform().m11()   2.00
		 *     painter.device()->devicePixelRatioF()   1.00
		 *     painter.transform().m11()         1.00
		 *     this->devicePixelRatioF()         1.00
		 *
		 * During QWidget::render the painter's DEVICE is the widget
		 * rather than the target, so it reports the screen's ratio and
		 * not the surface's, and the user transform is identity because
		 * nobody set one. The scaling is applied in the device
		 * transform, which is the only one of the four that knows how
		 * many real pixels a logical one covers -- which is exactly the
		 * question a stamp has to answer.
		 *
		 * It follows a user transform too, if a caller ever sets one.
		 * That is right rather than incidental: a stamp should be
		 * rendered for the resolution it will be drawn at.
		 */
		const double ratio =
		        std::max(0.25, painter.deviceTransform().m11());

		if (m_dot_stamps.empty() ||
		    !qFuzzyCompare(m_dot_stamp_ratio, ratio)) {
			build_dot_stamps(ratio);
		}

		/*
		 * Two fills rather than a stroke. A pen of width w centres its
		 * stroke ON the outline, so half of it falls INSIDE the dot --
		 * at the widths this needs that left a ring of ground with a
		 * pinprick of red in the middle, which is a different mark
		 * meaning nothing. Filling a larger disc first and the real one
		 * over it puts the whole ring outside, where it belongs.
		 */
		for (int x = 0; x < plot.width(); ++x) {
			const column &c = columns[x];
			if (!c.covered || !c.knot_has_temperature) {
				continue;
			}

			/*
			 * THE MARK NEED NOT LAND ON THE CURVE, and when it does
			 * not that is the feature rather than a misplacement
			 * (sec 16.110). This is the column's own reading; the
			 * line below it is drawn from the smoothed and rounded
			 * series, so the two differ by however much the smoothing
			 * moved the line -- which is precisely what sec 3.11.3
			 * put the marks here to show.
			 *
			 * Measured once, in case the scatter looks like an offset
			 * again: over the 70 columns of one frame where a mark
			 * separates from the curve, 34 sat above it and 36 below.
			 * A placement error is a displacement and cannot scatter.
			 */
			const double px = plot.left() + x;
			const double py = y_for_temperature(c.knot_temperature);

			/*
			 * STAMPED FROM A CACHE, NOT DRAWN (sec 16.92).
			 *
			 * drawEllipse is not a circle primitive: Qt builds a path
			 * of four beziers, flattens it and fills it through the
			 * antialiasing rasteriser, every time. Measured at a
			 * sixteen-day view, where 369 of these are drawn per
			 * frame, that was 5885us -- about 16us for a dot four
			 * pixels across, and a fifth of the whole paint.
			 *
			 * Drawn once into a pixmap and blitted it is 655us, nine
			 * times faster, because a blit of a premultiplied stamp is
			 * a memory operation.
			 *
			 * THE SUBPIXEL OFFSETS ARE WHAT MAKE IT HONEST. A naive
			 * blit rounds its destination to whole pixels, so a dot
			 * would snap up to half a pixel from where the reading
			 * actually is -- and snap BACK as the view scrolled, which
			 * is a shimmer along the curve during exactly the drag
			 * this work exists to smooth. So the stamp is rendered at
			 * eight vertical offsets and the nearest is chosen, which
			 * puts the error under a sixteenth of a pixel.
			 *
			 * Vertical only, because px is an integer here -- the
			 * columns are whole pixels by construction -- so there is
			 * nothing for a horizontal offset to fix.
			 */
			const int stamp_at =
			        int(std::floor((py - std::floor(py)) *
			                       double(dot_stamp_offsets))) %
			        dot_stamp_offsets;
			const QPixmap &stamp = m_dot_stamps[size_t(stamp_at)];

			painter.drawPixmap(
			        QPoint(int(px) - stamp.width() / 2,
			               int(std::floor(py)) - stamp.height() / 2),
			        stamp);
		}

		painter.setBrush(Qt::NoBrush);
	}



	/* --- temperature, broken wherever no band covers a column --------- */
	painter.setBrush(Qt::NoBrush);

	/*
	 * HALOED (sec 16.32). The curve crosses grounds this drawing makes
	 * for itself -- the rain area, the chance wash, a grilling window,
	 * and any two of them at once -- and stacked they take Weather
	 * Underground's red to 1.35:1 on the dark scheme. No alpha fixes
	 * that, because the washes compose; a ground of the line's own does,
	 * and it leaves the measured red exactly where it is.
	 *
	 * The same answer this project reached for the tray icon and the
	 * widget's number, and for the same reason: the ground is not ours
	 * to know, so the ink brings one.
	 */
	const QPen curve_ink(m_palette.temperature, m_metrics.line_width);
	const QPen curve_halo =
	        halo_pen(ground_behind(), m_metrics.line_width);

	/*
	 * SIMPLIFIED BEFORE STROKING (sec 16.91).
	 *
	 * The curve carries one point per column and is stroked twice --
	 * halo then ink, the halo with round joins that cost an arc at every
	 * vertex. On a one-day view of real weather that pair was 70% of the
	 * paint, which makes it the single most expensive thing this widget
	 * does.
	 *
	 * A hundredth of a pixel is the tolerance rather than a tenth. Both
	 * are invisible in principle; measured against the unsimplified
	 * render, a tenth moves an antialiased edge pixel by up to 26 of 255
	 * and a fiftieth by 6, and 6 is under what a screen will show. It
	 * still drops five vertices in six on a smooth trace -- 429 points
	 * to 69 -- because what is being removed is not detail but the same
	 * straight line said hundreds of times.
	 *
	 * Noisier weather keeps more points and saves less. That is the
	 * right way round: the tolerance bounds the ERROR, and the speed is
	 * whatever the data allows.
	 */
	const double curve_tolerance = 0.02;

	const auto stroke_curve = [&](const QPolygonF &full) {
		const QPolygonF line = bbq_simplify_polyline(full, curve_tolerance);
		painter.setPen(curve_halo);
		painter.drawPolyline(line);
		painter.setPen(curve_ink);
		painter.drawPolyline(line);
	};

	QPolygonF run;
	for (int x = 0; x < plot.width(); ++x) {
		const column &c = columns[x];

		if (!c.covered || !c.has_temperature) {
			/*
			 * The break, and it is the point. Joining across an
			 * uncovered column would draw a line through a period
			 * nothing has reported on (sec 3.6).
			 */
			if (run.size() > 1) {
				stroke_curve(run);
			}
			run.clear();
			continue;
		}

		run.append(QPointF(plot.left() + x, y_for_temperature(c.temperature)));
	}

	if (run.size() > 1) {
		stroke_curve(run);
	}

	/*
	 * The bias-corrected overlay (sec 12.5).
	 *
	 * Dashed, in a colour that is not one of Weather Underground's
	 * measured set, and never marked with sample dots -- because it has
	 * no samples. Every point on it is arithmetic, and the whole reason
	 * it is drawn beside the forecast instead of replacing it is so the
	 * difference between what was said and what this program thinks is
	 * visible rather than asserted.
	 */
	for (int pass = 0; pass < 2 && !m_corrected.is_empty(); ++pass) {
		/*
		 * Temperature and rain are corrected independently (sec 12.5): a
		 * provider can be reliably warm and perfectly good about rain,
		 * so one of these may be drawn without the other.
		 */
		const bool doing_rain = pass == 1;
		std::vector<bbq_knot> knots;

		for (const bbq_sample &sample : m_corrected.samples()) {
			if (sample.start_utc < from || sample.start_utc >= to) {
				continue;
			}

			const std::optional<double> &value =
			        doing_rain ? sample.precip_rate : sample.temperature;

			if (!value.has_value()) {
				continue;
			}

			bbq_knot knot;
			knot.x = (sample.start_utc - from) / seconds_per_pixel;
			knot.y = *value;
			knots.push_back(knot);
		}

		/*
		 * A corrected rain line with no rain in it is not drawn.
		 *
		 * On a dry forecast the raw rate is zero, a positive bias
		 * corrects it below zero, and the clamp puts it back at zero --
		 * so the honest result is a dashed line lying flat along the
		 * baseline for the width of the graph. It is not wrong; it just
		 * says nothing, and a line that says nothing on a weather graph
		 * still has to be read before it can be dismissed.
		 */
		if (doing_rain) {
			double most = 0.0;
			for (const bbq_knot &knot : knots) {
				most = std::max(most, knot.y);
			}

			if (most < 0.05) {
				continue;
			}

			/*
			 * NOR ONE THAT SAYS WHAT THE FORECAST ALREADY SAID
			 * (sec 16.97.5).
			 *
			 * The rain bias is floored at zero (sec 16.97), so where a
			 * band has been UNDER-forecasting the correction declines
			 * to act and the corrected rate is the forecast's own. A
			 * view lying entirely inside such a lead -- anything past a
			 * week, where the bucket is `beyond` -- then draws a dashed
			 * line exactly along the forecast it is drawn against.
			 *
			 * That is worse than the empty line above rather than
			 * merely as bad. A flat line at the baseline plainly says
			 * nothing; a line lying on the forecast says the forecast
			 * was checked and found right, when what happened is that
			 * it was checked, found too dry, and left alone.
			 *
			 * Compared against the composite at each sample's own
			 * instant, which is what the correction was computed FROM
			 * -- not against the drawn columns, which carry smoothing
			 * and rounding this line is given separately below, so two
			 * identical inputs could differ there and hide the case.
			 */
			bool says_something = false;
			for (const bbq_sample &sample : m_corrected.samples()) {
				if (sample.start_utc < from || sample.start_utc >= to ||
				    !sample.precip_rate.has_value()) {
					continue;
				}

				const bbq_reading raw = m_composite.at(sample.start_utc);
				if (!raw.is_valid() || !raw.sample->precip_rate.has_value()) {
					says_something = true;
					break;
				}

				if (qAbs(*sample.precip_rate - *raw.sample->precip_rate) >=
				    0.001) {
					says_something = true;
					break;
				}
			}

			if (!says_something) {
				continue;
			}
		}

		/*
		 * The SAME rounding as the curve it is drawn against.
		 *
		 * Without this the forecast was smoothed and the correction was
		 * not, so the gap between the two lines was part bias and part
		 * smoothing -- and the whole purpose of drawing them together is
		 * that the gap is the correction. Any treatment applied to one
		 * has to be applied to the other or the comparison lies.
		 */
		bbq_smooth(knots, spec.smooth_columns);

		if (knots.size() >= 2) {
			bbq_curve curve;
			const double first = knots.front().x;
			const double last = knots.back().x;
			curve.set(std::move(knots), m_interpolation);

			QPolygonF corrected_run;
			for (int x = 0; x < plot.width(); ++x) {
				const double at = static_cast<double>(x);
				if (at < first || at > last) {
					continue;
				}

				const double value = curve.at(at);
				const double y = doing_rain ? y_for_rain(std::max(0.0, value))
				                            : y_for_temperature(value);

				corrected_run.append(QPointF(plot.left() + x, y));
			}

			if (corrected_run.size() > 1) {
				/*
				 * The rain outline is drawn in the rain's own colour,
				 * darkened, rather than in the temperature correction's.
				 * Two dashed lines of one colour against two different
				 * scales would say they were the same quantity.
				 */
				QColor ink = m_palette.corrected;
				if (doing_rain) {
					ink = m_palette.rain.darker(150);
				}

				QPen corrected_pen(ink, m_metrics.line_width);
				corrected_pen.setStyle(Qt::DashLine);

				/*
				 * NOT HALOED, and that is a decision rather than an
				 * omission (sec 16.32.1).
				 *
				 * A halo is destructive: it lays ground over whatever
				 * is already there. This line is drawn after the
				 * forecast curve and runs along it, so its halo erased
				 * the red under every dash -- and the curve then read
				 * as DASHED, which in this chart's own language means
				 * "arithmetic rather than measured". A legibility fix
				 * that makes a measured line claim to be derived is a
				 * worse bug than the one it fixes.
				 *
				 * What would work is drawing every halo before every
				 * ink, so no halo can reach an ink already down. That
				 * is a restructure of this function and is recorded
				 * rather than done; meanwhile the overlay's contrast
				 * over the washes is held by the palette gate's
				 * tripwires.
				 */
				painter.setBrush(Qt::NoBrush);
				painter.setPen(corrected_pen);
				painter.drawPolyline(corrected_run);

				/*
				 * Labelled where it starts. An unexplained second line
				 * on a weather graph is worse than no second line. The
				 * rain one is left unlabelled: it sits on the rain area
				 * in the rain's colour, which says what it is, and a
				 * second caption in the same corner would collide with
				 * the first.
				 */
				if (!doing_rain) {
					const QPointF head = corrected_run.first();

					/*
					 * ON THE FAR SIDE FROM THE LINE IT IS ABOUT
					 * (sec 3.19.2).
					 *
					 * It was always 16 pixels above the head, which put
					 * it straight through the temperature trace: a
					 * positive bias draws the corrected line BELOW the
					 * forecast, so above the head is exactly where the
					 * red line is. Which side is clear depends on the
					 * sign of the correction, so it is chosen from the
					 * temperature's own position rather than assumed.
					 */
					const int at = qBound(0, int(head.x() - plot.left()),
					                      int(columns.size()) - 1);
					const column &under = columns[at];

					double offset = -16.0;
					if (under.covered && under.has_temperature &&
					    head.y() > y_for_temperature(under.temperature)) {
						offset = 4.0;
					}

					/*
					 * AND CLEAR OF THE LINE IT LABELS (sec 16.107).
					 *
					 * The rule above picks the side away from the
					 * TEMPERATURE trace, which is the line the label
					 * used to be drawn through. It says nothing about
					 * the corrected line itself, and that one starts at
					 * the head and runs to the right underneath the
					 * caption: where the correction climbs, it climbs
					 * through its own label.
					 *
					 * Seen on the mobile layout, whose plot is taller
					 * and narrower, so the same rise covers more
					 * vertical distance inside the label's 120 pixels.
					 * The desktop shot of the same minute has the
					 * caption clear.
					 *
					 * So the run is asked where it goes across the
					 * caption's own span, and the side that collides
					 * gives way. If both collide the first rule stands
					 * -- something has to be chosen, and the trace it
					 * was written for is the more important of the two
					 * to stay off.
					 */
					/*
					 * The temperature trace across the caption's span,
					 * as a polyline, so one helper answers for both
					 * lines. Moving off one and onto the other is the
					 * collision sec 3.19.2 fixed, reintroduced by the
					 * fix for its neighbour.
					 */
					QPolygonF trace;
					for (double x = head.x(); x <= head.x() + 128.0;
					     x += 1.0) {
						const int column_at = int(x) - plot.left();
						if (column_at < 0 ||
						    column_at >= int(columns.size())) {
							continue;
						}

						const column &c = columns[size_t(column_at)];
						if (c.covered && c.has_temperature) {
							trace << QPointF(
							        x, y_for_temperature(c.temperature));
						}
					}

					const auto collides = [&](double at_offset) {
						const QRectF box(head.x() + 4, head.y() + at_offset,
						                 120, 14);

						return bbq_box_meets_polyline(box, corrected_run) ||
						       bbq_box_meets_polyline(box, trace);
					};

					if (collides(offset) && !collides(-offset - 12.0)) {
						offset = -offset - 12.0;
					}

					const QRectF where(head.x() + 4, head.y() + offset, 120, 14);
					const QString caption = tr("bias-corrected");

					/*
					 * Haloed for the same reason the tray number is
					 * (sec 4.3): it is drawn over a rain wash whose
					 * darkness is the weather's to decide.
					 */
					painter.setPen(ground_behind());
					for (int dx = -1; dx <= 1; ++dx) {
						for (int dy = -1; dy <= 1; ++dy) {
							if (dx == 0 && dy == 0) {
								continue;
							}

							painter.drawText(where.translated(dx, dy),
							                 Qt::AlignLeft | Qt::AlignVCenter,
							                 caption);
						}
					}

					painter.setPen(ink);
					painter.drawText(where, Qt::AlignLeft | Qt::AlignVCenter,
					                 caption);
				}
			}
		}
	}



	/* --- the hour marks, over the series so rain cannot bury them ----- */
	/*
	 * EVERY hour, not only the labelled ones (sec 3.20).
	 *
	 * The first version marked the labelled times alone, which at a
	 * day's zoom is one mark every six hours -- a scale, not the hour
	 * breaks that were asked for. The labelled ones stay longer, so the
	 * two read as major and minor rather than as a row of identical
	 * marks.
	 *
	 * Skipped entirely when the hours would be closer together than a
	 * few pixels. At a fortnight's zoom an hourly comb is 336 marks and
	 * says nothing except that the edge is busy.
	 */
	const double hour_px = 3600.0 / seconds_per_pixel;

	if (hour_px >= 7.0) {
		/*
		 * FAINTER than the labelled marks, not just shorter. Length
		 * alone did not separate them enough at real size on a phone:
		 * a row of full-strength ticks reads as its own rule along the
		 * edge, which is close to the thing the banding was removed
		 * for. Half the weight puts them behind the hours that carry a
		 * name.
		 */
		QColor minor = m_palette.grid;
		/*
		 * 175, up from 110. Fading them was right in direction and too
		 * far in degree -- they have to sit BEHIND the labelled hours,
		 * not disappear.
		 */
		minor.setAlpha(175);
		painter.setPen(QPen(minor, 1.0));

		const qint64 first_hour = ((from / 3600) + 1) * 3600;
		for (qint64 t = first_hour; t < to; t += 3600) {
			const double x = plot.left() + (t - from) / seconds_per_pixel;
			if (x < plot.left() || x > plot.right()) {
				continue;
			}

			painter.drawLine(QPointF(x, plot.top()),
			                 QPointF(x, plot.top() + edge_tick_px * 0.66));
			painter.drawLine(QPointF(x, chance_plot.bottom()),
			                 QPointF(x, chance_plot.bottom() - edge_tick_px * 0.66));
		}
	}

	painter.setPen(QPen(m_palette.grid, 1.0));
	for (double x : hour_marks) {
		painter.drawLine(QPointF(x, plot.top()),
		                 QPointF(x, plot.top() + edge_tick_px));
		painter.drawLine(QPointF(x, chance_plot.bottom()),
		                 QPointF(x, chance_plot.bottom() - edge_tick_px));
	}

	/*
	 * The marks (sec 3.11.3). Only real samples get one, which is what
	 * lets a smoothed curve be read honestly: the dots are the data and
	 * the line between them is drawn.
	 */




	/* --- the provenance ribbon (sec 3.4) ------------------------------ */
	for (int x = 0; x < plot.width(); ++x) {
		if (!columns[x].covered) {
			continue;
		}

		/*
		 * FILLED PER COLUMN, and the run of equal bands coalesced
		 * (sec 16.89).
		 *
		 * This set a pen and stroked a one-pixel line for every column
		 * across the plot -- nine hundred pen changes and nine hundred
		 * strokes per paint, for a strip six pixels tall. Setting a pen
		 * is not free: it discards the stroker's state, so the loop
		 * paid for that nine hundred times over as well.
		 *
		 * The ribbon is a run-length encoding of the band by
		 * construction -- provenance changes a handful of times across
		 * a view, not once per pixel -- so it is drawn as the few
		 * rectangles it really is.
		 */
		const QColor ink = band_colour(m_palette, columns[x].band);
		int end = x + 1;
		while (end < plot.width() && columns[end].covered &&
		       band_colour(m_palette, columns[end].band) == ink) {
			++end;
		}

		painter.fillRect(QRectF(plot.left() + x, chance_plot.bottom() + 2,
		                        end - x, m_metrics.ribbon_height),
		                 ink);
		x = end - 1;
	}

	/*
	 * The day each divider opens, named where the day begins.
	 *
	 * Drawn after the data so it is never lost under a curve, and on the
	 * same translucent plate the edge labels use. A divider without a
	 * name only says "something changed here".
	 */
	if (!midnights.empty() && !days_would_crowd) {
		painter.setFont(label_font);

		for (qint64 midnight : midnights) {
			const double x = plot.left() + (midnight - from) / seconds_per_pixel;
			const QString name =
			        local_time(midnight, zone).toString(QStringLiteral("ddd d"));

			/* Skip one that would be drawn off the right-hand edge. */
			if (x + 52 > plot.right()) {
				continue;
			}

			/*
			 * Bold, because it names the thing the divider marks and
			 * has to be findable at a glance among the hour labels
			 * along the bottom, which are deliberately quiet.
			 */
			painter.setFont(day_font);

			/*
			 * NAMED IN THE LABEL COLOUR, SAID OUT LOUD (sec 16.90).
			 *
			 * edge_label draws with whatever pen the caller left --
			 * deliberately, because one of its callers wants the wind
			 * colour -- and this call site never set one. So the day
			 * names took whichever pen the previous block happened to
			 * finish with, which was the ribbon's, which is the LAST
			 * PROVENANCE BAND IN VIEW: the colour of "Mon 14" was a
			 * property of the weather data.
			 *
			 * It went unseen because the accident was plausible. The
			 * band colours are greys of about the right weight, so the
			 * names looked like labels rather than like a fault, and
			 * nothing in the picture said the colour had come from
			 * anywhere.
			 *
			 * Found only because the ribbon stopped setting a pen when
			 * it stopped stroking one line per column for speed, at
			 * which point the names inherited the GRID colour instead
			 * and went visibly dim. The overlay path -- the phone and
			 * the home-screen picture -- sets axis_text inside
			 * edge_label and was right all along, so this makes the
			 * desktop agree with the phone rather than inventing an
			 * answer.
			 */
			painter.setPen(m_palette.axis_text);

			edge_label(x + 4, plot.top() + 2, 56, Qt::AlignLeft, name);

			painter.setFont(label_font);
		}
	}



	/* --- now ---------------------------------------------------------- */
	const double now_x = plot.left() + (now - from) / seconds_per_pixel;
	painter.setPen(QPen(m_palette.now_marker, 1.5));
	painter.drawLine(QPointF(now_x, plot.top()),
	                 QPointF(now_x, chance_plot.bottom()));


	/* --- the readout at the cursor ------------------------------------ */
	/*
	 * Snapped to the nearest real sample, never to the cursor.
	 *
	 * This is the whole design of the thing. The curve between samples
	 * is drawn rather than measured (sec 3.11), so reporting the value
	 * under the pointer would put an interpolated number in a box that
	 * looks like a reading -- a graph that is wrong while looking fine,
	 * which is the failure this project keeps naming. Snapping means
	 * every number in the box is one a provider actually reported.
	 *
	 * It also says WHICH band and provider produced it, which is what
	 * sec 3.4 kept the series separate for.
	 */
	if (m_cursor_column >= 0 && m_cursor_column < plot.width()) {
		int found = -1;
		for (int step = 0; step < plot.width(); ++step) {
			const int left = m_cursor_column - step;
			const int right = m_cursor_column + step;

			if (left >= 0 && columns[left].has_knot) {
				found = left;
				break;
			}
			if (right < plot.width() && columns[right].has_knot) {
				found = right;
				break;
			}
		}

		if (found >= 0) {
			const column &c = columns[found];
			const double px = plot.left() + found;

			painter.setPen(QPen(m_palette.readout_edge, 1, Qt::DashLine));
			painter.drawLine(QPointF(px, plot.top()),
			                 QPointF(px, chance_plot.bottom()));

			if (c.knot_has_temperature) {
				painter.setPen(Qt::NoPen);
				painter.setBrush(m_palette.temperature);
				const double py = y_for_temperature(c.knot_temperature);
				painter.drawEllipse(QPointF(px, py), 4.0, 4.0);
				painter.setBrush(Qt::NoBrush);
			}

			/*
			 * ONE ROW along the top, not a stack beside the cursor
			 * (sec 3.17).
			 *
			 * It was six lines in an opaque box pinned inside the
			 * plot, and it blocked the graph it was describing --
			 * worst on a phone, where the box is a large share of a
			 * small picture and the reader has no second window to
			 * put it in. Height was the problem rather than width, so
			 * the fields join into a single row.
			 *
			 * Ordered by what somebody reading a weather graph
			 * actually wants, because that is also the order they get
			 * dropped in when the row will not fit.
			 */

			/*
			 * Two spellings, and the narrow one is not a fallback for
			 * an unusual case -- a phone is the ordinary case here.
			 * The Fold's cover screen is 320 logical pixels wide and
			 * the roomy row does not fit it at all, so without this
			 * the loop below would strip the row back to the time and
			 * the temperature on the device people actually carry.
			 *
			 * The weekday is what goes first, because the graph now
			 * names every day across the top (sec 3.15). A readout
			 * repeating what the divider beside it already says is
			 * the cheapest thing in the row to lose.
			 */
			const bool wide_enough = plot.width() >= 420;

			QStringList parts;
			parts.append(bbq_readout_time_label(c.knot_utc,
			                                    c.knot_last_utc,
			                                    c.knot_count, wide_enough,
			                                    zone));

			if (c.knot_has_temperature) {
				parts.append(QString::number(c.knot_temperature, 'f', 1) +
				             tr(" C"));
			}
			if (c.knot_has_rain) {
				/*
				 * One decimal rather than two. The second never
				 * decided anything and it cost a character in the
				 * row that has to fit.
				 */
				parts.append(QString::number(c.knot_rain, 'f', 1) +
				             tr(" mm/h"));
			}
			if (c.knot_has_chance) {
				parts.append(QString::number(c.knot_chance, 'f', 0) +
				             QStringLiteral("%"));
			}
			if (c.knot_has_wind) {
				parts.append(QString::number(c.knot_wind, 'f', 0) +
				             tr(" km/h"));
			}

			/*
			 * The band, without the provider. "nowcast / wunderground"
			 * was the longest line in the old box and therefore set
			 * its width, to say something the ribbon underneath
			 * already says in colour.
			 */
			parts.append(QString::fromLatin1(bbq_band_name(c.band)));

			/*
			 * MEASURED, and shortened by dropping fields rather than
			 * by clipping. A row wider than the plot would be cut
			 * mid-character at the edge, which is the fault sec
			 * 3.12.1.1 exists about; dropping the wind or the band
			 * loses a field the reader can see elsewhere, and the
			 * time and temperature at the front survive to the last.
			 */
			const QFontMetrics metrics(label_font);
			const int pad = 6;

			/*
			 * The gap between fields is the other thing that scales.
			 * Five separators at three spaces is a dozen characters
			 * of nothing, which on a narrow screen is a field.
			 */
			const QString gap = wide_enough ? QStringLiteral("   ")
			                                : QStringLiteral(" ");

			QString text = parts.join(gap);
			while (parts.size() > 2 &&
			       metrics.horizontalAdvance(text) + pad * 2 > plot.width() - 8) {
				parts.removeLast();
				text = parts.join(gap);
			}

			const int box_w = metrics.horizontalAdvance(text) + pad * 2;
			const int box_h = metrics.height() + pad * 2;

			/*
			 * Centred on the cursor where there is room, and pushed
			 * inside the plot where there is not, so it stays near
			 * the finger without ever leaving the widget.
			 */
			const double box_x = bbq_readout_box_x(
			        px, box_w, plot.left(), plot.right());

			const QRectF box(box_x, plot.top() + 4, box_w, box_h);
			painter.setPen(m_palette.readout_edge);
			painter.setBrush(m_palette.readout_back);
			painter.drawRoundedRect(box, 3, 3);

			painter.setPen(m_palette.readout_text);
			painter.drawText(box, Qt::AlignCenter, text);

			painter.setBrush(Qt::NoBrush);
		}
	}



	/* --- axis labels --------------------------------------------------- */
	painter.setPen(m_palette.axis_text);

	const QString high_text = QString::number(temperature_high, 'f', 0) + tr(" C");
	const QString low_text = QString::number(temperature_low, 'f', 0) + tr(" C");

	edge_label(left_label_x, plot.top() - 2, left_label_wide, left_align,
	           high_text);
	edge_label(left_label_x, plot.bottom() - 12, left_label_wide, left_align,
	           low_text);

	edge_label(left_label_x, chance_plot.bottom() + m_metrics.ribbon_height + 3,
	           left_label_wide, left_align, clock);

	/*
	 * The top of the rain scale, which is now a constant and so says
	 * something about the world rather than about this screenful. The
	 * "+" appears only when the trace has actually been clamped, so it
	 * is a statement about the data in view rather than decoration.
	 */
	const bool rain_clipped = rain_peak > rain_full_scale_mm_h;
	const QString rain_text =
	        QString::number(rain_full_scale_mm_h, 'f', 0) +
	        (rain_clipped ? QStringLiteral("+") : QString()) + tr(" mm/h");

	/*
	 * At the top, because that is where full scale now is. It sat at
	 * 45% of the height while the rain trace did.
	 */
	edge_label(right_label_x, plot.top() - 2, right_label_wide, right_align,
	           rain_text);

	/*
	 * And the chance's ceiling directly under it: both are full-height
	 * scales now, so both are labelled where they top out.
	 */
	edge_label(right_label_x, plot.top() - 2 + gutter.height(),
	           right_label_wide, right_align, tr("100% rain"));

	if (m_show_wind) {
		const QString wind_text = QString::number(wind_high, 'f', 0) + tr(" km/h");

		painter.setPen(m_palette.wind);
		edge_label(right_label_x, plot.top() - 2, right_label_wide, right_align,
		           wind_text);
		painter.setPen(m_palette.axis_text);
	}
}
