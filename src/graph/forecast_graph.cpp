#include "graph/forecast_graph.h"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QRect>
#include <QTimeZone>

#include <algorithm>
#include <vector>

namespace {

const int margin_left = 46;
const int margin_right = 62;
const int margin_top = 10;
const int margin_bottom = 34;

/*
 * The rain-chance panel, below the main plot and sharing its x axis --
 * the stacked-panel shape WU's own dashboard uses.
 *
 * Its own panel rather than a third line in the main one: a percentage
 * has nothing to do with either axis up there, and hanging it off one
 * of them would make a scale mean two things.
 */
const int chance_height = 46;
const int chance_gap = 6;
const int ribbon_height = 5;

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
	bbq_band band = bbq_band::hourly;
};

QColor band_colour(const bbq_graph_palette &palette, bbq_band band) {
	switch (band) {
	case bbq_band::observed:
		return palette.band_observed;
	case bbq_band::current:
		return palette.band_current;
	case bbq_band::nowcast:
		return palette.band_nowcast;
	case bbq_band::hourly:
		return palette.band_hourly;
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

	const bbq_reading reading = composite.at((from + to) / 2);
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

		if (starts_here && samples[i].temperature.has_value()) {
			temperature_total += *samples[i].temperature;
			++temperature_count;
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

	if (temperature_count > 0) {
		result.temperature = temperature_total / temperature_count;
		result.has_temperature = true;
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

QString hour_label(qint64 when_utc) {
	const QDateTime when = QDateTime::fromSecsSinceEpoch(when_utc);
	return when.toString(QStringLiteral("HH:mm"));
}

} // namespace

bbq_forecast_graph::bbq_forecast_graph(QWidget *parent) : QWidget(parent) {
	setMinimumSize(360, 180);

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
	m_palette.background = QColor(0xff, 0xff, 0xff);
	m_palette.band_shade = QColor(0xf1, 0xf7, 0xfb);
	m_palette.grid = QColor(0xe7, 0xe7, 0xe7);
	m_palette.axis_text = QColor(0x4a, 0x4a, 0x4a);
	m_palette.temperature = QColor(0xd5, 0x20, 0x2a);
	m_palette.rain = QColor(0x87, 0xc4, 0x03);

	/*
	 * WU's own rain-family cyan, taken from the accumulation series on
	 * their dashboard. Their dashboard plots observations and so has no
	 * precipitation-chance panel to measure, which is said out loud
	 * rather than left as an implied measurement: this is a WU colour
	 * used for a WU-adjacent purpose, not one sampled from the thing it
	 * is drawing.
	 */
	m_palette.chance = QColor(0x17, 0xaa, 0xdb);
	m_palette.now_marker = QColor(0x00, 0x53, 0xae);
	m_palette.stale_warning = QColor(0xd5, 0x20, 0x2a);
	m_palette.band_observed = QColor(0x5b, 0x9f, 0x49);
	m_palette.band_current = QColor(0x87, 0xc4, 0x03);
	m_palette.band_nowcast = QColor(0x17, 0xaa, 0xdb);
	m_palette.band_hourly = QColor(0x9a, 0x9a, 0x9a);
}

QSize bbq_forecast_graph::sizeHint() const {
	return QSize(760, 280);
}

void bbq_forecast_graph::set_composite(bbq_composite composite) {
	m_composite = std::move(composite);
	update();
}

void bbq_forecast_graph::set_window(qint64 before_s, qint64 after_s) {
	m_before_s = before_s;
	m_after_s = after_s;
	update();
}

void bbq_forecast_graph::paintEvent(QPaintEvent *event) {
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.fillRect(event->rect(), m_palette.background);

	const int stack = chance_height + chance_gap;
	const QRect plot(margin_left, margin_top,
	                 width() - margin_left - margin_right,
	                 height() - margin_top - margin_bottom - stack);
	const QRect chance_plot(plot.left(), plot.bottom() + chance_gap,
	                        plot.width(), chance_height);

	if (plot.width() < 20 || plot.height() < 20) {
		return;
	}

	const qint64 now = QDateTime::currentSecsSinceEpoch();
	const qint64 from = now - m_before_s;
	const qint64 to = now + m_after_s;

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

	/* Scales, from what is actually visible rather than from the whole set. */
	double temperature_low = 0.0;
	double temperature_high = 0.0;
	bool have_temperature = false;
	double rain_high = 1.0;

	for (const column &c : columns) {
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
			rain_high = std::max(rain_high, c.rain);
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

	const double temperature_pad = (temperature_high - temperature_low) * 0.12;
	temperature_low -= temperature_pad;
	temperature_high += temperature_pad;

	const auto y_for_temperature = [&](double value) {
		const double span = temperature_high - temperature_low;
		const double t = (value - temperature_low) / span;
		return plot.bottom() - t * plot.height();
	};

	const auto y_for_rain = [&](double value) {
		const double t = value / rain_high;
		return plot.bottom() - t * (plot.height() * 0.45);
	};

	/*
	 * Alternating three-hour bands, which is the most recognisable
	 * thing about the WU chart and the cheapest density cue there is:
	 * it gives the eye a ruler without adding a single line.
	 */
	const qint64 band_step = 3 * 3600;
	const qint64 first_band = (from / band_step) * band_step;

	for (qint64 t = first_band; t < to; t += band_step) {
		if ((t / band_step) % 2 != 0) {
			continue;
		}

		const double x0 = plot.left() + (t - from) / seconds_per_pixel;
		const double x1 = plot.left() + (t + band_step - from) / seconds_per_pixel;
		const double left = std::max(x0, static_cast<double>(plot.left()));
		const double right = std::min(x1, static_cast<double>(plot.right()));

		if (right > left) {
			const double tall = chance_plot.bottom() - plot.top();
			painter.fillRect(QRectF(left, plot.top(), right - left, tall),
			                 m_palette.band_shade);
		}
	}

	/* --- grid and time axis ------------------------------------------- */
	painter.setPen(QPen(m_palette.grid, 1, Qt::DotLine));

	QFont label_font = font();
	label_font.setPointSizeF(label_font.pointSizeF() * 0.85);
	painter.setFont(label_font);

	const qint64 tick_step = 3 * 3600;
	const qint64 first_tick = ((from / tick_step) + 1) * tick_step;

	for (qint64 t = first_tick; t < to; t += tick_step) {
		const double x = plot.left() + (t - from) / seconds_per_pixel;
		painter.setPen(QPen(m_palette.grid, 1, Qt::DotLine));
		painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));

		painter.setPen(m_palette.axis_text);
		const QRectF label(x - 24, chance_plot.bottom() + ribbon_height + 3,
		                   48, 14);
		painter.drawText(label, Qt::AlignCenter, hour_label(t));
	}

	/* --- rain, drawn first so the temperature line sits over it -------- */
	QPainterPath rain_path;
	bool rain_open = false;

	for (int x = 0; x < plot.width(); ++x) {
		const column &c = columns[x];
		const double px = plot.left() + x;

		if (!c.covered || !c.has_rain) {
			if (rain_open) {
				rain_path.lineTo(px, plot.bottom());
				rain_open = false;
			}
			continue;
		}

		if (!rain_open) {
			rain_path.moveTo(px, plot.bottom());
			rain_open = true;
		}

		rain_path.lineTo(px, y_for_rain(c.rain));
	}

	if (rain_open) {
		rain_path.lineTo(plot.right(), plot.bottom());
	}

	QColor rain_fill = m_palette.rain;
	rain_fill.setAlpha(120);
	painter.setPen(Qt::NoPen);
	painter.setBrush(rain_fill);
	painter.drawPath(rain_path);

	/* --- temperature, broken wherever no band covers a column --------- */
	painter.setBrush(Qt::NoBrush);
	painter.setPen(QPen(m_palette.temperature, 2.0));

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
				painter.drawPolyline(run);
			}
			run.clear();
			continue;
		}

		run.append(QPointF(plot.left() + x, y_for_temperature(c.temperature)));
	}

	if (run.size() > 1) {
		painter.drawPolyline(run);
	}

	/* --- rain chance, its own panel on a fixed 0..100 scale ----------- */
	/*
	 * No background fill here: the hour bands are drawn tall enough to
	 * cover both panels and filling this one would erase them, which is
	 * what a first attempt did. Sharing the banding is what makes two
	 * panels read as one chart rather than two.
	 */
	QColor chance_fill = m_palette.chance;
	chance_fill.setAlpha(150);
	painter.setPen(Qt::NoPen);
	painter.setBrush(chance_fill);

	for (int x = 0; x < plot.width(); ++x) {
		const column &c = columns[x];
		if (!c.covered || !c.has_chance) {
			continue;
		}

		/*
		 * Fixed 0..100 rather than scaled to what is visible. A
		 * percentage means the same thing everywhere, and rescaling it
		 * would make a dry day's five percent look like a downpour.
		 */
		const double h = chance_plot.height() * (c.chance / 100.0);
		painter.drawRect(QRectF(chance_plot.left() + x,
		                        chance_plot.bottom() - h, 1.0, h));
	}

	painter.setBrush(Qt::NoBrush);
	painter.setPen(m_palette.grid);
	painter.drawLine(chance_plot.bottomLeft(), chance_plot.bottomRight());

	painter.setPen(m_palette.axis_text);
	painter.drawText(QRectF(2, chance_plot.top() - 2, margin_left - 6, 14),
	                 Qt::AlignRight | Qt::AlignVCenter, tr("100%"));
	painter.drawText(QRectF(width() - margin_right + 4, chance_plot.top() - 2,
	                        margin_right - 6, 14),
	                 Qt::AlignLeft | Qt::AlignVCenter, tr("rain %"));

	/* --- the provenance ribbon (sec 3.4) ------------------------------ */
	for (int x = 0; x < plot.width(); ++x) {
		if (!columns[x].covered) {
			continue;
		}

		painter.setPen(band_colour(m_palette, columns[x].band));
		const double px = plot.left() + x;
		painter.drawLine(QPointF(px, chance_plot.bottom() + 2),
		                 QPointF(px, chance_plot.bottom() + 2 + ribbon_height));
	}

	/* --- now ---------------------------------------------------------- */
	const double now_x = plot.left() + (now - from) / seconds_per_pixel;
	painter.setPen(QPen(m_palette.now_marker, 1.5));
	painter.drawLine(QPointF(now_x, plot.top()),
	                 QPointF(now_x, chance_plot.bottom()));

	/* --- axis labels --------------------------------------------------- */
	painter.setPen(m_palette.axis_text);

	const QRectF temperature_top(2, plot.top() - 2, margin_left - 6, 14);
	painter.drawText(temperature_top, Qt::AlignRight | Qt::AlignVCenter,
	                 QString::number(temperature_high, 'f', 0) + tr(" C"));

	const QRectF temperature_bottom(2, plot.bottom() - 12, margin_left - 6, 14);
	painter.drawText(temperature_bottom, Qt::AlignRight | Qt::AlignVCenter,
	                 QString::number(temperature_low, 'f', 0) + tr(" C"));

	const QRectF rain_top(width() - margin_right + 4, plot.bottom() -
	                              plot.height() * 0.45 - 6, margin_right - 6, 14);
	painter.drawText(rain_top, Qt::AlignLeft | Qt::AlignVCenter,
	                 QString::number(rain_high, 'f', 1) + tr(" mm/h"));
}
