#ifndef BBQ_FORECAST_GRAPH_H
#define BBQ_FORECAST_GRAPH_H

#include <QColor>
#include <QSize>
#include <QRect>
#include <QWidget>

#include "graph/interpolate.h"
#include "ui/layout.h"
#include "model/composite.h"
#include "model/grill.h"

class QMouseEvent;
class QWheelEvent;
class QPaintEvent;

/*
 * The colours, gathered in one place on purpose.
 *
 * MEASURED from Weather Underground's own station dashboard on
 * 2026-08-07, not chosen (project.md sec 3.8.2). Their chart is
 * client-rendered, so the values came out of a rendered screenshot
 * rather than a stylesheet, sampled by counting pixels.
 *
 * The band shading and the red are what make the WU chart recognisable
 * at a glance, which is what sec 0 asks for when it says the aesthetic
 * matters as much as the sample rate.
 */
struct bbq_graph_palette {
	QColor background;
	QColor band_shade;
	QColor grid;
	QColor axis_text;
	QColor temperature;
	QColor rain;
	QColor chance;
	QColor now_marker;
	QColor stale_warning;
	QColor grill_window;
	QColor readout_back;
	QColor readout_edge;

	/*
	 * The readout's text. Here rather than written into the painter at
	 * the point of use, which is where it was: this file's own opening
	 * note says a constant beside the palette is "a third opinion nobody
	 * set", and the box's back and edge were already palette entries
	 * while the ink on them was not.
	 */
	QColor readout_text;

	/* Per band, for the provenance ribbon (sec 3.4). */
	QColor band_observed;
	QColor band_current;
	QColor band_nowcast_fine;
	QColor band_nowcast;
	QColor band_extended;
	QColor band_hourly;
};

/*
 * The temperature and rain graph -- the reason this program exists
 * (project.md sec 3).
 *
 * Draws a bbq_composite across one continuous time axis. What it must
 * get right is written down in sec 3.1 to 3.7 and each rule shows up in
 * the painting:
 *
 *   - bands keep their identity, so the curve is one line while a
 *     ribbon underneath says which band produced each stretch of it
 *     (sec 3.4 -- provenance survives resolution);
 *   - rain downsamples by MAXIMUM per pixel column, never by mean,
 *     because a five-minute downpour meaned into an hour disappears
 *     and that is the signal this program exists to show (sec 3.5);
 *   - nothing is upsampled, and a column no band covers is a break in
 *     the line rather than a segment drawn across it (sec 3.5, 3.6);
 *   - no blending at the seams; a step that survives normalisation is
 *     information and is drawn (sec 3.7).
 *
 * Hand-painted with QPainter rather than QtCharts, which sec 3.8 held
 * as a prior to confirm by trying. See that section for what trying it
 * actually taught.
 */
class bbq_forecast_graph : public QWidget {
	Q_OBJECT

public:
	explicit bbq_forecast_graph(QWidget *parent = nullptr);

	void set_composite(bbq_composite composite);
	const bbq_composite &composite() const { return m_composite; }

	/*
	 * The visible time window, as offsets from now in seconds. The
	 * default looks a little way back and a day forward, which is the
	 * span a question about this afternoon actually needs -- the hourly
	 * band reaches fifteen days and drawing all of it would compress
	 * today into a few pixels.
	 */
	void set_window(qint64 before_s, qint64 after_s);

	/*
	 * Adopt a layout's numbers, including its time window (sec 10).
	 * Repaints, so switching is answerable by looking.
	 */
	void set_layout(bbq_layout layout);

	/*
	 * How the curve is drawn between samples, and whether the samples
	 * themselves are marked (project.md sec 3.11).
	 *
	 * The marks are what make a smoothed curve honest: they say where
	 * the data is while the curve says what is drawn between. Both
	 * repaint immediately, so the choice is answerable by looking.
	 */
	void set_interpolation(bbq_interpolation method);
	bbq_interpolation interpolation() const { return m_interpolation; }

	/*
	 * How hard the corners are rounded, in seconds of time (sec
	 * 3.11.4). Zero draws the knees as sharp as the data makes them.
	 */
	void set_smoothing(int seconds);
	int smoothing() const { return m_smoothing_s; }

	/* Shade the stretches worth lighting a fire in (sec 7). */
	void set_show_windows(bool show);
	bool show_windows() const { return m_show_windows; }

	void set_show_samples(bool show);
	bool show_samples() const { return m_show_samples; }

	/*
	 * Park the readout on a given column without a mouse, so a
	 * rendered shot can show it. -1 clears.
	 */
	void set_cursor_column(int column);

	/*
	 * The view, which is the user's rather than the layout's once they
	 * have touched it (project.md sec 13).
	 *
	 * The layout still supplies the span a fresh window opens at. After
	 * a drag or a wheel the view is explicit, and follow_now goes false
	 * so the clock stops dragging the graph out from under whoever is
	 * reading it. Double-click puts it back.
	 */
	void set_view(qint64 from_utc, qint64 span_s);
	void follow_now();
	bool is_following_now() const { return m_follow_now; }

	qint64 view_from_utc() const;
	qint64 view_span_s() const;

	QSize sizeHint() const override;

signals:
	/*
	 * The view moved. Whoever owns the data uses this to make sure the
	 * range being looked at is actually loaded (project.md sec 12.8).
	 */
	void view_changed(qint64 from_utc, qint64 to_utc);

public:

protected:
	void paintEvent(QPaintEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void leaveEvent(QEvent *event) override;

private:
	bbq_graph_palette m_palette;
	bbq_composite m_composite;
	qint64 m_before_s = 3 * 3600;
	qint64 m_after_s = 21 * 3600;

	/*
	 * Akima and half an hour of rounding, which is what this data
	 * actually looks best under (sec 3.11.5).
	 *
	 * Akima because the shape here is plateaus beside fast drops and it
	 * keeps a sharp change local instead of ringing the flat parts.
	 * Rounding because the source quantises to whole degrees, so
	 * without it every step is a hard knee that is an artefact of the
	 * reporting rather than of the weather.
	 *
	 * Both are departures from the safest possible defaults, and both
	 * are visible and reversible from the controls -- with the samples
	 * marked, so what the data actually says is never hidden by them.
	 */
	bbq_metrics m_metrics;
	bbq_interpolation m_interpolation = bbq_interpolation::akima;
	bool m_show_samples = true;
	int m_smoothing_s = 30 * 60;
	bool m_show_windows = true;

	/*
	 * Where the readout is pointing, as a column index into the plot,
	 * or -1 for nowhere.
	 */
	int m_cursor_column = -1;

	/*
	 * The view. Zero span means "not set yet", so the layout's window is
	 * used until something moves it.
	 */
	bool m_follow_now = true;
	qint64 m_view_from = 0;
	qint64 m_view_span_s = 0;

	/*
	 * The plot rectangle from the last paint.
	 *
	 * A drag has to convert pixels to seconds, and the geometry that
	 * conversion needs is decided in paintEvent -- the right margin is
	 * MEASURED from the gutter text rather than fixed, so it cannot
	 * simply be recomputed here without saying the same thing twice.
	 * Empty until the first paint, and every handler checks.
	 */
	QRect m_plot;

	bool m_dragging = false;
	double m_drag_x = 0.0;
	qint64 m_drag_from = 0;
};

#endif
