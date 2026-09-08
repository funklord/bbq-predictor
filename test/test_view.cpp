#include <QApplication>
#include <QFile>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <limits>
#include <QPainter>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>

#include <QAccessible>
#include <QLabel>
#include <QScrollBar>
#include <QSlider>

#include "graph/forecast_graph.h"
#include "graph/simplify.h"
#include "model/composite.h"
#include "model/grill.h"
#include <QTimeZone>
#include "ui/accessibility.h"
#include "ui/theme.h"

/*
 * Panning and zooming (project.md sec 13).
 *
 * This is the only widget test in the suite, and it exists because the
 * gesture handlers are the most interaction-heavy code in the project
 * and were verified by rendering pictures and reasoning about them.
 * Rendering shows that a view was honoured; it does not show that the
 * arithmetic behind the gesture is right, and the anchor invariant
 * below is exactly the kind of thing that looks correct in a screenshot
 * while being subtly wrong.
 *
 * The handlers are protected, so they are reached through a subclass
 * rather than by faking events through the window system -- there is no
 * window manager here and a synthetic click would be testing Qt rather
 * than this code.
 */
class probe : public bbq_forecast_graph {
public:
	using bbq_forecast_graph::mouseMoveEvent;
	using bbq_forecast_graph::mousePressEvent;
	using bbq_forecast_graph::mouseReleaseEvent;
	using bbq_forecast_graph::wheelEvent;
};

class test_view : public QObject {
	Q_OBJECT

private slots:
	void bbq_flatten_matches_qt();
	void the_sample_dots_follow_a_theme_change();
	void a_dot_stamp_carries_the_ratio_it_was_rendered_at();
	void bbq_simplify_keeps_every_point_within_tolerance();
	void bbq_simplify_keeps_what_a_curve_needs();
	void a_fresh_graph_follows_the_clock();
	void the_span_is_bounded_at_both_ends();
	void zooming_holds_the_moment_under_the_cursor();
	void dragging_moves_time_with_the_hand();
	void double_click_comes_back_to_now();
	void a_theme_setting_lands_somewhere_defined();
	void automatic_never_answers_unknown();
	void automatic_releases_the_override();

	/*
	 * The home-screen widget's ground (sec 16.23).
	 */
	void the_ground_is_painted_unless_it_is_turned_off();

	/*
	 * A grilling window's edge rules, and the clipping (sec 16.30.1).
	 */
	void a_window_boundary_in_view_is_drawn();
	void a_window_running_off_the_edge_draws_no_rule_there();
	void a_window_lands_where_it_lands_whatever_range_was_asked();
	void the_curve_clears_the_floor_against_whatever_it_crosses();
	void a_readout_too_wide_to_fit_keeps_its_left_edge();
	void a_column_holding_several_samples_reads_as_a_range();
	void day_furniture_goes_away_once_the_days_would_crowd();
	void the_window_scan_is_kept_and_forgotten_with_the_composite();

	/*
	 * The contrast clamp the home-screen picture draws through
	 * (sec 16.25).
	 */
	void the_contrast_ratio_agrees_with_a_published_pair();
	void an_ink_is_walked_until_it_clears_the_floor();
	void a_clamped_palette_lifts_the_curve_and_leaves_furniture();
	void naming_no_ground_leaves_every_colour_where_it_was();

	/*
	 * Tier 4: the scheme a TDE or KDE 3 desktop writes to kdeglobals,
	 * which is the only place it says so. The parser takes its sources
	 * as an argument precisely so this runs without such a desktop.
	 */
	void the_desktops_own_colours_are_read_when_qt_says_nothing();
	void a_light_scheme_in_the_same_format_reads_light();
	void the_colours_decide_and_the_scheme_name_does_not();
	void the_keys_are_taken_from_general_and_not_another_section();
	void a_file_that_cannot_be_read_abstains_rather_than_guessing();
	void a_half_written_scheme_abstains_and_the_next_file_answers();

	/* The second dialect: LXQt states the same thing another way. */
	void an_lxqt_palette_is_read_though_it_spells_it_differently();
	void an_lxqt_light_palette_reads_light();
	void an_lxqt_file_named_light_holding_dark_colours_reads_dark();
	void the_applied_lxqt_palette_is_read_from_lxqt_conf_itself();
	void every_day_boundary_is_local_midnight();
	void the_short_night_is_twenty_three_hours();
	void the_long_night_is_twenty_five_hours();
	void a_boundary_on_the_left_edge_is_kept();
	void the_count_is_bounded();
	void a_slider_reports_no_value_to_accessibility();
	void the_temperature_line_survives_a_certain_downpour();
	void showing_the_wind_does_not_touch_the_temperature_line();
	void a_pixel_sized_font_is_scaled_rather_than_refused();

private:
	static void paint_once(probe &graph);
	static double seconds_per_pixel(const probe &graph);
	static double time_under(const probe &graph, double x);
};

void test_view::paint_once(probe &graph) {
	/*
	 * The handlers need the plot rectangle, and it is decided during
	 * painting because the right margin is measured from the gutter
	 * text. Grabbing is how a paint is forced without a window manager.
	 */
	graph.resize(900, 400);
	graph.grab();
}

double test_view::seconds_per_pixel(const probe &graph) {
	return static_cast<double>(graph.view_span_s()) / graph.plot_rect().width();
}

double test_view::time_under(const probe &graph, double x) {
	const double offset = x - graph.plot_rect().left();
	return graph.view_from_utc() + offset * seconds_per_pixel(graph);
}

void test_view::a_fresh_graph_follows_the_clock() {
	probe graph;
	paint_once(graph);

	QVERIFY(graph.is_following_now());

	/*
	 * The layout supplies the span a fresh window opens at, and the
	 * window tracks the clock until somebody touches it (sec 13).
	 */
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	QVERIFY(qAbs(graph.view_from_utc() - (now - 3 * 3600)) <= 2);
	QCOMPARE(graph.view_span_s(), static_cast<qint64>(24 * 3600));
}

void test_view::the_span_is_bounded_at_both_ends() {
	probe graph;
	paint_once(graph);

	graph.set_view(1000000, 1);
	QVERIFY2(graph.view_span_s() >= 15 * 60,
	         "the span was allowed below the floor, where a pixel is less "
	         "than a second and there is nothing finer to look at");

	graph.set_view(1000000, 500LL * 365 * 24 * 3600);
	QVERIFY2(graph.view_span_s() <= 10LL * 365 * 24 * 3600,
	         "the span was allowed past the ceiling, where the whole "
	         "history is one column of ink");

	/* Touching the view stops it following the clock. */
	QVERIFY(!graph.is_following_now());
}

void test_view::zooming_holds_the_moment_under_the_cursor() {
	probe graph;
	paint_once(graph);

	graph.set_view(1000000, 24 * 3600);

	/*
	 * The invariant that makes zooming feel right: whatever is under the
	 * pointer stays under it. Zooming about the centre instead slides
	 * the thing being examined away exactly while it is examined -- and
	 * a screenshot of the result looks perfectly reasonable either way,
	 * which is why this is asserted rather than looked at.
	 */
	const double cursor = graph.plot_rect().left() + 300.0;
	const double before = time_under(graph, cursor);

	for (int step = 0; step < 4; ++step) {
		QWheelEvent zoom_in(QPointF(cursor, 100.0), QPointF(cursor, 100.0),
		                    QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
		                    Qt::NoModifier, Qt::NoScrollPhase, false);
		graph.wheelEvent(&zoom_in);
	}

	const double zoomed = time_under(graph, cursor);
	QVERIFY(graph.view_span_s() < 24 * 3600);

	/* Within a second of a pixel's worth of time. */
	QVERIFY2(qAbs(zoomed - before) < seconds_per_pixel(graph) + 1.0,
	         "the moment under the cursor moved while zooming about it");

	/* And back out again, to the same moment. */
	for (int step = 0; step < 4; ++step) {
		QWheelEvent zoom_out(QPointF(cursor, 100.0), QPointF(cursor, 100.0),
		                     QPoint(0, 0), QPoint(0, -120), Qt::NoButton,
		                     Qt::NoModifier, Qt::NoScrollPhase, false);
		graph.wheelEvent(&zoom_out);
	}

	QVERIFY(qAbs(time_under(graph, cursor) - before) <
	        seconds_per_pixel(graph) + 1.0);
}

void test_view::dragging_moves_time_with_the_hand() {
	probe graph;
	paint_once(graph);

	graph.set_view(1000000, 24 * 3600);

	const double grab_x = graph.plot_rect().left() + 400.0;
	const double grabbed = time_under(graph, grab_x);
	const double per_pixel = seconds_per_pixel(graph);

	QMouseEvent press(QEvent::MouseButtonPress, QPointF(grab_x, 100.0),
	                  QPointF(grab_x, 100.0), Qt::LeftButton, Qt::LeftButton,
	                  Qt::NoModifier);
	graph.mousePressEvent(&press);

	/* Thrown 150 pixels to the right: time under the hand goes with it. */
	const double moved_x = grab_x + 150.0;
	QMouseEvent move(QEvent::MouseMove, QPointF(moved_x, 100.0),
	                 QPointF(moved_x, 100.0), Qt::NoButton, Qt::LeftButton,
	                 Qt::NoModifier);
	graph.mouseMoveEvent(&move);

	QCOMPARE(graph.view_span_s(), static_cast<qint64>(24 * 3600));
	QVERIFY2(qAbs(time_under(graph, moved_x) - grabbed) < per_pixel + 1.0,
	         "the plot did not track the hand: the moment grabbed is no "
	         "longer under the pointer that grabbed it");

	QMouseEvent release(QEvent::MouseButtonRelease, QPointF(moved_x, 100.0),
	                    QPointF(moved_x, 100.0), Qt::LeftButton, Qt::NoButton,
	                    Qt::NoModifier);
	graph.mouseReleaseEvent(&release);

	/* Released, so further movement is a hover and not a pan. */
	const qint64 settled = graph.view_from_utc();
	QMouseEvent hover(QEvent::MouseMove, QPointF(moved_x + 200.0, 100.0),
	                  QPointF(moved_x + 200.0, 100.0), Qt::NoButton,
	                  Qt::NoButton, Qt::NoModifier);
	graph.mouseMoveEvent(&hover);
	QCOMPARE(graph.view_from_utc(), settled);
}

