#ifndef BBQ_FORECAST_GRAPH_H
#define BBQ_FORECAST_GRAPH_H

#include <QColor>
#include <QSize>
#include <QRect>
#include <QTimeZone>
#include <QWidget>

#include <vector>

#include "graph/interpolate.h"
#include "ui/layout.h"
#include "ui/theme.h"
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

	/*
	 * The bias-corrected overlay (sec 12.5). Deliberately not one of the
	 * measured Weather Underground colours: it is not their data, and a
	 * curve wearing their palette would say it was.
	 */
	QColor corrected;

	/*
	 * Wind. Muted on purpose: it is a supporting quantity here -- it
	 * matters to the grilling score (sec 7) rather than being something
	 * the graph is read for -- and a fourth confident line would compete
	 * with the three that are.
	 */
	QColor wind;

	/*
	 * The line at local midnight. Stronger than the grid on purpose:
	 * "which day is this" is the question a multi-day graph gets asked
	 * most, and an hour tick cannot answer it.
	 */
	QColor day_divider;

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
/*
 * Local midnights in [from_utc, to_utc), in the LOCATION's clock.
 *
 * Free rather than a member, and declared here rather than kept private
 * to the paint code, because the interesting part of it is a claim that
 * wants asserting: a day is 23 or 25 hours on the two changeover
 * nights, so the cursor is advanced with QDateTime::addDays and never
 * by adding 86400 seconds. Buried in paintEvent that claim could only
 * be read, not checked -- and a stride error there would surface twice
 * a year, in a build nobody had touched, looking like a rendering fault
 * rather than an arithmetic one.
 *
 * `cap` bounds the result so that a pathological span cannot fill
 * memory; the graph never needs more than a few hundred.
 */
std::vector<qint64> bbq_day_boundaries(qint64 from_utc, qint64 to_utc,
                                       const QTimeZone &zone, int cap = 400);

/*
 * Where the readout box starts, given the cursor it points at and the
 * plot it must stay inside.
 *
 * Out here rather than inside paintEvent because the interesting case
 * cannot be reached from a desktop window: when the box is WIDER than
 * the plot, no position satisfies both edges, and the order the two
 * clamps are applied in decides which edge loses. The earlier shape
 * pushed left then right, so the right-hand clamp won and the box
 * hung off the LEFT -- taking the time and the temperature with it,
 * which are the two fields the shrinking loop works to keep.
 *
 * The left edge wins here, so an unfittable box is cut off at the
 * right by the widget's own clipping and reads from the front.
 */
double bbq_readout_box_x(double centre_px, double box_w,
                         double plot_left, double plot_right);

/*
 * What the readout calls the moment it is describing.
 *
 * One sample in the column and it is that sample's own start time. More
 * than one and the mean is what gets drawn (sec 3.7's rule that the
 * trace and the readout are replaced together), so a single timestamp
 * beside it would claim a precision nobody measured -- the same fault
 * sec 13.2 stops the sample marks committing when they would crowd.
 * The label becomes a range instead.
 */
QString bbq_readout_time_label(qint64 first_utc, qint64 last_utc,
                               int knot_count, bool wide,
                               const QTimeZone &zone);

class bbq_forecast_graph : public QWidget {
	Q_OBJECT

public:
	explicit bbq_forecast_graph(QWidget *parent = nullptr);

	void set_composite(bbq_composite composite);

	/*
	 * The bias-corrected overlay (sec 12.5), drawn over the forecast it
	 * corrects rather than replacing it. An empty series removes it,
	 * which is the normal state until enough has been verified.
	 */
	void set_corrected(bbq_series corrected);
	const bbq_composite &composite() const { return m_composite; }

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

	/*
	 * Wind, off by default. It earns its place through the grilling
	 * score rather than through the graph, so it is offered rather than
	 * imposed on a plot that already carries three quantities.
	 */
	/*
	 * Light, dark, or the device's answer (sec 10.3). Repaints, so the
	 * choice is answerable by looking.
	 */
	/*
	 * How steady the temperature axis is, 0 to 100 (sec 3.14).
	 *
	 * At 0 the scale follows exactly what is visible, which is precise
	 * and flutters while scrolling. Higher rounds the range outward and
	 * then HOLDS it until the data leaves, so panning moves the curve
	 * rather than the axis under it.
	 */
	void set_scale_steadiness(int percent);
	int scale_steadiness() const { return m_scale_steadiness; }

