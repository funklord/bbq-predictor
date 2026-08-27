#include <QApplication>
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
#include "model/composite.h"
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
	void a_fresh_graph_follows_the_clock();
	void the_span_is_bounded_at_both_ends();
	void zooming_holds_the_moment_under_the_cursor();
	void dragging_moves_time_with_the_hand();
	void double_click_comes_back_to_now();
	void a_theme_setting_lands_somewhere_defined();
	void automatic_never_answers_unknown();
	void automatic_releases_the_override();
	void every_day_boundary_is_local_midnight();
	void the_short_night_is_twenty_three_hours();
	void the_long_night_is_twenty_five_hours();
	void a_boundary_on_the_left_edge_is_kept();
	void the_count_is_bounded();
	void a_slider_reports_no_value_to_accessibility();
	void the_temperature_line_survives_a_certain_downpour();

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

int main(int argc, char *argv[]) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);
	test_view suite;
	return QTest::qExec(&suite, argc, argv);
}

#include "test_view.moc"