void test_view::double_click_comes_back_to_now() {
	probe graph;
	paint_once(graph);

	graph.set_view(1000000, 3600);
	QVERIFY(!graph.is_following_now());

	graph.follow_now();

	QVERIFY(graph.is_following_now());
	QCOMPARE(graph.view_span_s(), static_cast<qint64>(24 * 3600));

	const qint64 now = QDateTime::currentSecsSinceEpoch();
	QVERIFY(qAbs(graph.view_from_utc() - (now - 3 * 3600)) <= 2);
}

void test_view::a_theme_setting_lands_somewhere_defined() {
	QCOMPARE(bbq_theme_resolve(QStringLiteral("light")), bbq_theme::light);
	QCOMPARE(bbq_theme_resolve(QStringLiteral("dark")), bbq_theme::dark);
	QCOMPARE(bbq_theme_resolve(QStringLiteral("auto")), bbq_theme::automatic);

	/*
	 * A config file is edited by hand. "Dark ", "DARK" and a typo must
	 * all land somewhere defined rather than somewhere undefined -- the
	 * same rule the layout setting follows, and the reason both read
	 * through a resolver instead of comparing strings at the call site.
	 */
	QCOMPARE(bbq_theme_resolve(QStringLiteral("  DARK ")), bbq_theme::dark);
	QCOMPARE(bbq_theme_resolve(QStringLiteral("Light")), bbq_theme::light);
	QCOMPARE(bbq_theme_resolve(QString()), bbq_theme::automatic);
	QCOMPARE(bbq_theme_resolve(QStringLiteral("midnight")), bbq_theme::automatic);
}

void test_view::automatic_never_answers_unknown() {
	QCOMPARE(bbq_theme_scheme(bbq_theme::light), Qt::ColorScheme::Light);
	QCOMPARE(bbq_theme_scheme(bbq_theme::dark), Qt::ColorScheme::Dark);

	/*
	 * Qt returns Unknown where the platform has no opinion, and a caller
	 * choosing a palette has to pick something. Leaving three cases for
	 * a two-valued question would push the same decision out to every
	 * call site, differently each time.
	 */
	const Qt::ColorScheme resolved = bbq_theme_scheme(bbq_theme::automatic);
	QVERIFY(resolved == Qt::ColorScheme::Light ||
	        resolved == Qt::ColorScheme::Dark);
}

void test_view::automatic_releases_the_override() {
	/*
	 * Asserted on the PALETTE, not on the colour-scheme hint.
	 *
	 * The hint is advisory and a platform may ignore it: offscreen does,
	 * reporting Unknown straight after setColorScheme(Dark). That is not
	 * a fault to work around -- it is the reason bbq_theme_apply sets a
	 * palette explicitly rather than asking and hoping, which the first
	 * rendering had already shown when the graph went dark and the
	 * controls stayed light.
	 *
	 * So the test checks the thing that carries the theme. Asserting on
	 * the hint would have passed on a platform that honours it and
	 * failed on one that does not, while telling us nothing about what
	 * the user sees on either.
	 */
	bbq_theme_apply(bbq_theme::dark);
	const QColor dark_window = QGuiApplication::palette().color(QPalette::Window);

	bbq_theme_apply(bbq_theme::light);
	const QColor light_window = QGuiApplication::palette().color(QPalette::Window);

	QVERIFY2(dark_window != light_window,
	         "light and dark produced the same window colour");
	QVERIFY2(dark_window.lightness() < light_window.lightness(),
	         "the dark scheme is not darker than the light one");

	/*
	 * Automatic resolves to one of the two rather than to a third thing,
	 * so whatever the device says, the applet has a palette.
	 */
	bbq_theme_apply(bbq_theme::automatic);
	const QColor automatic_window =
	        QGuiApplication::palette().color(QPalette::Window);

	QVERIFY(automatic_window == dark_window || automatic_window == light_window);
}

/*
 * The day boundaries, and the reason these exist at all.
 *
 * Stockholm is the location this project was written for and it keeps
 * summer time, so both changeover nights are real here: the clocks go
 * forward on the last Sunday of March and back on the last Sunday of
 * October. A stride of 86400 seconds walks off the boundary on the
 * first of those and stays off it, which is a fault that appears twice
 * a year in a build nobody touched.
 *
 * The zone is NAMED rather than taken from the machine. A test that
 * asked the system for its timezone would pass in Stockholm, pass
 * uselessly in UTC where there is no transition to get wrong, and be
 * unreproducible everywhere else.
 */
namespace {

const QTimeZone stockholm(QByteArrayLiteral("Europe/Stockholm"));

qint64 at_local(int year, int month, int day, int hour = 0) {
	const QDateTime when(QDate(year, month, day), QTime(hour, 0), stockholm);
	return when.toSecsSinceEpoch();
}

} // namespace

void test_view::every_day_boundary_is_local_midnight() {
	QVERIFY(stockholm.isValid());

	const qint64 from = at_local(2026, 3, 26, 9);
	const qint64 to = at_local(2026, 4, 2, 9);

	const std::vector<qint64> found = bbq_day_boundaries(from, to, stockholm);

	QCOMPARE(static_cast<int>(found.size()), 7);

	for (qint64 when : found) {
		const QDateTime local = QDateTime::fromSecsSinceEpoch(when, stockholm);
		QCOMPARE(local.time(), QTime(0, 0));
	}
}

void test_view::the_short_night_is_twenty_three_hours() {
	/*
	 * 29 March 2026, the spring transition: 02:00 becomes 03:00, so the
	 * gap from that midnight to the next is 23 hours. The assertion is
	 * on the GAP rather than on the timestamp, because that is what a
	 * fixed stride would get wrong.
	 */
	const std::vector<qint64> found =
	        bbq_day_boundaries(at_local(2026, 3, 28, 12),
	                           at_local(2026, 3, 31, 12), stockholm);

	QCOMPARE(static_cast<int>(found.size()), 3);
	QCOMPARE(found.at(1) - found.at(0), 23 * 3600);
	QCOMPARE(found.at(2) - found.at(1), 24 * 3600);
}

void test_view::the_long_night_is_twenty_five_hours() {
	/* 25 October 2026, the autumn transition: 03:00 becomes 02:00. */
	const std::vector<qint64> found =
	        bbq_day_boundaries(at_local(2026, 10, 24, 12),
	                           at_local(2026, 10, 27, 12), stockholm);

	QCOMPARE(static_cast<int>(found.size()), 3);
	QCOMPARE(found.at(1) - found.at(0), 25 * 3600);
	QCOMPARE(found.at(2) - found.at(1), 24 * 3600);
}

void test_view::a_boundary_on_the_left_edge_is_kept() {
	/*
	 * A view starting exactly at midnight has a boundary there, and
	 * dropping it would leave the leftmost day unnamed. The right edge
	 * is the other way round: the range is half open, so a boundary
	 * exactly at `to` belongs to the next view rather than this one.
	 */
	const qint64 midnight = at_local(2026, 6, 1);

	const std::vector<qint64> inclusive =
	        bbq_day_boundaries(midnight, at_local(2026, 6, 2), stockholm);
	QCOMPARE(static_cast<int>(inclusive.size()), 1);
	QCOMPARE(inclusive.at(0), midnight);

	const std::vector<qint64> empty =
	        bbq_day_boundaries(midnight, midnight, stockholm);
	QVERIFY(empty.empty());
}

void test_view::the_count_is_bounded() {
	/* Ten years asked for, and the cap is what comes back. */
	const std::vector<qint64> found =
	        bbq_day_boundaries(at_local(2026, 1, 1), at_local(2036, 1, 1),
	                           stockholm, 400);

	QCOMPARE(static_cast<int>(found.size()), 400);
}


void test_view::a_slider_reports_no_value_to_accessibility() {
	/*
	 * Sec 10.6. Qt's Android bridge builds an AccessibilityNodeInfo
	 * RangeInfo for any widget whose accessible interface offers a
	 * VALUE, using a constructor that does not exist before API 33, and
	 * aborts the process when it fails. The workaround hands those
	 * widgets an interface with no value interface at all.
	 *
	 * That property is what this checks. It cannot check the Android
	 * half -- there is no Android here -- but it checks the half that
	 * was written, and the half that would silently stop working if a
	 * later Qt returned something else from QAccessibleWidget.
	 */
	QSlider slider;
	QScrollBar bar;
	QLabel label;

	QAccessibleInterface *from_slider =
	        bbq_accessible_without_value(QString(), &slider);
	QVERIFY2(from_slider != nullptr, "the factory declined a slider");
	QVERIFY2(from_slider->valueInterface() == nullptr,
	         "a slider still offers a value, which is what crashes Android");

	/*
	 * The scrollbar matters more than the slider: Qt creates those
	 * itself inside every scrollable view, so no application choice
	 * avoids them.
	 */
	QAccessibleInterface *from_bar = bbq_accessible_without_value(QString(), &bar);
	QVERIFY2(from_bar != nullptr, "the factory declined a scrollbar");
	QVERIFY2(from_bar->valueInterface() == nullptr,
	         "a scrollbar still offers a value");

	/*
	 * Everything else is left to Qt. Declining is how the workaround
	 * stays narrow: a factory that answered for every object would
	 * replace accessibility wholesale rather than withhold one field.
	 */
	QVERIFY2(bbq_accessible_without_value(QString(), &label) == nullptr,
	         "the factory answered for a widget it has no business with");

	/*
	 * HANDED TO THEIR OWNER, which is not the same as deleted.
	 *
	 * In the running program Qt's accessibility cache owns whatever a
	 * factory returns. This test calls the factory directly and
	 * registered nothing, so the two interfaces were nobody's -- leaked
	 * until a sanitizer run said so: 240 bytes in 8 allocations, the
	 * two objects and what QAccessibleWidget hangs off them.
	 *
	 * `delete` does not compile here, and that is Qt being explicit
	 * rather than awkward: `~QAccessibleInterface` is PROTECTED, which
	 * says the cache destroys these and a caller does not. So they are
	 * registered and released through the same door they would take in
	 * the application.
	 */
	QAccessible::deleteAccessibleInterface(
	        QAccessible::registerAccessibleInterface(from_slider));
	QAccessible::deleteAccessibleInterface(
	        QAccessible::registerAccessibleInterface(from_bar));
}