	void set_theme(bbq_theme theme);
	bbq_theme theme() const { return m_theme; }

	/*
	 * Paint the ground, or leave whatever is behind showing through.
	 *
	 * On for anything on a screen: a graph that does not fill its own
	 * rectangle shows the last frame under it. Off for the home-screen
	 * widget picture (project.md sec 16.23), which is rendered into a
	 * transparent image so the wallpaper carries the ground.
	 */
	void set_opaque_background(bool opaque);
	bool opaque_background() const { return m_opaque_background; }

	/*
	 * Lift every ink that must be legible until it clears `floor`
	 * against `ground` (project.md sec 16.25).
	 *
	 * For a picture drawn onto something this program does not own. An
	 * invalid ground turns it off, which is the default and is what the
	 * on-screen graph uses: there the ground IS this palette's own
	 * background, the colours were chosen against it, and Weather
	 * Underground's red is a measurement rather than a decoration.
	 *
	 * The set lifted is the set tool/palette_contrast.py already gates,
	 * so "which inks must be legible" is answered in one place. Grid
	 * lines and band shading are deliberately outside it: they are
	 * furniture, and furniture that clears a text floor is furniture
	 * competing with the data.
	 */
	void set_contrast_ground(const QColor &ground, double floor);
	QColor contrast_ground() const { return m_contrast_ground; }

	/*
	 * The colours in force, for a caller that has to draw on the same
	 * ground this does -- the home-screen picture's scrim is this
	 * background made translucent, so the widget is the window's scheme
	 * seen through a wallpaper rather than a second scheme nobody set.
	 */
	const bbq_graph_palette &palette_colours() const { return m_palette; }

	void set_show_wind(bool show);
	bool show_wind() const { return m_show_wind; }

	void set_show_samples(bool show);
	bool show_samples() const { return m_show_samples; }

	/*
	 * Park the readout on a given column without a mouse, so a
	 * rendered shot can show it. -1 clears.
	 */
	void set_cursor_column(int column);
	int cursor_column() const { return m_cursor_column; }

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

	/*
	 * The plot area as the last paint decided it, which is the geometry
	 * every pixels-to-seconds conversion here uses. Empty before the
	 * first paint. Exposed so the view arithmetic can be asserted
	 * against the same rectangle the handlers use rather than against a
	 * second guess at it.
	 */
	QRect plot_rect() const { return m_plot; }

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
	bool event(QEvent *event) override;
	void leaveEvent(QEvent *event) override;

private:
	/*
	 * Rebuild m_palette from the theme, clamped against the foreign
	 * ground when one has been named. See set_contrast_ground.
	 */
	void apply_palette();

	bbq_graph_palette m_palette;
	bbq_composite m_composite;
	bbq_series m_corrected;
	/*
	 * The DEFAULT visible window, as offsets from now in seconds --
	 * the span before anybody has panned or zoomed. It looks a little
	 * way back and a day forward, which is what a question about this
	 * afternoon actually needs: the hourly band reaches fifteen days
	 * and drawing all of it would compress today into a few pixels.
	 *
	 * Written by set_layout from the layout metrics, and read only
	 * where the view has not been set. There was a public setter for
	 * it once, superseded by set_layout for the default and set_view
	 * for the user's own window -- and removed rather than left,
	 * because it was the one entry point that could set a zero span,
	 * which divides by zero in seconds_per_pixel (sec 16.54).
	 */
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
	bool m_show_wind = false;
	bbq_theme m_theme = bbq_theme::automatic;

	/*
	 * The held temperature range. Kept across paints, which is the whole
	 * point: a scale recomputed from scratch every frame is a scale that
	 * moves every frame.
	 */
	int m_scale_steadiness = 60;
	double m_scale_low = 0.0;
	double m_scale_high = 0.0;
	bool m_scale_held = false;
	int m_smoothing_s = 30 * 60;
	bool m_show_windows = true;
	bool m_opaque_background = true;
	QColor m_contrast_ground;
	double m_contrast_floor = 3.0;

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
