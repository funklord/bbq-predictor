#include <QApplication>
#include <QTest>
#include <QWheelEvent>

#include "graph/forecast_graph.h"

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

/*
 * Its own main, so the platform is chosen here rather than depending on
 * whatever ran it. Widgets need a QApplication; offscreen needs no
 * display.
 */
int main(int argc, char *argv[]) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);
	test_view suite;
	return QTest::qExec(&suite, argc, argv);
}

#include "test_view.moc"