/*
 * Its own main, so the platform is chosen here rather than depending on
 * whatever ran it. Widgets need a QApplication; offscreen needs no
 * display.
 */
void test_view::the_temperature_line_survives_a_certain_downpour() {
	/*
	 * Red on top of blue, asserted in pixels (project.md sec 3.19).
	 *
	 * Rain chance used to have a panel of its own, where nothing could
	 * hide behind it, and it was drawn last. Sharing the plot at full
	 * height it would cover the temperature line on any hour the chance
	 * was high -- which is to say on exactly the hours somebody is
	 * looking at the chart to decide about.
	 *
	 * A dry forecast cannot ask this question: with no rain the area is
	 * flat against the bottom and the line is untouched however the
	 * painting is ordered. So the fixture is certain rain, which is the
	 * case where the wrong order and the right one differ.
	 */
	probe graph;

	std::vector<bbq_sample> samples;
	for (int i = 0; i < 24; ++i) {
		bbq_sample sample;
		sample.start_utc = 1600000000 + i * 3600;
		sample.duration_s = 3600;
		sample.temperature = 15.0 + (i % 6);
		sample.precip_chance = 100.0;
		sample.precip_rate = 8.0;
		samples.push_back(sample);
	}

	bbq_series band(bbq_band::hourly, QStringLiteral("test"));
	band.set_samples(std::move(samples));

	bbq_composite composite;
	composite.set_series(std::move(band));
	graph.set_composite(composite);

	graph.resize(900, 400);
	graph.set_view(1600000000, 12 * 3600);

	const QImage shot = graph.grab().toImage();

	/*
	 * The temperature red, exactly. A wash drawn over it would blend
	 * rather than cover, so the test is for the colour ARRIVING
	 * unmixed -- which is what "on top" means and what a blend would
	 * quietly fail.
	 */
	const QRgb wanted = qRgb(0xd5, 0x20, 0x2a);
	int found = 0;
	for (int y = 0; y < shot.height(); ++y) {
		for (int x = 0; x < shot.width(); ++x) {
			if (shot.pixel(x, y) == wanted) {
				++found;
			}
		}
	}

	QVERIFY2(found > 100,
	         qPrintable(QStringLiteral("the temperature line is not drawn over "
	                                   "the rain: %1 unmixed red pixels")
	                            .arg(found)));
}

void test_view::showing_the_wind_does_not_touch_the_temperature_line() {
	/*
	 * Wind is context, not a headline (project.md sec 3.19.1).
	 *
	 * Its comment says it is drawn under everything else, and it was
	 * painted after the temperature -- so wherever the two crossed, a
	 * dotted grey line put holes in the red one. Harmless-looking, and
	 * the same defect as the rain chance one directly above it: an
	 * ordering that was fine when the series had somewhere else to be.
	 *
	 * ASSERTED AS A RELATIONSHIP rather than a count, which is what
	 * makes it sharp. Contriving a fixture where wind and temperature
	 * coincide needs both scales solved simultaneously and they are
	 * derived from the data. But whether the wind is SHOWN cannot
	 * change the temperature line at all -- so the same render with the
	 * wind on and off must give pixel-identical red, and every crossing
	 * is a place where a wrong order breaks that.
	 */
	probe graph;

	std::vector<bbq_sample> samples;
	for (int i = 0; i < 24; ++i) {
		bbq_sample sample;
		sample.start_utc = 1600000000 + i * 3600;
		sample.duration_s = 3600;

		/* Opposed ramps, so they cross in the middle of the plot. */
		sample.temperature = 10.0 + i;
		sample.wind_kph = 40.0 - i;
		samples.push_back(sample);
	}

	bbq_series band(bbq_band::hourly, QStringLiteral("test"));
	band.set_samples(std::move(samples));

	bbq_composite composite;
	composite.set_series(std::move(band));
	graph.set_composite(composite);
	graph.resize(900, 400);
	graph.set_view(1600000000, 23 * 3600);

	const QRgb red = qRgb(0xd5, 0x20, 0x2a);

	const auto red_pixels = [&](bool wind) {
		graph.set_show_wind(wind);
		const QImage shot = graph.grab().toImage();

		QSet<QPair<int, int>> found;
		for (int y = 0; y < shot.height(); ++y) {
			for (int x = 0; x < shot.width(); ++x) {
				if (shot.pixel(x, y) == red) {
					found.insert(qMakePair(x, y));
				}
			}
		}
		return found;
	};

	const QSet<QPair<int, int>> without = red_pixels(false);
	const QSet<QPair<int, int>> with = red_pixels(true);

	QVERIFY2(!without.isEmpty(), "no temperature line was drawn at all");

	const QSet<QPair<int, int>> lost = without - with;
	QVERIFY2(lost.isEmpty(),
	         qPrintable(QStringLiteral("showing the wind erased %1 pixels of "
	                                   "the temperature line")
	                            .arg(lost.size())));
}

namespace {

/* Set while the next test paints, so the handler knows to watch. */
bool g_watching_fonts = false;
int g_font_complaints = 0;
QtMessageHandler g_previous_handler = nullptr;

void count_font_complaints(QtMsgType type, const QMessageLogContext &context,
                           const QString &text) {
	if (g_watching_fonts && text.contains(QStringLiteral("Point size"))) {
		++g_font_complaints;
	}

	if (g_previous_handler != nullptr) {
		g_previous_handler(type, context, text);
	}
}

} // namespace

void test_view::a_pixel_sized_font_is_scaled_rather_than_refused() {
	/*
	 * The gutter labels are scaled by a metric, and the scaling was
	 * written as setPointSizeF(pointSizeF() * k) (project.md sec 3.21).
	 *
	 * On Android the UI font is sized in PIXELS, so pointSizeF() is -1,
	 * the product is negative, Qt refuses it, and the label keeps the
	 * size it already had. The scaling did nothing on the one platform
	 * whose labels most needed it, and complained to a log nobody
	 * reads -- which is how it survived.
	 *
	 * Asserted by COUNTING the complaint rather than by measuring a
	 * font: what went wrong is that Qt was asked for something
	 * impossible, and the refusal is the evidence.
	 */
	probe graph;

	QFont pixel_sized = graph.font();
	pixel_sized.setPixelSize(16);
	QVERIFY2(pixel_sized.pointSizeF() < 0.0,
	         "a pixel-sized font still reports a point size, so this "
	         "fixture cannot ask its question");
	graph.setFont(pixel_sized);

	g_font_complaints = 0;
	g_previous_handler = qInstallMessageHandler(count_font_complaints);
	g_watching_fonts = true;

	graph.resize(900, 400);
	graph.grab();

	g_watching_fonts = false;
	qInstallMessageHandler(g_previous_handler);

	QCOMPARE(g_font_complaints, 0);
}

int main(int argc, char *argv[]) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);
	test_view suite;
	return QTest::qExec(&suite, argc, argv);
}

#include "test_view.moc"

/*
 * Tier 4, and why the applet needs it.
 *
 * A Trinity or KDE 3 session exposes no Qt 6 platform theme, so the
 * hint answers Unknown and `automatic` falls to light -- a white
 * rectangle at night on a desktop that has said, in the only place it
 * says it, that it is dark. That is precisely what sec 10.4 added a
 * dark mode to stop: not a style choice somebody made, a torch.
 *
 * The values are the real ones, measured on the desktop that prompted
 * the rule: windowBackground=0,42,78 against windowForeground=220,220,220.
 */
namespace {

QString write_kdeglobals(const QTemporaryDir &dir, const QString &name,
                          const QString &body) {
	const QString path = dir.filePath(name);
	QFile file(path);
	file.open(QIODevice::WriteOnly | QIODevice::Text);
	file.write(body.toUtf8());
	file.close();
	return path;
}

}  // namespace

void test_view::the_desktops_own_colours_are_read_when_qt_says_nothing() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("dark"),
	    QStringLiteral("[General]\n"
	                    "colorScheme=DarkBlue.kcsrc\n"
	                    "windowBackground=0,42,78\n"
	                    "windowForeground=220,220,220\n"));
	QCOMPARE(bbq_scheme_from_desktop_files({path}), Qt::ColorScheme::Dark);
}

/* The control. Without it a parser hardcoded to dark would pass. */
void test_view::a_light_scheme_in_the_same_format_reads_light() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("light"),
	    QStringLiteral("[General]\n"
	                    "windowBackground=255,255,255\n"
	                    "windowForeground=0,0,0\n"));
	QCOMPARE(bbq_scheme_from_desktop_files({path}), Qt::ColorScheme::Light);
}

/*
 * The name is not a predicate. This desktop's scheme is called
 * DarkBlue.kcsrc and IS dark, which is luck. So a file whose name says
 * dark and whose colours say light must read light, or the parser is
 * reading the name and passing the case above by coincidence.
 */
void test_view::the_colours_decide_and_the_scheme_name_does_not() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("misnamed"),
	    QStringLiteral("[General]\n"
	                    "colorScheme=DarkBlue.kcsrc\n"
	                    "windowBackground=255,255,255\n"
	                    "windowForeground=0,0,0\n"));
	QCOMPARE(bbq_scheme_from_desktop_files({path}), Qt::ColorScheme::Light);
}

/*
 * kdeglobals carries per-application sections using the same key names.
 * Taking whichever came last would answer about some other program's
 * colours -- here a dark [General] followed by a light section.
 */
void test_view::the_keys_are_taken_from_general_and_not_another_section() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("sectioned"),
	    QStringLiteral("[General]\n"
	                    "windowBackground=0,42,78\n"
	                    "windowForeground=220,220,220\n"
	                    "[konsole]\n"
	                    "windowBackground=255,255,255\n"
	                    "windowForeground=0,0,0\n"));
	QCOMPARE(bbq_scheme_from_desktop_files({path}), Qt::ColorScheme::Dark);
}

/*
 * Abstain rather than guess. A wrong light answer is merely plain; a
 * wrong dark one is unreadable text on a pale ground. So no readable
 * file means Unknown -- NOT light dressed up as an answer, which is the
 * caller's default to apply and not this function's to invent.
 */
void test_view::a_file_that_cannot_be_read_abstains_rather_than_guessing() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString absent = dir.filePath(QStringLiteral("no-such-file"));
	QCOMPARE(bbq_scheme_from_desktop_files({absent}), Qt::ColorScheme::Unknown);
	QCOMPARE(bbq_scheme_from_desktop_files({}), Qt::ColorScheme::Unknown);
}

/*
 * A file missing one of the pair says nothing, and the list is an order
 * of authority rather than a set: the next file answers. Both halves in
 * one case, because a parser skipping the first file for the wrong
 * reason would still pass the second half alone.
 */
void test_view::a_half_written_scheme_abstains_and_the_next_file_answers() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString half = write_kdeglobals(dir, QStringLiteral("half"),
	    QStringLiteral("[General]\n"
	                    "windowBackground=0,42,78\n"));
	QCOMPARE(bbq_scheme_from_desktop_files({half}), Qt::ColorScheme::Unknown);

	const QString whole = write_kdeglobals(dir, QStringLiteral("whole"),
	    QStringLiteral("[General]\n"
	                    "windowBackground=0,42,78\n"
	                    "windowForeground=220,220,220\n"));
	QCOMPARE(bbq_scheme_from_desktop_files({half, whole}), Qt::ColorScheme::Dark);
}

/*
 * The second dialect. LXQt is the other session installed on the machine
 * this was written on, and before this the parser found NOTHING in its
 * config -- not a wrong answer, which would have been noticed, but
 * silence, which reads as no opinion and falls through to light. That is
 * the failure this rung exists to prevent, one desktop over.
 */
void test_view::an_lxqt_palette_is_read_though_it_spells_it_differently() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("pal-dark"),
	    QStringLiteral("[Palette]\n"
	                    "base_color=#282828\n"
	                    "window_color=#232323\n"
	                    "window_text_color=#e1e6e6\n"));
	QCOMPARE(bbq_scheme_from_desktop_files({path}), Qt::ColorScheme::Dark);
}

void test_view::an_lxqt_light_palette_reads_light() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("pal-light"),
	    QStringLiteral("[Palette]\n"
	                    "window_color=#efefef\n"
	                    "window_text_color=#000000\n"));
	QCOMPARE(bbq_scheme_from_desktop_files({path}), Qt::ColorScheme::Light);
}

/*
 * Of the twelve palettes LXQt ships, luminance classifies all twelve
 * correctly while EIGHT are named something that says nothing. Here the
 * file is called "Light" and holds Silver's colours, which are dark, so
 * a substring test gets it backwards.
 */
void test_view::an_lxqt_file_named_light_holding_dark_colours_reads_dark() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("Light"),
	    QStringLiteral("[Palette]\n"
	                    "window_color=#636464\n"
	                    "window_text_color=#f9f9f9\n"));
	QCOMPARE(bbq_scheme_from_desktop_files({path}), Qt::ColorScheme::Dark);
}

/*
 * **The applied palette lives in lxqt.conf, not in the library.** This
 * first read <data>/lxqt/palettes/<theme=>, wrong twice over: that file
 * is loaded only when palette_override is on, so it can describe a
 * palette nobody is using; and LXQt title-cases the name before building
 * the path, so seven of the twelve themes installed here -- ambiance,
 * dark, frost, kvantum, light, silver, system -- did not resolve at all.
 * The shipped /etc/xdg/lxqt/lxqt.conf says theme=frost, so a DEFAULT
 * install silently found nothing: the failure this rung removes.
 */
void test_view::the_applied_lxqt_palette_is_read_from_lxqt_conf_itself() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString conf = write_kdeglobals(dir, QStringLiteral("lxqt.conf"),
	    QStringLiteral("[General]\n"
	                    "theme=frost\n"
	                    "icon_theme=oxygen\n"
	                    "[Palette]\n"
	                    "window_color=#232323\n"
	                    "window_text_color=#e1e6e6\n"
	                    "[Qt]\n"
	                    "style=Fusion\n"));
	/* theme= is lowercase and names no palette file; the answer is in
	 * this file regardless. */
	QCOMPARE(bbq_scheme_from_desktop_files({conf}), Qt::ColorScheme::Dark);
}

/*
 * THE WIDGET'S TRANSPARENT GROUND, BOTH WAYS ROUND (sec 16.23).
 *
 * The home-screen picture is rendered into an image filled transparent
 * and relies on the graph declining to paint its own ground; the
 * wallpaper is then what shows through. A graph that painted regardless
 * would produce a picture with a black slab in it, and the widget would
 * look wrong rather than fail, which is the kind of fault a screenshot
 * catches and a suite does not.
 *
 * Asserted in both directions on purpose. Only checking the transparent
 * case would pass against a graph that had stopped painting a ground at
 * all -- which is the same defect seen from the window's side, where it
 * shows as the previous frame smeared under the curve.
 */
void test_view::the_ground_is_painted_unless_it_is_turned_off() {
	bbq_forecast_graph graph;
	graph.resize(200, 100);

	/* A corner the plot does not reach, so what is read there is the
	 * ground and nothing drawn over it. */
	const QPoint corner(1, 98);

	QImage opaque(graph.size(), QImage::Format_ARGB32_Premultiplied);
	opaque.fill(Qt::transparent);
	graph.render(&opaque, QPoint(), QRegion(), QWidget::DrawChildren);

	QVERIFY(graph.opaque_background());
	QCOMPARE(qAlpha(opaque.pixel(corner)), 255);

	graph.set_opaque_background(false);

	QImage clear(graph.size(), QImage::Format_ARGB32_Premultiplied);
	clear.fill(Qt::transparent);
	graph.render(&clear, QPoint(), QRegion(), QWidget::DrawChildren);

	QCOMPARE(qAlpha(clear.pixel(corner)), 0);
}

/*
 * THE ARITHMETIC, AGAINST SOMETHING THIS TREE DID NOT COMPUTE.
 *
 * Black on white is 21:1 and a colour on itself is 1:1 -- both are in
 * WCAG 2.1 itself rather than being this code's own output, which is
 * what makes them worth asserting. A ratio that came from running the
 * function under test would be one witness twice, and a clamp built on
 * wrong arithmetic walks colours confidently to the wrong place.
 *
 * The third is the one that would catch a missing gamma step: #767676
 * on white is the canonical 4.54:1 boundary colour, and a version of
 * this using a plain weighted sum instead of the sRGB transfer function
 * answers about 3.0 for it.
 */
void test_view::the_contrast_ratio_agrees_with_a_published_pair() {
	QVERIFY(qAbs(bbq_contrast_ratio(Qt::black, Qt::white) - 21.0) < 0.01);
	QVERIFY(qAbs(bbq_contrast_ratio(Qt::white, Qt::white) - 1.0) < 0.01);

	const double grey = bbq_contrast_ratio(QColor(0x76, 0x76, 0x76),
	                                       Qt::white);
	QVERIFY2(qAbs(grey - 4.54) < 0.02,
	         qPrintable(QStringLiteral("#767676 on white is %1:1")
	                            .arg(grey)));
}

/*
 * Walked far enough and no further, with its hue intact.
 *
 * The distance matters as much as the direction: a clamp that jumped
 * straight to white would clear every floor and would have thrown away
 * the colour somebody chose, which is the failure this is between.
 */
void test_view::an_ink_is_walked_until_it_clears_the_floor() {
	const QColor ground(0x39, 0x3b, 0x3c);
	const QColor red(0xd5, 0x20, 0x2a);

	QVERIFY(bbq_contrast_ratio(red, ground) < 3.0);

	const QColor lifted = bbq_ensure_contrast(red, ground, 3.0);
	QVERIFY(bbq_contrast_ratio(lifted, ground) >= 3.0);

	/* Only just: one step back down must fail, or it walked too far. */
	QVERIFY(bbq_contrast_ratio(lifted, ground) < 3.3);

	/* Still red. Interpolating towards white would have desaturated it. */
	QVERIFY(lifted.hslSaturation() > 100);
	QCOMPARE(lifted.hslHue(), red.hslHue());

	/* Already clear means untouched, not walked to a rounder number. */
	QCOMPARE(bbq_ensure_contrast(Qt::white, Qt::black, 3.0), QColor(Qt::white));
}

/*
 * The graph's own palette, before and after naming a foreign ground.
 *
 * This is the assertion the widget actually depends on, and it is a
 * RELATIONSHIP rather than a value: the curve must clear the floor
 * against the scrim's worst case, whatever colour that takes. Pinning
 * the lifted red instead would go stale the first time Weather
 * Underground's measured colour was re-measured, and would say nothing
 * about whether the clamp had run.
 *
 * Furniture is asserted unmoved in the same breath. A clamp that lifted
 * the grid to a text floor would have made the graph worse while
 * passing every check aimed at legibility.
 */
void test_view::a_clamped_palette_lifts_the_curve_and_leaves_furniture() {
	bbq_forecast_graph graph;
	graph.set_theme(bbq_theme::dark);

	const bbq_graph_palette plain = graph.palette_colours();

	/*
	 * The dark scrim over a white wallpaper: the palest ground the
	 * widget's picture can land on, and so the one its inks must clear.
	 *
	 * Written out rather than computed here on purpose. Deriving it with
	 * the same expression the widget uses would be that expression
	 * agreeing with itself, which is corroboration from one witness --
	 * and the test does not depend on the exact value in any case. What
	 * it needs is a ground the unclamped palette demonstrably fails
	 * against, which the line below asserts before relying on it.
	 */
	const QColor worst(0x39, 0x3b, 0x3c);
	QVERIFY(bbq_contrast_ratio(plain.temperature, worst) < 3.0);

	graph.set_contrast_ground(worst, 3.0);
	const bbq_graph_palette clamped = graph.palette_colours();

	QVERIFY(bbq_contrast_ratio(clamped.temperature, worst) >= 3.0);
	QVERIFY(bbq_contrast_ratio(clamped.axis_text, worst) >= 3.0);
	QVERIFY(bbq_contrast_ratio(clamped.corrected, worst) >= 3.0);
	QVERIFY(bbq_contrast_ratio(clamped.stale_warning, worst) >= 3.0);

	QCOMPARE(clamped.grid, plain.grid);
	QCOMPARE(clamped.band_shade, plain.band_shade);
	QCOMPARE(clamped.background, plain.background);
}

/*
 * And the off position, which is what the window on this machine uses.
 *
 * Weather Underground's red is a measurement of their chart rather than
 * a decoration (sec 3.8.2), and the on-screen graph draws it exactly.
 * A clamp that leaked into the default would change a documented colour
 * everywhere while looking like a widget change.
 */
void test_view::naming_no_ground_leaves_every_colour_where_it_was() {
	bbq_forecast_graph graph;
	graph.set_theme(bbq_theme::dark);
	const bbq_graph_palette plain = graph.palette_colours();

	graph.set_contrast_ground(QColor(0x39, 0x3b, 0x3c), 3.0);
	QVERIFY(graph.palette_colours().temperature != plain.temperature);

	/* An invalid ground is the way back, and it must go all the way
	 * back rather than to wherever the last clamp left things. */
	graph.set_contrast_ground(QColor(), 3.0);
	QCOMPARE(graph.palette_colours().temperature, plain.temperature);
	QCOMPARE(graph.palette_colours().now_marker, plain.now_marker);
}

namespace {

/*
 * Weather that scores a grilling window: warm, dry and still, in UTC so
 * the policy's local-hour rule lands somewhere the test can predict.
 *
 * COLD FOR THE FIRST FIVE HOURS, and that is the load-bearing part. A
 * fixture warm from its first sample opens a window at the first
 * instant anybody asks about, so the window's start is the QUESTION'S
 * boundary rather than the weather's -- and a test asking whether the
 * start moves with the question then measures clamping and calls it
 * phase. The first draft did exactly that and reported a failure the
 * code did not have.
 */
bbq_composite grillable_days(qint64 from_utc, int days) {
	std::vector<bbq_sample> samples;

	for (int at = 0; at < days * 24; ++at) {
		bbq_sample sample;
		sample.start_utc = from_utc + at * 3600;
		sample.duration_s = 3600;
		sample.temperature = at < 5 ? 0.0 : 25.0;
		sample.precip_rate = 0.0;
		sample.precip_chance = 0.0;
		sample.wind_kph = 0.0;
		samples.push_back(sample);
	}

	bbq_series band(bbq_band::hourly, QStringLiteral("test"));
	band.set_zone(QTimeZone::UTC);
	band.set_samples(std::move(samples));

	bbq_composite composite;
	composite.set_series(std::move(band));
	return composite;
}

/*
 * How many pixels of the window-edge orange are in the shot.
 *
 * By colour and not by position, because the position is the thing
 * under test. #ff8b33 at full alpha is the edge rule; the wash is the
 * same hue at 22 and composites nowhere near it, the now-marker is
 * #ffd400 and the curve is #d5202a. The band is loose enough for the
 * antialiasing on a two-pixel line at a fractional column and tight
 * enough to admit none of those.
 */
int orange_pixels(const QImage &shot) {
	int seen = 0;

	for (int y = 0; y < shot.height(); ++y) {
		for (int x = 0; x < shot.width(); ++x) {
			const QColor at = shot.pixelColor(x, y);
			if (at.red() > 200 && at.green() > 100 && at.green() < 180 &&
			    at.blue() < 110) {
				++seen;
			}
		}
	}

	return seen;
}

} // namespace

/*
 * THE CONTROL, and it is not optional.
 *
 * The test below asserts an ABSENCE, and an absence proves nothing
 * until something has been seen to make the same detector speak. A
 * graph that had stopped drawing edge rules entirely -- or an
 * orange_pixels that matched nothing -- would pass it perfectly.
 */
void test_view::a_window_boundary_in_view_is_drawn() {
	bbq_forecast_graph graph;
	graph.set_theme(bbq_theme::dark);
	graph.set_composite(grillable_days(1600000000, 3));
	graph.resize(900, 400);

	const std::vector<bbq_window> windows = bbq_grill_windows(
	        graph.composite(), QTimeZone::UTC, 1600000000,
	        1600000000 + 3 * 86400, bbq_grill_policy());
	QVERIFY2(!windows.empty(), "the fixture scored no grilling window");

	/* A view wider than the first window, so both its ends are in it. */
	const bbq_window &first = windows.front();
	const qint64 span = (first.end_utc - first.start_utc) * 3;
	graph.set_view(first.start_utc - span / 3, span);

	QVERIFY2(orange_pixels(graph.grab().toImage()) > 0,
	         "no edge rule was drawn for a window wholly in view");
}

/*
 * AND THE CLIPPING (sec 16.30.1).
 *
 * A window running off the side of the view has been cut by the screen,
 * not by the weather. A rule drawn at the cut would say the window
 * starts where the plot does -- a claim about the forecast made by the
 * scroll position, which is the worst kind of wrong a chart can be.
 *
 * The view sits strictly inside the window, so BOTH ends are off-screen
 * and no rule belongs anywhere in the shot.
 */
void test_view::a_window_running_off_the_edge_draws_no_rule_there() {
	bbq_forecast_graph graph;
	graph.set_theme(bbq_theme::dark);
	graph.set_composite(grillable_days(1600000000, 3));
	graph.resize(900, 400);

	const std::vector<bbq_window> windows = bbq_grill_windows(
	        graph.composite(), QTimeZone::UTC, 1600000000,
	        1600000000 + 3 * 86400, bbq_grill_policy());
	QVERIFY(!windows.empty());

	const bbq_window &first = windows.front();
	const qint64 length = first.end_utc - first.start_utc;
	QVERIFY2(length > 3600, "the window is too short to look inside");

	/*
	 * Two ways for a boundary to be out of view, and only the second
	 * tests the guard.
	 *
	 * Far outside, the rule would be drawn at a coordinate thousands of
	 * pixels off the widget and the painter discards it -- so removing
	 * the guard entirely still passes. Measured: it does.
	 *
	 * Just outside is where the guard earns its place. A view starting
	 * a couple of minutes after the window does puts the boundary a few
	 * pixels left of the plot, which is IN THE GUTTER, on the widget,
	 * painted -- an orange rule beside the temperature axis, in the one
	 * place nothing else is drawn.
	 */
	const struct {
		const char *what;
		qint64 from;
		qint64 span;
	} views[] = {
		{ "the middle third, both ends far outside",
		  first.start_utc + length / 3, length / 3 },
		{ "starting two minutes in, the start just off the plot",
		  first.start_utc + 120, 3 * 3600 },
		{ "ending two minutes early, the end just off the plot",
		  first.end_utc - 3 * 3600, 3 * 3600 - 120 },
	};

	for (const auto &view : views) {
		graph.set_view(view.from, view.span);

		const int found = orange_pixels(graph.grab().toImage());
		QVERIFY2(found == 0,
		         qPrintable(QStringLiteral("%1 edge pixel(s) with %2")
		                            .arg(found)
		                            .arg(QString::fromLatin1(view.what))));
	}
}

/*
 * A WINDOW'S EDGES ARE THE WEATHER'S, NOT THE QUESTION'S (sec 16.30.2).
 *
 * Two callers ask this about overlapping ranges -- the header from now,
 * the plot over everything the composite covers -- and a scan that
 * began at whatever it was handed sampled different instants for each,
 * so the same afternoon could be reported ten minutes apart in two
 * places on one screen.
 *
 * Asked three ways here, none of them a multiple of the stride apart,
 * because a fixture whose offsets all happen to align proves the
 * property for the one case where it cannot fail.
 */
void test_view::a_window_lands_where_it_lands_whatever_range_was_asked() {
	const bbq_composite composite = grillable_days(1600000000, 3);
	const bbq_grill_policy policy;
	const qint64 last = 1600000000 + 3 * 86400;

	const auto first_window = [&](qint64 from) {
		const std::vector<bbq_window> found = bbq_grill_windows(
		        composite, QTimeZone::UTC, from, last, policy);
		QTest::qVerify(!found.empty(), "found a window", "", __FILE__,
		               __LINE__);
		return found.front();
	};

	const bbq_window whole = first_window(1600000000);

	/*
	 * The window must open on the weather rather than on the range, or
	 * this measures clamping. Five cold hours are in the fixture for
	 * exactly that, and this asserts they did their job.
	 */
	QVERIFY2(whole.start_utc > 1600000000 + 4 * 3600,
	         "the window opens at the range start, so nothing below is "
	         "about the scan grid");

	/* 137 and 431 seconds: prime-ish, and nowhere near 600. */
	const bbq_window shifted = first_window(1600000000 + 137);
	const bbq_window shifted_again = first_window(1600000000 + 431);

	QCOMPARE(shifted.start_utc, whole.start_utc);
	QCOMPARE(shifted.end_utc, whole.end_utc);
	QCOMPARE(shifted_again.start_utc, whole.start_utc);
	QCOMPARE(shifted_again.end_utc, whole.end_utc);
}

/*
 * THE HALO, ASSERTED AS THE PROPERTY IT EXISTS FOR (sec 16.32).
 *
 * Not "is a halo drawn" -- that is a mechanism, and a test on it would
 * pass a halo one pixel wide, the wrong colour, or on the wrong side.
 * What the curve needs is to clear the contrast floor against WHATEVER
 * IS BESIDE IT, and beside it is a ground this drawing invents: the
 * rain area, the chance wash, a grilling window, or any two at once.
 *
 * So the fixture is certain rain at a rate, which puts both washes over
 * the plot, and the assertion samples what actually surrounds the ink.
 * Reverted, the same measurement reads about 1.4:1.
 */
void test_view::the_curve_clears_the_floor_against_whatever_it_crosses() {
	std::vector<bbq_sample> samples;
	for (int at = 0; at < 24; ++at) {
		bbq_sample sample;
		sample.start_utc = 1600000000 + at * 3600;
		sample.duration_s = 3600;
		sample.temperature = 15.0 + (at % 6);
		sample.precip_chance = 100.0;
		sample.precip_rate = 8.0;
		samples.push_back(sample);
	}

	bbq_series band(bbq_band::hourly, QStringLiteral("test"));
	band.set_samples(std::move(samples));

	bbq_composite composite;
	composite.set_series(std::move(band));

	/*
	 * BOTH LAYOUTS, because a halo is measured in pixels and the two
	 * disagree about how many a line has. The desktop draws at 2.0 wide
	 * with 2.0 sample dots, the phone at 2.6 and 3.0 -- and this was
	 * written, passed and committed against the desktop alone, then
	 * failed on the first run at the phone's numbers (sec 16.32.5).
	 *
	 * That is this project's own recurring fault: a value calibrated
	 * against the one configuration that had ever varied. Iterating
	 * over the pair is the cheapest possible guard against repeating
	 * it.
	 */
	for (const bbq_layout shape : {bbq_layout::desktop, bbq_layout::mobile}) {
		bbq_forecast_graph graph;
		graph.set_theme(bbq_theme::dark);
		graph.set_layout(shape);
		graph.set_composite(composite);
		/*
		 * Samples ON, and it matters. The first draft turned them off to
		 * keep the measurement clean, and the sample dots turned out to be
		 * the only thing that failed: the curve is smoothed, so a knot can
		 * sit well away from the line, and two of them landed on the rain
		 * wash with nothing under them (sec 16.32.1). Excluding them would
		 * have measured the easy half.
		 */
		graph.set_show_samples(true);
		graph.resize(900, 400);
		graph.set_view(1600000000, 12 * 3600);

		const QImage shot = graph.grab().toImage();
		const QColor ink = graph.palette_colours().temperature;

		/*
		 * WALK OUT AND SEE WHICH ARRIVES FIRST: the plot's own ground, or
		 * the wash.
		 *
		 * That ordering IS the property. A fixed sample distance was tried
		 * twice and is the wrong instrument, for two reasons the profiles
		 * showed and no amount of care would have predicted: the ink is
		 * antialiased, so three rows below the last pure #d5202a are still
		 * dark red and read 1.56:1 against it; and a sloped column stretches
		 * both the ink and its halo vertically, so the right distance is
		 * different in every column.
		 *
		 * Walking is immune to both. It does not care how thick the halo is
		 * or how steep the line -- only that the ink is bordered by a colour
		 * this program chose rather than by one the weather painted.
		 */
		/*
		 * CONTRAST, not colour identity, and the difference took three
		 * drafts to find. A 1.5-pixel halo under an antialiased 2.6-pixel
		 * line does not produce a pixel equal to the ground: the profile on
		 * the tighter side reads #d5202a, #33191c, #182128, then the wash.
		 * Nothing there IS the background, and asking for one failed on
		 * eight of seventeen columns while the halo was working perfectly.
		 *
		 * What the curve needs is not a pixel of a particular colour beside
		 * it. It is a border it can be read against -- so the question is
		 * whether contrast reaches the floor before the wash arrives.
		 */
		const auto legible_against = [&](const QColor &at) {
			return bbq_contrast_ratio(ink, at) >= 3.0;
		};

		/*
		 * Unambiguously the wash rather than a blend on the way to it. The
		 * washes are blue where the ground is neutral, so the test is
		 * "clearly bluer than the ink" -- which names no wash's colour and
		 * so does not go stale when an alpha moves.
		 */
		const auto is_wash = [&](const QColor &at) {
			return at.blue() - at.red() > 25;
		};

		/*
		 * The ink or its antialiasing, by hue rather than by equality.
		 *
		 * Finding the ink's extent with `== ink` finds only the pixels that
		 * came out exactly #d5202a, and on a steep segment that can be a
		 * single pixel with the real run twenty deep around it. The
		 * steepness filter below then reads span=1, decides the column is
		 * shallow, and walks ALONG the line -- which is the one direction
		 * the halo does not cover, and it blamed the drawing.
		 *
		 * Red-dominance separates the ink and every blend of it from both
		 * the ground and the washes, which are neutral and blue.
		 */
		const auto ink_like = [](const QColor &at) {
			return at.red() - at.green() > 20;
		};

		int checked = 0;
		int bare = 0;
		int bare_x = 0;

		/*
		 * ACROSS THE INK, WHICH IS NOT ALWAYS DOWN THE COLUMN.
		 *
		 * A halo is a border, so it is measured across the line -- and a
		 * near-vertical segment has no across in a column. Walking up from
		 * the top of one travels ALONG the line and out past its end, where
		 * a flat cap leaves no halo at all: measured at the phone's line
		 * width, x=450 spans thirty rows and reads #36191d then the wash,
		 * and the reading is 2.94:1 for a drawing that is perfectly correct
		 * to its left and right.
		 *
		 * So each axis measures only where it is the across: columns where
		 * the ink is shallow, rows where it is narrow. Together they cover
		 * the whole curve, and neither is asked a question it cannot
		 * answer.
		 */
		const auto sweep = [&](bool by_column) {
			const int outer = by_column ? shot.width() : shot.height();
			const int inner = by_column ? shot.height() : shot.width();

			/*
			 * EVERY line, not every twenty-fifth. A sample dot is about
			 * seven pixels across, so a coarse step hits one only by luck
			 * -- and the dots turned out to be the part that failed
			 * (sec 16.32.1). Stepping by 25 the dot-ring sabotage went
			 * undetected; stepping by one it does not.
			 */
			for (int at = 20; at < outer - 20; ++at) {
				int first = -1;
				int last = -1;


				for (int step = 1; step < inner - 1; ++step) {
					const QColor px = by_column ? shot.pixelColor(at, step)
					                            : shot.pixelColor(step, at);
					if (px != ink) {
						continue;
					}
					if (first < 0) {
						first = step;
					}
					last = step;
				}

				if (first < 0) {
					continue;
				}

				const auto pixel = [&](int step) {
					return by_column ? shot.pixelColor(at, step)
					                 : shot.pixelColor(step, at);
				};

				/*
				 * A coarse filter only. The exact-match extent understates
				 * a steep run badly -- a near-vertical segment can show one
				 * pixel of exactly #d5202a with twenty of blend around it
				 * -- so the real check for "running along this axis" is in
				 * the walk below, which notices that it is still in ink
				 * after four steps.
				 */
				if (last - first > 4) {
					continue;
				}

				if (first < 12 || last + 12 >= inner) {
					continue;
				}

				const std::pair<int, int> walks[] = {{first, -1}, {last, 1}};

				for (const std::pair<int, int> &walk : walks) {
					bool along = true;
					bool decided = false;

					for (int step = 1; step <= 10 && !decided; ++step) {
						const QColor px =
						        pixel(walk.first + walk.second * step);

						/*
						 * FOUR STEPS STILL IN INK MEANS THIS IS THE LINE'S
						 * OWN DIRECTION, not its border. Walking that way
						 * leaves the segment past its flat cap, where there
						 * is no halo by construction -- and the reading
						 * blames a drawing that is correct to either side.
						 */
						if (ink_like(px)) {
							if (step >= 4) {
								break;
							}
							continue;
						}

						along = false;

						if (legible_against(px)) {
							decided = true;
						} else if (is_wash(px)) {
							++bare;
							bare_x = at;
							decided = true;
						}
					}

					if (!along) {
						++checked;
					}
				}
			}
		};

		sweep(true);
		sweep(false);

		QVERIFY2(checked >= 16,
		         qPrintable(QStringLiteral("only %1 side(s) had the curve in "
		                                   "them, so this measured almost "
		                                   "nothing")
		                            .arg(checked)));

		QVERIFY2(bare == 0,
		         qPrintable(QStringLiteral("%1 of %2 side(s) put the curve "
		                                   "straight onto the wash with no "
		                                   "ground of its own, at x=%3, in "
		                                   "the %4 layout")
		                            .arg(bare)
		                            .arg(checked)
		                            .arg(bare_x)
		                            .arg(shape == bbq_layout::mobile
		                                         ? QStringLiteral("mobile")
		                                         : QStringLiteral("desktop"))));
	}
}

/*
 * The readout box, where it cannot fit.
 *
 * The shrinking loop drops fields while there are more than two, so a
 * plot narrower than the time and the temperature together leaves a box
 * wider than the space -- and then no position satisfies both edges.
 * Which edge loses is decided by the order the clamps run in, and it is
 * invisible from a desktop window, where the box always fits.
 *
 * ASSERT THE RELATIONSHIP: the box must start inside the plot in every
 * case, including the one where it cannot end inside it. Asserting the
 * fitting cases alone passes against either order.
 */
void test_view::a_readout_too_wide_to_fit_keeps_its_left_edge() {
	const double left = 40.0;
	const double right = 340.0;

	struct {
		const char *what;
		double centre;
		double box_w;
	} const cases[] = {
		{"room on both sides", 200.0, 120.0},
		{"hard against the right", 338.0, 120.0},
		{"hard against the left", 41.0, 120.0},
		{"exactly as wide as the plot", 200.0, 300.0},
		{"wider than the plot", 200.0, 400.0},
		{"far wider, cursor at the left", 45.0, 900.0},
	};

	for (const auto &c : cases) {
		const double x = bbq_readout_box_x(c.centre, c.box_w, left, right);

		QVERIFY2(x >= left,
		         qPrintable(QStringLiteral("%1: box starts at %2, left of "
		                                   "the plot at %3 -- the time and "
		                                   "the temperature are off the edge")
		                            .arg(QString::fromLatin1(c.what))
		                            .arg(x)
		                            .arg(left)));
	}

	/* And where it DOES fit, it is still centred and still inside. */
	const double fits = bbq_readout_box_x(200.0, 120.0, left, right);
	QCOMPARE(fits, 140.0);
	QVERIFY(fits + 120.0 <= right);
}

/*
 * The readout's time, where the column holds more than one sample.
 *
 * The value drawn is the mean over the column -- sec 3.7 requires the
 * trace and the readout to be replaced together -- so a single sample's
 * timestamp beside it claims a precision nobody measured. Sec 13.2
 * already refuses that claim for the sample marks; this is the same
 * refusal in text.
 *
 * The single-sample case is asserted too, because it is the one that
 * must NOT change: deriving the time from the cursor put 05:59 in the
 * readout for a sample stamped 06:00.
 */
void test_view::a_column_holding_several_samples_reads_as_a_range() {
	const QTimeZone utc = QTimeZone::utc();
	const qint64 noon = QDateTime(QDate(2026, 9, 7), QTime(12, 0), utc)
	                            .toSecsSinceEpoch();

	/*
	 * ASSERT THE RELATIONSHIP FIRST, because it is what survives a
	 * change of format -- and because QCOMPARE aborts the function, so
	 * whichever assertion runs first is the only one a sabotage run
	 * proves. Pinning the exact spellings below is worth having and is
	 * not what this test is for.
	 */
	const struct {
		int count;
		qint64 last;
		bool wide;
	} several[] = {
		{2, noon + 300, false},
		{2, noon + 300, true},
		{24, noon + 86400, false},
		{260, noon + 11 * 86400, true},
	};

	for (const auto &c : several) {
		const QString many =
		        bbq_readout_time_label(noon, c.last, c.count, c.wide, utc);
		const QString one =
		        bbq_readout_time_label(noon, noon, 1, c.wide, utc);

		QVERIFY2(many != one,
		         qPrintable(QStringLiteral("%1 sample(s) spanning %2 s read "
		                                   "as \"%3\", the same as a single "
		                                   "sample -- the mean is wearing one "
		                                   "sample's timestamp")
		                            .arg(c.count)
		                            .arg(c.last - noon)
		                            .arg(many)));
	}

	/* One sample: its own stamp, both spellings, unchanged. */
	QCOMPARE(bbq_readout_time_label(noon, noon, 1, false, utc),
	         QStringLiteral("12:00"));
	QCOMPARE(bbq_readout_time_label(noon, noon, 1, true, utc),
	         QStringLiteral("Mon 12:00"));

	/* Several within the day: a range, and it still starts where it did. */
	const QString within =
	        bbq_readout_time_label(noon, noon + 6 * 3600, 7, false, utc);
	QCOMPARE(within, QStringLiteral("12:00-18:00"));

	/* Across days the weekday is not enough, so both dates are named. */
	const QString across =
	        bbq_readout_time_label(noon, noon + 11 * 86400, 260, true, utc);
	QCOMPARE(across, QStringLiteral("7 Sep 12:00-18 Sep 12:00"));
	QCOMPARE(bbq_readout_time_label(noon, noon + 11 * 86400, 260, false, utc),
	         QStringLiteral("7 Sep-18 Sep"));

}

/*
 * The day dividers and their names, at a span nobody designed for.
 *
 * Sec 13.2 stops the sample marks once they would merge. The day
 * furniture had no such rule, and the span became the reader's to
 * choose: at a year it is 365 dividers across the plot, which draws it
 * as a barcode and the names as a smear -- structure claiming a
 * regularity that belongs to the calendar rather than to the data.
 *
 * ASKED OF THE FUNCTION, not of the pixels, and the first attempt here
 * was the other way. A divider is a one-pixel line at a fractional x,
 * so nearly all its ink is an antialiased blend and a colour-match
 * counter found none of it even at a three-day span -- the control
 * caught that, which is what a control is for. Loosening the tolerance
 * until it passed would have been fitting the instrument to the
 * answer. The end-to-end half is a screenshot at each span, which is
 * how this project checks drawing anyway.
 */
void test_view::day_furniture_goes_away_once_the_days_would_crowd() {
	const double name = 40.0;

	/* Fewer than two midnights: nothing to crowd against. */
	QVERIFY(bbq_day_furniture_fits(-1.0, name));

	/*
	 * The relationship, over spans that actually occur. A day is
	 * 86400 s, so the gap in pixels is 86400 / (span / plot width).
	 */
	const struct {
		const char *what;
		double span_s;
		double plot_px;
		bool wanted;
	} cases[] = {
		{"a day, on a desktop", 86400, 860, true},
		{"a day, on the Fold's cover screen", 86400, 300, true},
		{"the sixteen days of data, desktop", 16 * 86400.0, 860, true},
		{"a year, desktop", 365 * 86400.0, 860, false},
		{"a year, cover screen", 365 * 86400.0, 300, false},
		{"the ten-year ceiling", 3650 * 86400.0, 860, false},
	};

	for (const auto &c : cases) {
		const double gap = 86400.0 / (c.span_s / c.plot_px);
		const bool fits = bbq_day_furniture_fits(gap, name);

		QVERIFY2(fits == c.wanted,
		         qPrintable(QStringLiteral("%1: midnights %2 px apart with a "
		                                   "%3 px name -- drawn=%4, wanted=%5")
		                            .arg(QString::fromLatin1(c.what))
		                            .arg(gap, 0, 'f', 1)
		                            .arg(name)
		                            .arg(fits)
		                            .arg(c.wanted)));
	}

	/*
	 * And it is the NAME that decides, not a constant: the same
	 * spacing flips as the label grows, which is what makes the rule
	 * follow the font instead of assuming one.
	 */
	QVERIFY(bbq_day_furniture_fits(50.0, 40.0));
	QVERIFY(!bbq_day_furniture_fits(50.0, 60.0));
}

/*
 * The grill windows, cached against the composite.
 *
 * The scan ran inside paintEvent, so a drag paid for it on every mouse
 * move -- 2.2 ms for three days of span, 18.8 ms for thirty, 195 ms for
 * a year, measured over this fixture. It is linear in the composite's
 * span, and the composite grows as the archive fills, which is what
 * this program is for.
 *
 * ASSERT BOTH HALVES. That the cache agrees with a fresh scan is the
 * easy one and a cache that never invalidates passes it; the half that
 * catches that is the second composite, where the answer has to have
 * changed.
 */
void test_view::the_window_scan_is_kept_and_forgotten_with_the_composite() {
	const auto fresh = [](const bbq_composite &c) {
		return bbq_grill_windows(c, c.zone(), c.begin_utc(), c.end_utc(),
		                         bbq_grill_policy());
	};

	bbq_forecast_graph graph;

	const bbq_composite first = grillable_days(1600000000, 3);
	graph.set_composite(first);

	const std::vector<bbq_window> wanted_first = fresh(first);
	QVERIFY2(!wanted_first.empty(), "the fixture produced no windows, so "
	                                "neither half of this can discriminate");
	QCOMPARE(graph.grill_windows().size(), wanted_first.size());

	for (std::size_t i = 0; i < wanted_first.size(); ++i) {
		QCOMPARE(graph.grill_windows()[i].start_utc, wanted_first[i].start_utc);
		QCOMPARE(graph.grill_windows()[i].end_utc, wanted_first[i].end_utc);
	}

	/*
	 * A different composite, far enough away that no window of the
	 * first could be mistaken for one of the second.
	 */
	const bbq_composite second = grillable_days(1600000000 + 40 * 86400, 5);
	const std::vector<bbq_window> wanted_second = fresh(second);
	QVERIFY(wanted_second.size() != wanted_first.size());

	graph.set_composite(second);

	QVERIFY2(graph.grill_windows().size() == wanted_second.size(),
	         qPrintable(QStringLiteral("after replacing the composite the "
	                                   "graph reports %1 window(s), the new "
	                                   "data has %2 and the old had %3 -- the "
	                                   "scan was not forgotten")
	                            .arg(graph.grill_windows().size())
	                            .arg(wanted_second.size())
	                            .arg(wanted_first.size())));

	QCOMPARE(graph.grill_windows().front().start_utc,
	         wanted_second.front().start_utc);
}

/*
 * The graph swaps a translucent fill for an opaque one over a known
 * ground and calls the result unchanged. That is the claim tested here,
 * and it is deliberately not "does bbq_flatten_over reproduce Qt's
 * rounding" -- that function asks Qt for the blend, so such a test would
 * be Qt agreeing with itself, which sec 16.89 records as the reason this
 * one is shaped the way it is.
 *
 * What is checked instead is the substitution: fill an area with the
 * ground and blend the translucent colour over it, fill a second area
 * with the ground and paint the flattened colour opaquely, and require
 * the two IMAGES to be identical. That would fail if the blend were not
 * uniform across the area, if it depended on position, or if an opaque
 * fill took a path that landed anywhere else -- none of which the
 * one-pixel probe inside bbq_flatten_over can see on its own.
 *
 * Every alpha rather than a few, because rounding is where this breaks
 * and it will not break at 0, 128 and 255.
 */
void test_view::bbq_flatten_matches_qt() {
	const QList<QColor> grounds = {
		QColor(0, 0, 0),
		QColor(255, 255, 255),
		QColor(18, 20, 24),
		QColor(240, 238, 233),
		QColor(90, 30, 130),
		QColor(7, 199, 61),
	};
	const QList<QColor> inks = {
		QColor(255, 160, 60),
		QColor(0, 0, 0),
		QColor(255, 255, 255),
		QColor(45, 111, 181),
		QColor(3, 250, 128),
	};

	/*
	 * Narrow and tall, which is the shape the window shades are and the
	 * shape the whole change is about.
	 */
	const QRect area(1, 0, 5, 40);

	int checked = 0;

	for (const QColor &ground : grounds) {
		for (const QColor &ink : inks) {
			for (int alpha = 0; alpha <= 255; ++alpha) {
				QColor src = ink;
				src.setAlpha(alpha);

				QImage blended(8, 40, QImage::Format_ARGB32_Premultiplied);
				blended.fill(ground);
				{
					QPainter painter(&blended);
					painter.fillRect(area, src);
				}

				QImage flattened(8, 40, QImage::Format_ARGB32_Premultiplied);
				flattened.fill(ground);
				{
					QPainter painter(&flattened);
					painter.fillRect(area, bbq_flatten_over(src, ground));
				}

				if (blended != flattened) {
					QFAIL(qPrintable(
					        QStringLiteral("ground %1 ink %2 alpha %3: blended "
					                       "%4, flattened %5")
					                .arg(ground.name(), ink.name())
					                .arg(alpha)
					                .arg(blended.pixelColor(3, 20).name(),
					                     flattened.pixelColor(3, 20).name())));
				}
				++checked;
			}
		}
	}

	/*
	 * The sweep is evidence only if it ran over what it says it did.
	 */
	QCOMPARE(checked, int(grounds.size() * inks.size()) * 256);
}

namespace {

/*
 * The largest distance from any input point to the polyline that
 * survived. This is the quantity Douglas-Peucker bounds, so it is the
 * quantity to assert -- not the point count, which is a property of the
 * data, and not the output itself, which would be this file agreeing
 * with the implementation about a particular answer.
 */
double worst_departure(const QPolygonF &before, const QPolygonF &after) {
	double worst = 0.0;

	for (const QPointF &p : before) {
		double best = std::numeric_limits<double>::max();

		for (int at = 0; at + 1 < after.size(); ++at) {
			const QPointF a = after[at];
			const QPointF b = after[at + 1];
			const double dx = b.x() - a.x();
			const double dy = b.y() - a.y();
			const double len_squared = dx * dx + dy * dy;

			double t = 0.0;
			if (len_squared > 1e-18) {
				t = ((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) /
				    len_squared;
				t = std::clamp(t, 0.0, 1.0);
			}

			best = std::min(best, std::hypot(p.x() - (a.x() + t * dx),
			                                 p.y() - (a.y() + t * dy)));
		}

		worst = std::max(worst, best);
	}

	return worst;
}

} // namespace

/*
 * The guarantee the drawing relies on: nothing moves further than the
 * tolerance. The graph simplifies the temperature curve before stroking
 * it and calls the picture unchanged, which is only honest if this
 * holds -- so it is checked against the SEGMENTS that survive rather
 * than against the vertices, since a dropped point is judged by how far
 * it sits from the line now drawn in its place.
 *
 * The inputs are chosen to be hostile rather than representative. A
 * smooth arc is the easy case and the one the optimisation was measured
 * on; a sawtooth, a step and a spike are what would expose a simplifier
 * that walks past accumulated drift -- which the first attempt at this
 * did, reducing a 429-point arc to 7.
 */
void test_view::bbq_simplify_keeps_every_point_within_tolerance() {
	const QList<double> tolerances = {0.001, 0.02, 0.1, 1.0, 5.0};

	QList<QPolygonF> shapes;

	QPolygonF arc;
	QPolygonF sawtooth;
	QPolygonF spike;
	QPolygonF flat;
	QPolygonF staircase;

	for (int at = 0; at < 400; ++at) {
		const double x = at;
		arc << QPointF(x, 200.0 + 150.0 * std::sin(at / 61.0));
		sawtooth << QPointF(x, at % 7 < 4 ? 100.0 : 140.0);
		spike << QPointF(x, at == 200 ? 10.0 : 300.0);
		flat << QPointF(x, 123.0);
		staircase << QPointF(x, 50.0 + 10.0 * (at / 40));
	}

	shapes << arc << sawtooth << spike << flat << staircase;

	int checked = 0;

	for (const QPolygonF &shape : shapes) {
		for (double tolerance : tolerances) {
			const QPolygonF kept = bbq_simplify_polyline(shape, tolerance);

			QVERIFY2(kept.size() >= 2, "a polyline must keep its ends");
			QCOMPARE(kept.first(), shape.first());
			QCOMPARE(kept.last(), shape.last());
			QVERIFY2(kept.size() <= shape.size(),
			         "simplifying cannot add vertices");

			const double worst = worst_departure(shape, kept);
			if (worst > tolerance + 1e-9) {
				QFAIL(qPrintable(
				        QStringLiteral("tolerance %1: a point moved %2")
				                .arg(tolerance)
				                .arg(worst)));
			}

			++checked;
		}
	}

	QCOMPARE(checked, int(shapes.size() * tolerances.size()));
}

/*
 * The bound above is satisfied by a simplifier that removes NOTHING, so
 * on its own it cannot tell a working one from an inert one -- the
 * vacuous pass of the guidelines, wearing a proof.
 *
 * These are the two ends. A straight line has no vertex worth keeping
 * whatever the tolerance, and a spike is exactly what a tolerance below
 * its height must not lose. A simplifier that fails either is broken in
 * a way the distance bound is blind to.
 */
void test_view::bbq_simplify_keeps_what_a_curve_needs() {
	QPolygonF straight;
	for (int at = 0; at < 300; ++at) {
		straight << QPointF(at, 7.0 + 0.25 * at);
	}
	QCOMPARE(bbq_simplify_polyline(straight, 0.02).size(), 2);

	QPolygonF spike;
	for (int at = 0; at < 300; ++at) {
		spike << QPointF(at, at == 150 ? 0.0 : 200.0);
	}
	const QPolygonF kept = bbq_simplify_polyline(spike, 0.02);
	QVERIFY2(kept.contains(QPointF(150, 0.0)),
	         "a spike far outside the tolerance was dropped");

	/* And a tolerance wider than the spike is allowed to lose it. */
	QCOMPARE(bbq_simplify_polyline(spike, 400.0).size(), 2);

	/* Degenerate inputs must not divide by a zero-length segment. */
	QPolygonF same;
	same << QPointF(5.0, 5.0) << QPointF(5.0, 5.0) << QPointF(5.0, 5.0);
	QCOMPARE(bbq_simplify_polyline(same, 0.02).size(), 2);
}

/*
 * The sample dots are stamped from a cache of pixmaps built from the
 * palette (project.md sec 16.92), so a theme change has to throw that
 * cache away. Nothing else in the picture would show it if it did not:
 * the curve, the grid and the ground would all turn light while the
 * dots stayed dark, and the dots are small.
 *
 * Checked by colour rather than by asking the widget what it cached.
 * The dark ground is what a stale ring would be drawn in, and after a
 * switch to light there should be none of it anywhere on the plot --
 * which is a claim about the PICTURE, and so survives the cache being
 * reorganised.
 */
void test_view::the_sample_dots_follow_a_theme_change() {
	bbq_forecast_graph graph;
	graph.set_composite(grillable_days(1600000000, 2));
	graph.resize(600, 400);
	graph.set_view(1600000000, 2 * 86400LL);

	graph.set_theme(bbq_theme::dark);
	const QColor dark_ground = graph.palette_colours().background;
	const QImage dark_shot = graph.grab().toImage();

	/*
	 * The control: the dark render must actually CONTAIN the colour
	 * being searched for, or the assertion below passes by finding
	 * nothing in a picture that never had any.
	 */
	int dark_ground_pixels = 0;
	for (int y = 0; y < dark_shot.height(); ++y) {
		for (int x = 0; x < dark_shot.width(); ++x) {
			if (dark_shot.pixelColor(x, y) == dark_ground) {
				++dark_ground_pixels;
			}
		}
	}
	QVERIFY2(dark_ground_pixels > 1000,
	         "the dark render does not contain its own ground colour, so "
	         "the search below would prove nothing");

	graph.set_theme(bbq_theme::light);
	const QImage light_shot = graph.grab().toImage();
	QVERIFY(graph.palette_colours().background != dark_ground);

	int stale = 0;
	for (int y = 0; y < light_shot.height(); ++y) {
		for (int x = 0; x < light_shot.width(); ++x) {
			if (light_shot.pixelColor(x, y) == dark_ground) {
				++stale;
			}
		}
	}

	if (stale != 0) {
		QFAIL(qPrintable(
		        QStringLiteral("%1 pixel(s) of the dark ground survived a "
		                       "switch to light -- the dot stamps were not "
		                       "rebuilt")
		                .arg(stale)));
	}
}

/*
 * Every stamp must carry the device pixel ratio it was rendered at, and
 * be sized in device pixels to match (project.md sec 16.99).
 *
 * A QPixmap defaults to a ratio of 1 and is then drawn at its PIXEL size
 * whatever surface it lands on -- so on a HiDPI screen the sample dots
 * came out about half the size they should be. That shipped in sec
 * 16.92 and no test could see it, because every offscreen render this
 * project makes runs at a ratio of 1.
 *
 * Asserted on the stamps rather than on a render, after three attempts
 * to infer it from one measured something else each time: the difference
 * between a marked and unmarked render contains the curve as well, and
 * the dot's fill is drawn in the curve's own colour, so pixels where
 * they coincide are invisible to it. The property is a property of the
 * pixmap, so the test holds the pixmap.
 */
void test_view::a_dot_stamp_carries_the_ratio_it_was_rendered_at() {
	const QColor ring(20, 22, 26);
	const QColor fill(213, 32, 42);
	const double radius = 3.0;

	int checked = 0;

	for (double ratio : {1.0, 1.5, 2.0, 3.0}) {
		const std::vector<QPixmap> stamps =
		        bbq_dot_stamps(ring, fill, radius, ratio);

		QVERIFY2(!stamps.empty(), "no stamps were rendered");

		const QSize logical = stamps.front().deviceIndependentSize().toSize();

		for (const QPixmap &stamp : stamps) {
			QCOMPARE(stamp.devicePixelRatio(), ratio);

			/*
			 * The claim that matters: the same LOGICAL size at every
			 * ratio, with the pixels to back it. A stamp left at ratio
			 * 1 has the right pixel count and the wrong logical size,
			 * which is exactly the defect.
			 */
			QCOMPARE(stamp.deviceIndependentSize().toSize(), logical);
			QCOMPARE(stamp.width(), int(std::lround(logical.width() * ratio)));

			QVERIFY2(!stamp.isNull(), "a stamp came back null");
			++checked;
		}
	}

	QCOMPARE(checked, 4 * 8);

	/*
	 * The control: the logical size must be the same across ratios, so
	 * compare one against another rather than each against itself. Two
	 * runs agreeing that a stamp equals its own size would pass however
	 * wrong both were.
	 */
	const QSize at_one =
	        bbq_dot_stamps(ring, fill, radius, 1.0).front()
	                .deviceIndependentSize().toSize();
	const QSize at_three =
	        bbq_dot_stamps(ring, fill, radius, 3.0).front()
	                .deviceIndependentSize().toSize();
	QCOMPARE(at_three, at_one);

	/* And the pixels really are there, rather than the ratio alone. */
	QCOMPARE(bbq_dot_stamps(ring, fill, radius, 3.0).front().width(),
	         at_one.width() * 3);
}
