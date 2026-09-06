#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QLayout>
#include <QMouseEvent>
#include <QNetworkProxy>
#include <QStandardPaths>
#include <QLabel>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include "model/settings.h"
#include "graph/forecast_graph.h"
#include "ui/theme.h"
#include "ui/widget_picture.h"
#include "model/composite.h"
#include "store/history.h"
#include "ui/layout.h"
#include "ui/flow_layout.h"
#include "ui/main_window.h"
#include "ui/tray_icon.h"
#include "wu/feed.h"

/*
 * The window's wiring (project.md sec 14.10).
 *
 * This binary exists because every defect found in this layer was in
 * the connections rather than in a function: a fix to the feed that the
 * graph never saw, an error message that outlived the station it was
 * about, a display label written to the configuration as a station id.
 * Each was found by hand, on a phone, and each was invisible to a suite
 * that stopped at the feed.
 *
 * TWO GUARDS, both of them load-bearing.
 *
 * The window reads and WRITES real configuration -- watch_station calls
 * bbq_settings::set_station -- so the config and data locations are
 * redirected into a temporary directory before QApplication is built,
 * which is when Qt resolves and caches them. Without it a test run
 * would rewrite the station somebody is watching.
 *
 * `QStandardPaths::setTestModeEnabled` was used first and removed. It
 * protects the real file, and it OVERRIDES the environment -- so the
 * two together are not belt and braces: test mode wins, and the run
 * leaves a settings file in $HOME on a machine whose owner did not ask
 * for one. The assertion below caught that within a minute of being
 * written, which is the argument for stating a guard as what it must
 * not do rather than as what it should.
 *
 * And watch_station refreshes, which fetches. An application-wide proxy
 * pointing at a closed port on the loopback interface means a request
 * that escapes cannot leave the machine: this project scrapes a key it
 * is not licensed to have, and a test suite firing at a third party on
 * every run would be wrong whatever it was measuring.
 */
class test_window : public QObject {
	Q_OBJECT

private slots:
	void initTestCase();
	void a_label_is_not_stored_as_a_station_id();

	/*
	 * The scrim's bound, which the widget's whole clamp rests on
	 * (project.md sec 16.26).
	 */
	void no_wallpaper_can_get_between_the_scrim_and_an_ink();
	void a_scrim_light_enough_to_pass_an_ink_is_reported_unbounded();
	void the_clamp_nudges_a_colour_rather_than_redesigning_it();
	void the_widget_render_leaves_the_parked_readout_where_it_was();
	void the_picture_is_posed_at_now_with_no_cursor();
	void a_following_window_is_still_following_after_a_render();
	void the_server_default_tries_a_local_host_before_a_remote_one();
	void the_tooltip_lists_every_window_the_count_promises();
	void refreshing_the_status_actually_hands_the_label_that_list();
	void a_window_that_dips_says_so_and_a_steady_one_does_not();
	void changing_station_clears_the_old_curves();
	void changing_station_clears_the_old_error();
	void pinning_marks_the_station_in_the_store();
	void a_warm_band_and_a_cold_one_read_differently();
	void the_record_line_reports_the_verdict_too();
	void the_list_names_the_station_actually_being_read();
	void the_view_still_pans_and_zooms_through_the_window();
	void turning_and_unfolding_the_device_keeps_what_was_on_screen();
	void a_fix_from_where_discovery_already_ran_is_not_sent_to_it();
	void a_wrapping_row_asks_its_height_at_its_own_preferred_width();
	void an_unscored_lead_does_not_claim_the_archive_is_empty();
	void the_tray_says_how_old_its_reading_is();
	void the_tray_reads_the_owner_of_now_not_the_finest_band();
	void a_dry_spell_is_not_a_rain_forecast_with_no_skill();

private:
	static bbq_series bandful(bbq_band band, qint64 start, int count);
};

void test_window::initTestCase() {
	QNetworkProxy blocked(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"),
	                      1);
	QNetworkProxy::setApplicationProxy(blocked);

	/*
	 * Proof that the redirection took, stated as what it must NOT be.
	 *
	 * The first version asked whether the path contained "test", which
	 * is the weaker question: a path can satisfy it and still sit in
	 * $HOME, and this one did. A test that silently wrote to the real
	 * configuration would pass exactly as loudly as one that did not.
	 */
	const QString where = QStandardPaths::writableLocation(
	        QStandardPaths::AppConfigLocation);
	const QString home = QDir::homePath();

	QVERIFY2(!where.startsWith(home),
	         qPrintable(QStringLiteral("config still lands in $HOME: %1")
	                            .arg(where)));
}

bbq_series test_window::bandful(bbq_band band, qint64 start, int count) {
	std::vector<bbq_sample> samples;
	for (int i = 0; i < count; ++i) {
		bbq_sample sample;
		sample.start_utc = start + i * 3600;
		sample.duration_s = 3600;
		sample.temperature = 15.0;
		samples.push_back(sample);
	}

	bbq_series made(band, QStringLiteral("test"));
	made.set_samples(std::move(samples));
	return made;
}

void test_window::a_label_is_not_stored_as_a_station_id() {
	/*
	 * The list shows "ISTOCK877  4.0 km" because the distance is what
	 * makes one of ten choosable, and the box is editable -- so
	 * committing the field hands back the LABEL. It was written to the
	 * configuration as the station, and the next fetch asked Weather
	 * Underground for a station with a space and a distance in its name
	 * (sec 14.2.1). Measured on the device before it was fixed.
	 */
	QTemporaryDir directory;
	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	bbq_station near;
	near.id = QStringLiteral("ITEST877");
	near.distance_km = 4.0;
	QVERIFY(window.feed()->history().remember_station(near));

	window.refresh_station_list();

	const int listed = window.m_station_box->findData(near.id);
	QVERIFY2(listed >= 0, "the remembered station never reached the list");

	const QString label = window.m_station_box->itemText(listed);
	QVERIFY2(label != near.id,
	         "the list is showing bare ids, so this test cannot fail");

	window.watch_station(label);

	QCOMPARE(bbq_settings::station(), near.id);
}

void test_window::changing_station_clears_the_old_curves() {
	/*
	 * THE INERT FIX (sec 14.8.3).
	 *
	 * Dropping the old station's bands from the feed's composite is
	 * invisible on its own: the graph holds a COPY, taken by value, and
	 * learns of a change only when a fetch lands or the view moves.
	 * After a station change neither is guaranteed, and where the fetch
	 * fails -- the case the drop exists for -- neither ever comes. The
	 * feed was correct and the screen went on drawing the previous
	 * station.
	 *
	 * Asserted on the GRAPH's composite rather than the feed's, which
	 * is the whole point: the feed's is what the earlier test already
	 * covers, and it passed while this was broken.
	 */
	QTemporaryDir directory;
	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	window.watch_station(QStringLiteral("ITEST1"));

	/*
	 * Put the bands on the GRAPH, which is where the stale copy lived.
	 * Reaching into the feed would test the half that already has a
	 * suite; what has never been checked is whether the window pushes
	 * anything through after a station change.
	 */
	bbq_composite drawn_before;
	drawn_before.set_series(bandful(bbq_band::observed, 1600000000, 6));
	drawn_before.set_series(bandful(bbq_band::current, 1600000000, 6));
	window.m_graph->set_composite(drawn_before);

	QVERIFY2(window.m_graph->composite().has_band(bbq_band::observed),
	         "setup failed: the graph never held the first station's band");

	window.watch_station(QStringLiteral("ITEST2"));

	const bbq_series *drawn =
	        window.m_graph->composite().band(bbq_band::observed);
	QVERIFY2(drawn == nullptr || drawn->is_empty(),
	         "the graph went on drawing the old station's observations");

	const bbq_series *now = window.m_graph->composite().band(bbq_band::current);
	QVERIFY2(now == nullptr || now->is_empty(),
	         "the graph went on drawing the old station's current reading");
}

void test_window::changing_station_clears_the_old_error() {
	/*
	 * "hourly: Connection refused" is a fact about a fetch for the
	 * previous station. Left up, it reports a fault in the station now
	 * being watched (sec 14.8.2).
	 */
	bbq_main_window window;
	window.watch_station(QStringLiteral("ITEST1"));

	window.m_last_error = QStringLiteral("hourly: Connection refused");
	window.watch_station(QStringLiteral("ITEST2"));

	QVERIFY2(window.m_last_error.isEmpty(),
	         "the old station's error survived the station changing");
}

void test_window::pinning_marks_the_station_in_the_store() {
	/*
	 * Pinning is what makes a station cost requests, so it is a control
	 * rather than a consequence (sec 14.4). The checkbox is the only
	 * way one becomes pinned, and nothing had ever exercised it.
	 */
	QTemporaryDir directory;
	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	bbq_station one;
	one.id = QStringLiteral("ITEST1");
	QVERIFY(window.feed()->history().remember_station(one));

	window.watch_station(one.id);
	QCOMPARE(static_cast<int>(window.feed()->history().pinned_stations().size()),
	         0);

	window.m_pin_box->setChecked(true);

	const std::vector<bbq_station> pinned =
	        window.feed()->history().pinned_stations();
	QCOMPARE(static_cast<int>(pinned.size()), 1);
	QCOMPARE(pinned.front().id, one.id);

	window.m_pin_box->setChecked(false);
	QCOMPARE(static_cast<int>(window.feed()->history().pinned_stations().size()),
	         0);
}

void test_window::a_warm_band_and_a_cold_one_read_differently() {
	/*
	 * The record line, which has never once been drawn (sec 14.11).
	 *
	 * Verification has been empty on every machine this has run on --
	 * a forecast is scored only after the hour it predicted has been
	 * observed -- so the code that reports a score has never had a
	 * score to report. This seeds one, which is the only way to look
	 * at it before the weather obliges.
	 *
	 * The pair is the test. A band that runs warm and one that runs
	 * cold are different problems, and the sign is the whole of what
	 * separates them, so a single case cannot ask the question: the
	 * cold one is right whatever the code does, because the minus
	 * comes from the number.
	 */
	QTemporaryDir directory;
	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	/*
	 * A station id of its own. The configuration persists across cases
	 * in one process, and watch_station returns early when the station
	 * is already the configured one -- so reusing an id from an earlier
	 * case leaves this window's feed with no station at all, and the
	 * note comes back empty for a reason that has nothing to do with
	 * what is being asked.
	 */
	const QString station = QStringLiteral("ITESTNOTE");
	window.watch_station(station);

	const qint64 now = 1700000000;
	const qint64 when = now + 3600;

	bbq_composite composite;
	composite.set_series(bandful(bbq_band::hourly, now, 6));

	bbq_history &store = window.feed()->history();

	QVERIFY(store.set_verification(station, bbq_band::hourly,
	                               QStringLiteral("temperature"),
	                               bbq_lead_bucket::hour, 12, 1.2, 1.4, 1.6));

	const QString warm = window.verification_note(composite, when, now);
	QVERIFY2(warm.contains(QStringLiteral("+1.2")),
	         qPrintable(QStringLiteral("a warm band did not say so: %1")
	                            .arg(warm)));

	QVERIFY(store.set_verification(station, bbq_band::hourly,
	                               QStringLiteral("temperature"),
	                               bbq_lead_bucket::hour, 12, -1.2, 1.4, 1.6));

	const QString cold = window.verification_note(composite, when, now);
	QVERIFY2(cold.contains(QStringLiteral("-1.2")),
	         qPrintable(QStringLiteral("a cold band did not say so: %1")
	                            .arg(cold)));

	/* And the two must not read the same, which is the point. */
	QVERIFY2(warm != cold, "warm and cold produced the same record line");
}

void test_window::the_list_names_the_station_actually_being_read() {
	/*
	 * TWO PLACES ON ONE AXIS, in the control that names the place
	 * (sec 14.12).
	 *
	 * --station overrides the configuration for one run and
	 * deliberately does not write to it, so that trying a different
	 * station leaves the configured one alone. The list did not know
	 * that: it selected `bbq_settings::station()`, the configured one,
	 * while the feed read the override. Found in a rendered shot --
	 * the box said ISTOCK822 and every number beside it came from
	 * ISTOCK877.
	 */
	QTemporaryDir directory;
	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	const QString configured = QStringLiteral("ITESTCONF");
	const QString override_id = QStringLiteral("ITESTOVER");

	for (const QString &id : {configured, override_id}) {
		bbq_station one;
		one.id = id;
		QVERIFY(window.feed()->history().remember_station(one));
	}

	/* What begin() does with an override: the feed is moved, the
	 * configuration is not. */
	bbq_settings::set_station(configured);
	window.feed()->set_station(override_id);

	window.refresh_station_list();

	QCOMPARE(window.m_station_box->currentData().toString(), override_id);
}

void test_window::the_view_still_pans_and_zooms_through_the_window() {
	/*
	 * Panning and zooming with the WHOLE window wired up.
	 *
	 * test_view drives the same gestures against a bare graph and has
	 * always passed, which says the arithmetic is right and nothing
	 * about what happens when the handlers are connected: every
	 * view_changed runs set_view_range, pushes a composite back into
	 * the graph and recomputes the corrected overlay, and any of those
	 * could undo the movement that caused them.
	 */
	QTemporaryDir directory;
	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	window.watch_station(QStringLiteral("ITESTPAN"));

	const qint64 base = 1700000000;
	bbq_composite composite;
	composite.set_series(bandful(bbq_band::hourly, base, 48));
	window.m_graph->set_composite(composite);

	/* The handlers need a plot rectangle, which is decided while
	 * painting: grabbing forces one without a window manager. */
	window.m_graph->resize(900, 400);
	window.m_graph->grab();
	window.m_graph->set_view(base, 24 * 3600);

	const qint64 before_from = window.m_graph->view_from_utc();
	const qint64 before_span = window.m_graph->view_span_s();

	const double x = window.m_graph->width() / 2.0;
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(x, 100.0),
	                  QPointF(x, 100.0), Qt::LeftButton, Qt::LeftButton,
	                  Qt::NoModifier);
	QApplication::sendEvent(window.m_graph, &press);

	QMouseEvent move(QEvent::MouseMove, QPointF(x + 150.0, 100.0),
	                 QPointF(x + 150.0, 100.0), Qt::NoButton, Qt::LeftButton,
	                 Qt::NoModifier);
	QApplication::sendEvent(window.m_graph, &move);

	QMouseEvent release(QEvent::MouseButtonRelease, QPointF(x + 150.0, 100.0),
	                    QPointF(x + 150.0, 100.0), Qt::LeftButton,
	                    Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(window.m_graph, &release);

	QVERIFY2(window.m_graph->view_from_utc() != before_from,
	         "a drag through the wired window did not move the view");
	QCOMPARE(window.m_graph->view_span_s(), before_span);

	const qint64 dragged_from = window.m_graph->view_from_utc();

	QWheelEvent zoom(QPointF(x, 100.0), window.m_graph->mapToGlobal(
	                                            QPoint(int(x), 100)),
	                 QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
	                 Qt::NoModifier, Qt::NoScrollPhase, false);
	QApplication::sendEvent(window.m_graph, &zoom);

	QVERIFY2(window.m_graph->view_span_s() != before_span,
	         "a wheel through the wired window did not change the zoom");
	Q_UNUSED(dragged_from);
}

/*
 * TURNING THE PHONE AND OPENING THE FOLD ARE ONE EVENT, and neither may
 * cost anything that was on screen (project.md sec 10).
 *
 * AndroidManifest.xml declares orientation, screenSize, screenLayout,
 * smallestScreenSize and density in configChanges, so Android resizes
 * the window rather than destroying and recreating the activity. That
 * is what keeps the state safe, and it is also what makes this testable
 * by handing a desktop window the four sizes: a window that changed
 * shape is the whole of what the app ever sees.
 *
 * The risk is local and it is in set_layout, which deletes the
 * controls' LAYOUT and relies on the widgets being held separately in
 * m_control_items. A tidy-up that rebuilt them instead would pass every
 * other test in this binary and silently clear the drop-downs every
 * time somebody turned the phone -- so the assertion here is on the
 * widget POINTERS, and the values are only the reason for caring.
 *
 * The four sizes are a foldable's, in dp: 360x800 and 800x360 folded,
 * 674x841 and 841x674 open. 674 is wide by the 600dp rule even though
 * it is taller than it is wide, which is the case an aspect-ratio test
 * would get wrong.
 *
 * THIS TEST CANNOT PASS WITHOUT DOING SOMETHING: it asserts that the
 * control shape actually changes, so a resize that never reached the
 * window fails here rather than reporting that nothing was lost.
 */
void test_window::turning_and_unfolding_the_device_keeps_what_was_on_screen() {
	QTemporaryDir directory;
	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	window.watch_station(QStringLiteral("ITESTROT"));

	/*
	 * Mobile, deliberately. The desktop shape is never stacked, so a
	 * desktop-layout run would turn the window through four sizes and
	 * assert that nothing changed, having changed nothing.
	 */
	window.set_layout(bbq_layout::mobile);

	const qint64 base = 1700000000;
	bbq_composite composite;
	composite.set_series(bandful(bbq_band::hourly, base, 48));
	window.m_graph->set_composite(composite);

	/*
	 * Android hands a window its size; it does not ask. A desktop Qt
	 * window refuses to go below its layout's minimum, and the WIDE
	 * row's minimum is about 1660 logical pixels -- so without this the
	 * window stays 1662 wide however small a phone it is pretending to
	 * be, and every assertion below would be about a desktop. Measured:
	 * the first version of this test failed with "the window is
	 * 1662x800".
	 */
	if (window.layout() != nullptr) {
		window.layout()->setSizeConstraint(QLayout::SetNoConstraint);
	}
	window.setMinimumSize(0, 0);

	window.resize(360, 800);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	QCoreApplication::processEvents();
	QCOMPARE(window.width(), 360);

	/* The plot rectangle is decided while painting; grab forces one. */
	window.m_graph->grab();
	window.m_graph->set_view(base, 24 * 3600);

	/* Two settings the user chose, each with a control that reports it. */
	window.set_smoothing(3600);
	window.set_interpolation(bbq_interpolation::linear);

	const QList<QWidget *> items = window.m_control_items;
	QVERIFY2(!items.isEmpty(), "no controls to lose");
	QVERIFY2(!window.m_wide_controls,
	         qPrintable(QStringLiteral("a folded portrait phone must stack its "
	                                   "controls; the window is %1x%2")
	                            .arg(window.width())
	                            .arg(window.height())));

	QComboBox *const smoothing = window.m_smoothing_box;
	QComboBox *const method = window.m_method_box;
	QComboBox *const station = window.m_station_box;

	const qint64 from = window.m_graph->view_from_utc();
	const qint64 span = window.m_graph->view_span_s();
	const QString named = station->currentText();

	struct shape {
		int width;
		int height;
		bool wide;
		const char *what;
	};
	const shape shapes[] = {
		{ 800, 360, true,  "folded, turned sideways" },
		{ 674, 841, true,  "unfolded, held upright" },
		{ 841, 674, true,  "unfolded, turned sideways" },
		{ 360, 800, false, "folded again, back upright" },
	};

	for (const shape &next : shapes) {
		window.resize(next.width, next.height);
		QTest::qWait(20);

		QVERIFY2(window.m_wide_controls == next.wide,
		         qPrintable(QStringLiteral("%1: asked for %2x%3, the window is "
		                                   "%4x%5, wide=%6 wanted %7")
		                            .arg(QString::fromUtf8(next.what))
		                            .arg(next.width)
		                            .arg(next.height)
		                            .arg(window.width())
		                            .arg(window.height())
		                            .arg(window.m_wide_controls)
		                            .arg(next.wide)));

		QVERIFY2(window.m_control_items == items,
		         qPrintable(QStringLiteral("the controls were rebuilt at %1")
		                            .arg(QString::fromUtf8(next.what))));
		QCOMPARE(window.m_smoothing_box, smoothing);
		QCOMPARE(window.m_method_box, method);
		QCOMPARE(window.m_station_box, station);

		QCOMPARE(window.m_graph->smoothing(), 3600);
		QVERIFY(window.m_graph->interpolation() == bbq_interpolation::linear);
		QCOMPARE(smoothing->currentData().toInt(), 3600);
		QCOMPARE(station->currentText(), named);

		QCOMPARE(window.m_graph->view_from_utc(), from);
		QCOMPARE(window.m_graph->view_span_s(), span);
	}
}

void test_window::the_record_line_reports_the_verdict_too() {
	/*
	 * A MEASUREMENT NOTHING DISPLAYS IS THE SAME FAULT AGAIN
	 * (sec 12.20).
	 *
	 * The verdict's record was added to the store in the same hour this
	 * test was written, and the readout showed temperature and rain and
	 * not it -- which is precisely the shape of defect this project has
	 * spent a session finding. Seeded, because the weather takes days
	 * to supply one and sec 14.11 is what happens when a display path
	 * is never exercised.
	 */
	QTemporaryDir directory;
	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	const QString station = QStringLiteral("ITESTVERDICT");
	window.watch_station(station);

	const qint64 now = 1700000000;
	const qint64 when = now + 3600;

	bbq_composite composite;
	composite.set_series(bandful(bbq_band::hourly, now, 6));

	bbq_history &store = window.feed()->history();
	QVERIFY(store.set_verification(station, bbq_band::hourly,
	                               QStringLiteral("grill"),
	                               bbq_lead_bucket::hour, 12, -0.3, 0.42, 0.5));

	const QString note = window.verification_note(composite, when, now);

	QVERIFY2(note.contains(QStringLiteral("verdict")),
	         qPrintable(QStringLiteral("the verdict is measured and not "
	                                   "shown: %1").arg(note)));
	QVERIFY2(note.contains(QStringLiteral("0.42")),
	         qPrintable(QStringLiteral("the verdict's error is not in the "
	                                   "line: %1").arg(note)));
}

void test_window::a_fix_from_where_discovery_already_ran_is_not_sent_to_it() {
	/*
	 * THE WIRING OF THE GATE, WHICH NOTHING ELSE GUARDS (sec 15.7.4).
	 *
	 * The feed's own tests prove discover_stations_if_moved declines a
	 * fix that has not moved. They say nothing about whether the window
	 * ASKS it -- and the window is the only caller, so a revert to the
	 * ungated discover_stations_at would leave every one of them green
	 * while the defect came straight back. A correct function nothing
	 * calls is the shape this exists to refuse.
	 *
	 * The locator's signal is emitted directly rather than by asking for
	 * a real fix: what is under test is what the window does with an
	 * answer, and a headless run has no positioning source to give one.
	 */
	QTemporaryDir directory;
	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	QVERIFY(window.feed()->history().set_discovery_origin(59.3293, 18.0686,
	                                                      QDateTime::currentSecsSinceEpoch()));

	QVERIFY2(!window.feed()->is_busy(),
	         "the fixture itself left a request outstanding");

	/* The same place discovery already ran from. */
	emit window.m_locator->located(59.3293, 18.0686);

	QVERIFY2(!window.feed()->is_busy(),
	         "the window sent an unmoved fix to discovery anyway");
}

void test_window::a_wrapping_row_asks_its_height_at_its_own_preferred_width() {
	/*
	 * THE DEFECT THIS LAYOUT SHIPPED WITH, FOR ABOUT AN HOUR
	 * (project.md sec 16.10).
	 *
	 * QWidget::sizeHint defers to QLayout::totalSizeHint, and for a
	 * height-for-width layout that asks heightForWidth(sizeHint().
	 * width()). The first version returned minimumSize() as its
	 * sizeHint -- the width of the widest single item -- so the height
	 * came back as every item on a line of its own, and the pane
	 * demanded that at EVERY width. It sat mostly empty with a
	 * scrollbar while the row inside it fitted comfortably on one line.
	 *
	 * Asserted as the relationship rather than as pixel counts: at its
	 * own preferred width a wrapping row is one row, and its minimum
	 * width is smaller than that or it could never wrap at all.
	 */
	QWidget host;
	bbq_flow_layout *row = new bbq_flow_layout(&host, 6);

	for (int i = 0; i < 8; ++i) {
		QLabel *item = new QLabel(QStringLiteral("control %1").arg(i), &host);
		item->setFixedSize(90, 24);
		row->addWidget(item);
	}

	const QSize preferred = row->sizeHint();

	QVERIFY2(row->heightForWidth(preferred.width()) <= 24 + 2,
	         "at its preferred width the row is taller than one row");

	QVERIFY2(row->minimumSize().width() < preferred.width(),
	         "the row cannot shrink, so it can never wrap");

	/*
	 * And it must actually wrap, or the whole exercise buys nothing:
	 * half the width is more than one row and less than eight.
	 */
	const int narrow = row->heightForWidth(preferred.width() / 2);
	QVERIFY2(narrow > 24 + 2, "the row did not wrap when halved");
	QVERIFY2(narrow < 8 * (24 + 6), "the row wrapped every item onto its own line");
}

void test_window::an_unscored_lead_does_not_claim_the_archive_is_empty() {
	/*
	 * TWO DIFFERENT STATES THAT READ THE SAME (project.md sec 16.14).
	 *
	 * The record describes the band and lead of the WINDOW being shown,
	 * so an empty answer means empty at that lead. It said "none yet",
	 * whose own comment justified it as "nothing has been checked yet"
	 * -- and on a phone with 500 samples banked at other leads that was
	 * simply false. A reader wondering whether the archive was doing
	 * anything was told it was not.
	 *
	 * A fresh install and a lead that has not come round yet are
	 * opposite answers to that question and must not share a sentence.
	 */
	QTemporaryDir directory;
	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	const QString station = QStringLiteral("ITESTLEAD");
	window.watch_station(station);

	const qint64 now = 1700000000;
	const qint64 when = now + 3600;

	bbq_composite composite;
	composite.set_series(bandful(bbq_band::hourly, now, 6));

	/* Nothing scored at all: the fresh install, and it says so. */
	const QString fresh = window.verification_note(composite, when, now);
	QVERIFY2(fresh.contains(QStringLiteral("none yet")),
	         qPrintable(QStringLiteral("a fresh store should say none yet: "
	                                   "%1").arg(fresh)));

	/*
	 * Now score something at a DIFFERENT lead. The window still asks
	 * about the hour bucket and still finds nothing there, but the
	 * archive is plainly working and the line must not deny it.
	 */
	bbq_history &store = window.feed()->history();
	QVERIFY(store.set_verification(station, bbq_band::hourly,
	                               QStringLiteral("temperature"),
	                               bbq_lead_bucket::four_days,
	                               40, -0.2, 0.9, 1.1));

	const QString elsewhere = window.verification_note(composite, when, now);

	QVERIFY2(!elsewhere.contains(QStringLiteral("none yet")),
	         qPrintable(QStringLiteral("500 samples banked and the line "
	                                   "still claims none yet: %1")
	                            .arg(elsewhere)));
	QVERIFY2(elsewhere.contains(QStringLiteral("1h")),
	         qPrintable(QStringLiteral("the line does not name the lead it "
	                                   "has nothing for: %1").arg(elsewhere)));
}

void test_window::the_tray_says_how_old_its_reading_is() {
	/*
	 * THE TRAY HAD NO TEST AT ALL (project.md sec 16.17).
	 *
	 * It is glanced at rather than read, which its own comment gives as
	 * the reason it is the likelier place for a stale number to be
	 * believed. Colour alone is a claim only somebody who already knows
	 * the convention can read, so staleness is said in words too -- and
	 * nothing checked that it was.
	 *
	 * Asserted on the tooltip rather than the icon: the words are what
	 * carry the meaning, and a rendered glyph would be pinned to a font.
	 */
	bbq_tray_icon tray;

	const qint64 now = QDateTime::currentSecsSinceEpoch();

	/* Never fetched: not old, unknown, and they must not read the same. */
	bbq_composite empty;
	tray.show_state(empty, QString());
	QVERIFY2(tray.toolTip().contains(QStringLiteral("Never updated")),
	         qPrintable(QStringLiteral("a tray that has never fetched should "
	                                   "say so: %1").arg(tray.toolTip())));

	/* Fetched a moment ago: an age, and no claim of staleness. */
	bbq_series fresh = bandful(bbq_band::observed, now - 3600, 4);
	fresh.set_fetched_utc(now - 300);

	bbq_composite recent;
	recent.set_series(fresh);
	tray.show_state(recent, QString());

	QVERIFY2(!tray.toolTip().contains(QStringLiteral("STALE")),
	         qPrintable(QStringLiteral("five minutes old is not stale: %1")
	                            .arg(tray.toolTip())));
	QVERIFY2(tray.toolTip().contains(QStringLiteral("min old")),
	         qPrintable(QStringLiteral("the tray does not say how old its "
	                                   "reading is: %1").arg(tray.toolTip())));

	/*
	 * And past the threshold it is said in words. Three hours against a
	 * two-hour floor: comfortably over, so the test is not measuring
	 * the boundary by accident.
	 */
	bbq_series old = bandful(bbq_band::observed, now - 3600, 4);
	old.set_fetched_utc(now - 3 * 3600);

	bbq_composite stale;
	stale.set_series(old);
	tray.show_state(stale, QString());

	QVERIFY2(tray.toolTip().contains(QStringLiteral("STALE")),
	         qPrintable(QStringLiteral("three hours old and the tray does not "
	                                   "say stale: %1").arg(tray.toolTip())));
}

void test_window::the_tray_reads_the_owner_of_now_not_the_finest_band() {
	/*
	 * WHICH BAND THE TRAY CREDITS (sec 3.18.1, sec 16.17).
	 *
	 * Radar carries no temperature after its first step. It outranks
	 * every FORECAST band at 250 but not the observed band at 300, so it
	 * wins `now` only once the observed band's last measurement has
	 * ended -- which depends on how long ago the station reported, and
	 * is why the original fault could not be reproduced on demand.
	 *
	 * WHAT IT COSTS TODAY IS THE ATTRIBUTION, not the reading. The
	 * temperature comes from resolved_at, which is band-agnostic, so it
	 * appears either way; what changes is the band named beside it. With
	 * radar owning the instant the tray says the reading came from a
	 * band that has no temperature in it, which is a false statement
	 * about where a number came from.
	 *
	 * TWO EARLIER FIXTURES DID NOT REACH THIS, and both passed against
	 * the broken code. The first gave observed samples covering now, so
	 * observed won at 300 either way. The second reached the right
	 * branch and asserted on the reading rather than the attribution,
	 * which is the half that does not change. Sabotage said so both
	 * times; reading owner_at and the priorities is what fixed it.
	 *
	 * So: observed STOPS half an hour ago, radar covers now with rain
	 * and no temperature, and an hourly band covers now with one.
	 */
	bbq_tray_icon tray;
	const qint64 now = QDateTime::currentSecsSinceEpoch();

	/* Observed, ended: the station reported and then stopped. */
	std::vector<bbq_sample> past;
	for (int i = 0; i < 4; ++i) {
		bbq_sample sample;
		sample.start_utc = now - 3600 + i * 300;
		sample.duration_s = 300;
		sample.temperature = 14.0;
		past.push_back(sample);
	}

	bbq_series measured(bbq_band::observed, QStringLiteral("wunderground"));
	measured.set_samples(past);
	measured.set_fetched_utc(now - 120);

	/* Radar over now, rain only, as the real band is past its first step. */
	std::vector<bbq_sample> drops;
	for (int i = 0; i < 6; ++i) {
		bbq_sample sample;
		sample.start_utc = now - 600 + i * 300;
		sample.duration_s = 300;
		sample.precip_rate = 0.4;
		drops.push_back(sample);
	}

	bbq_series radar(bbq_band::nowcast_fine, QStringLiteral("met.no"));
	radar.set_samples(drops);
	radar.set_fetched_utc(now - 120);

	/* And a forecast band over now that does have a temperature. */
	bbq_series hourly = bandful(bbq_band::hourly, now - 1800, 6);
	hourly.set_fetched_utc(now - 120);

	bbq_composite all;
	all.set_series(measured);
	all.set_series(radar);
	all.set_series(hourly);

	tray.show_state(all, QString());

	const QString tip = tray.toolTip();

	QVERIFY2(tip.contains(QStringLiteral(" C from ")),
	         qPrintable(QStringLiteral("the tray does not name the band its "
	                                   "reading came from: %1").arg(tip)));

	/*
	 * The discriminating assertion, and the only one here that separates
	 * the fixed code from the broken: radar has no temperature at this
	 * instant, so crediting it is a false statement about where the
	 * number came from.
	 */
	QVERIFY2(!tip.contains(QStringLiteral("from radar")),
	         qPrintable(QStringLiteral("the tray credits radar, which "
	                                   "carries no temperature here: %1")
	                            .arg(tip)));
	QVERIFY2(tip.contains(QStringLiteral("from hourly")),
	         qPrintable(QStringLiteral("the tray does not credit the band "
	                                   "that supplied the reading: %1")
	                            .arg(tip)));
}

void test_window::a_dry_spell_is_not_a_rain_forecast_with_no_skill() {
	/*
	 * A NUMBER THAT IS UNDEFINED MUST NOT PRINT AS A VERDICT
	 * (project.md sec 16.19).
	 *
	 * Brier skill is measured against always predicting the observed
	 * base rate. With no rain observed at all that baseline scores a
	 * perfect zero: there is nothing to be better than, and the
	 * quantity is undefined rather than bad. skill() answers 0.0 to
	 * avoid dividing by it, which printed as "rain skill 0.00" -- the
	 * sentence for a forecast no better than knowing nothing, earned by
	 * a band that correctly said it would stay dry every time.
	 *
	 * Rare until sec 16.12 began scoring rain through dry spells, and
	 * routine after it. This is a defect that the same evening's fix
	 * created, which is the reason it is tested rather than reasoned
	 * about.
	 */
	QTemporaryDir directory;
	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	const QString station = QStringLiteral("ITESTDRY");
	window.watch_station(station);

	const qint64 now = 1700000000;
	const qint64 when = now + 3600;

	bbq_composite composite;
	composite.set_series(bandful(bbq_band::hourly, now, 6));

	bbq_history &store = window.feed()->history();

	/*
	 * Forty scored pairings and not a drop: base rate zero, so the
	 * baseline is zero and the skill is undefined.
	 */
	QVERIFY(store.set_reliability(station, bbq_band::hourly,
	                              bbq_lead_bucket::hour, 0, 40, 0, 1.2));

	const QString dry = window.verification_note(composite, when, now);

	QVERIFY2(!dry.contains(QStringLiteral("rain skill")),
	         qPrintable(QStringLiteral("an undefined skill is printed as a "
	                                   "verdict: %1").arg(dry)));
	QVERIFY2(dry.contains(QStringLiteral("no rain in 40")),
	         qPrintable(QStringLiteral("the dry spell is not said: %1")
	                            .arg(dry)));

	/*
	 * And with rain observed the skill is a real number again, so the
	 * fix does not simply suppress the quantity.
	 */
	QVERIFY(store.set_reliability(station, bbq_band::hourly,
	                              bbq_lead_bucket::hour, 0, 40, 10, 1.2));

	const QString wet = window.verification_note(composite, when, now);
	QVERIFY2(wet.contains(QStringLiteral("rain skill")),
	         qPrintable(QStringLiteral("rain was observed and the skill is "
	                                   "still not reported: %1").arg(wet)));
}

int main(int argc, char *argv[]) {
	qputenv("QT_QPA_PLATFORM", "offscreen");

	/*
	 * The configuration goes somewhere that does not outlive the run.
	 *
	 * Qt's test mode alone would be enough to protect the real file,
	 * and it is kept below for that, but it redirects into $HOME --
	 * which leaves a settings file behind on a machine whose owner did
	 * not ask for one. Both variables are set BEFORE QApplication,
	 * because Qt resolves and caches these paths on construction.
	 */
	QTemporaryDir scratch;
	qputenv("XDG_CONFIG_HOME", scratch.filePath("config").toLocal8Bit());
	qputenv("XDG_DATA_HOME", scratch.filePath("data").toLocal8Bit());

	QApplication app(argc, argv);
	test_window suite;
	return QTest::qExec(&suite, argc, argv);
}

#include "test_window.moc"

/*
 * THE PRECONDITION THE WORST-CASE GROUND IS ONLY THE WORST CASE UNDER.
 *
 * bbq_widget_worst_ground calls the scrim over white the worst ground,
 * and that holds while the scrim stays darker than every ink it
 * protects: contrast rises as a ground moves away from an ink, so the
 * nearest reachable ground is the one that fails first.
 *
 * Let a wallpaper push the composite PAST an ink's luminance and the
 * relationship inverts. The clamp walks away from the ground, so an ink
 * that was being lifted starts being darkened -- measured on the sweep
 * in sec 16.26, Weather Underground's red goes to #f3aeb2 at one alpha
 * and to #580d11 at the next, and nothing in the picture announces the
 * threshold.
 *
 * Asserted over the dark palette's real inks rather than a sample,
 * because the question is about this program's colours at this
 * program's alpha, and both move.
 */
void test_window::no_wallpaper_can_get_between_the_scrim_and_an_ink() {
	bbq_forecast_graph graph;
	graph.set_theme(bbq_theme::dark);
	const bbq_graph_palette dark = graph.palette_colours();

	const QColor scrim = bbq_widget_scrim(dark.background);

	const QColor must_read[] = {
		dark.axis_text,
		dark.temperature,
		dark.corrected,
		dark.day_divider,
		dark.stale_warning,
		dark.now_marker,
	};

	for (const QColor &ink : must_read) {
		QVERIFY2(bbq_widget_scrim_is_bounded(scrim, ink),
		         qPrintable(QStringLiteral(
		                 "%1 is not on one side of the scrim %2 and its "
		                 "worst ground %3")
		                            .arg(ink.name(), scrim.name(),
		                                 bbq_widget_worst_ground(scrim).name())));
	}

	graph.set_theme(bbq_theme::light);
	const bbq_graph_palette light = graph.palette_colours();
	const QColor pale = bbq_widget_scrim(light.background);

	const QColor light_inks[] = {
		light.axis_text,
		light.temperature,
		light.corrected,
		light.day_divider,
		light.stale_warning,
	};

	for (const QColor &ink : light_inks) {
		QVERIFY2(bbq_widget_scrim_is_bounded(pale, ink),
		         qPrintable(ink.name()));
	}
}

/*
 * And the control: the check must be able to say no.
 *
 * A scrim so thin that the composite passes the ink is exactly the
 * condition above, constructed. Without this the test would pass
 * against a bbq_widget_scrim_is_bounded that returned true always --
 * which is the shape a predicate written in a hurry takes, and it would
 * report every alpha safe including the ones the sweep showed are not.
 */
void test_window::a_scrim_light_enough_to_pass_an_ink_is_reported_unbounded() {
	const QColor red(0xd5, 0x20, 0x2a);

	QColor thin(0x16, 0x18, 0x1a);
	thin.setAlphaF(0.4);
	QVERIFY(!bbq_widget_scrim_is_bounded(thin, red));

	QColor thick(0x16, 0x18, 0x1a);
	thick.setAlphaF(0.75);
	QVERIFY(bbq_widget_scrim_is_bounded(thick, red));
}

/*
 * HOW FAR THE CLAMP HAS TO MOVE A COLOUR, WHICH IS THE COST NOTHING
 * ELSE MEASURES (project.md sec 16.27).
 *
 * The floor is always reachable -- the walk simply goes further -- so
 * "does it still clear 3:1" cannot say whether a scrim is too thin. It
 * answers yes at every alpha down to 0.5. What actually degrades is the
 * distance travelled, and past some distance the widget has stopped
 * showing the colours the window shows.
 *
 * Worst HSL lightness shift over the protected inks, measured across
 * both schemes:
 *
 *     alpha    dark     light
 *     0.85     0.125    0.071
 *     0.80     0.180    0.094
 *     0.75     0.235    0.122     <- the scrim in use
 *     0.70     0.286    0.145
 *     0.65     0.337    0.169
 *
 * A quarter is therefore not a round number picked to pass: it is the
 * value that separates the scrim in use from the next notch thinner.
 * The assertion means "0.75 is the thinnest scrim at which this is a
 * nudge", and thinning it further has to be a decision somebody takes
 * with this test in front of them rather than a constant they edit.
 *
 * The dark scheme is the binding one, which is not the intuition: it is
 * Weather Underground's red at 1.52:1 against the dark worst ground
 * that travels furthest, not the light scheme's amber at 1.61:1.
 */
void test_window::the_clamp_nudges_a_colour_rather_than_redesigning_it() {
	const double furthest_allowed = 0.25;

	bbq_forecast_graph graph;

	for (const bbq_theme scheme : {bbq_theme::dark, bbq_theme::light}) {
		graph.set_contrast_ground(QColor(), 3.0);
		graph.set_theme(scheme);
		const bbq_graph_palette plain = graph.palette_colours();

		const QColor scrim = bbq_widget_scrim(plain.background);
		graph.set_contrast_ground(bbq_widget_worst_ground(scrim), 3.0);
		const bbq_graph_palette clamped = graph.palette_colours();

		const QColor before[] = {
			plain.axis_text,     plain.temperature,
			plain.corrected,     plain.day_divider,
			plain.stale_warning, plain.now_marker,
		};
		const QColor after[] = {
			clamped.axis_text,     clamped.temperature,
			clamped.corrected,     clamped.day_divider,
			clamped.stale_warning, clamped.now_marker,
		};

		for (size_t at = 0; at < sizeof(before) / sizeof(before[0]); ++at) {
			const double moved = qAbs(after[at].lightnessF() -
			                          before[at].lightnessF());

			QVERIFY2(moved <= furthest_allowed,
			         qPrintable(QStringLiteral(
			                 "%1 became %2, a lightness shift of %3")
			                            .arg(before[at].name(),
			                                 after[at].name())
			                            .arg(moved, 0, 'f', 3)));

			/* And it is still the same colour, not a different one that
			 * happens to be legible. */
			if (before[at].hslSaturation() > 20) {
				QCOMPARE(after[at].hslHue(), before[at].hslHue());
			}
		}
	}
}

/*
 * THE STATE THE WIDGET RENDER BORROWS, PUT BACK (sec 16.35).
 *
 * A render changes four things about the graph -- its size, its ground,
 * its contrast clamp and the readout parked by whatever the user last
 * touched -- and it does that to the LIVE graph the user is looking at,
 * every five minutes. Leaving any of them changed is a defect in the
 * window rather than in the widget.
 *
 * The first draft of this test called bbq_write_widget_picture, which
 * off Android returns immediately: it asserted that a no-op changes
 * nothing and passed for that reason. The borrow is its own type now,
 * compiled on every platform, so this exercises the thing rather than
 * the platform.
 */
void test_window::the_widget_render_leaves_the_parked_readout_where_it_was() {
	bbq_forecast_graph graph;
	graph.resize(400, 300);
	graph.set_cursor_column(37);
	graph.set_theme(bbq_theme::dark);

	const QSize was = graph.size();
	const QColor ink = graph.palette_colours().temperature;

	/* Panned away from now, as a window somebody is reading would be. */
	graph.set_view(1600000000, 6 * 3600);
	QVERIFY(!graph.is_following_now());

	{
		const bbq_borrowed_graph borrowed(&graph);

		bbq_pose_graph_for_picture(&graph, QColor(0x39, 0x3b, 0x3c), 3.0,
		                           QSize(885, 546));

		/* Really changed, or the restore below proves nothing. */
		QCOMPARE(graph.cursor_column(), -1);
		QVERIFY(!graph.opaque_background());
		QVERIFY(graph.size() != was);
		QVERIFY(graph.palette_colours().temperature != ink);
		QVERIFY(graph.is_following_now());
	}

	QCOMPARE(graph.cursor_column(), 37);
	QCOMPARE(graph.size(), was);
	QVERIFY(graph.opaque_background());
	QVERIFY(!graph.contrast_ground().isValid());
	QCOMPARE(graph.palette_colours().temperature, ink);

	/*
	 * THE VIEW, WHICH IS THE ONE THAT WOULD MOVE UNDER A HAND. A render
	 * every five minutes that left the graph following now would drag a
	 * reader back to the present while they were looking at last week.
	 */
	QVERIFY(!graph.is_following_now());
	QCOMPARE(graph.view_from_utc(), Q_INT64_C(1600000000));
	QCOMPARE(graph.view_span_s(), Q_INT64_C(6 * 3600));
}

/*
 * And the other side of it: a graph that WAS following must still be
 * following afterwards.
 *
 * Restoring by set_view() would look right in every assertion above and
 * pin the window at whatever second the render ran, so it would stop
 * tracking the clock -- a freeze that shows up minutes later as a graph
 * that has quietly stopped moving.
 */
void test_window::a_following_window_is_still_following_after_a_render() {
	bbq_forecast_graph graph;
	graph.set_theme(bbq_theme::dark);
	graph.resize(400, 300);

	QVERIFY(graph.is_following_now());

	{
		const bbq_borrowed_graph borrowed(&graph);
		bbq_pose_graph_for_picture(&graph, QColor(0x39, 0x3b, 0x3c), 3.0,
		                           QSize(885, 546));
	}

	QVERIFY(graph.is_following_now());
}

/*
 * THE POSE A PICTURE IS DRAWN FROM (sec 16.36).
 *
 * Five differences between what a window is for and what a widget is
 * for, and until this was extracted only two of them were reachable by
 * any test: the render itself is Android-only, so the clearing of the
 * readout could be checked by screenshot and by nothing else.
 *
 * The view is the one that matters most and was missed longest. It
 * belongs to whoever last dragged the graph, and a window left panned
 * at last Tuesday put last Tuesday on the home screen -- under a
 * current temperature drawn from the composite at now, beside a
 * now-marker that had gone off the edge. Every part of that picture is
 * correct and the picture is a lie.
 */
void test_window::the_picture_is_posed_at_now_with_no_cursor() {
	bbq_forecast_graph graph;
	graph.set_theme(bbq_theme::dark);
	graph.resize(400, 300);

	/* A window somebody has been using: panned away from now, with a
	 * readout parked where their finger stopped. */
	graph.set_view(1600000000, 6 * 3600);
	graph.set_cursor_column(37);

	QVERIFY2(!graph.is_following_now(),
	         "the fixture is already at now, so it cannot show the view "
	         "being brought back");
	QCOMPARE(graph.cursor_column(), 37);

	const QSize shape(885, 546);
	const QColor ground(0x39, 0x3b, 0x3c);

	bbq_pose_graph_for_picture(&graph, ground, 3.0, shape);

	QVERIFY2(graph.is_following_now(),
	         "the picture would show whatever range the window was left "
	         "panned to");
	QCOMPARE(graph.cursor_column(), -1);
	QCOMPARE(graph.size(), shape);
	QVERIFY(!graph.opaque_background());
	QCOMPARE(graph.contrast_ground(), ground);

	/* And a null graph is a no-op rather than a crash: the render calls
	 * this before it has checked much. */
	bbq_pose_graph_for_picture(nullptr, ground, 3.0, shape);
}

/*
 * THE SERVER PLACEHOLDER (sec 16.38).
 *
 * There is no server, no socket and no wire format, so what can be
 * asserted is the one thing that IS a decision rather than a
 * placeholder: the order. A machine running the packaged timer has the
 * archive on disk, so a remote must never be tried before the local
 * one -- it is slower, needs a network, and tells a third party which
 * stations somebody watches.
 *
 * Pinned by POSITION rather than by value. Asserting the literal list
 * would fail the day somebody adds a second local candidate, which is a
 * change this test should welcome; asserting that local comes first
 * fails only when the property is lost.
 */
void test_window::the_server_default_tries_a_local_host_before_a_remote_one() {
	/*
	 * The binary's config location is already redirected into a
	 * temporary directory before QApplication is built -- see the note
	 * at the top of this file -- so this reads and writes a throwaway
	 * settings file rather than the one somebody is using.
	 */
	bbq_settings::set_server_hosts(QStringList());
	const QStringList fresh = bbq_settings::server_hosts();

	QVERIFY2(!fresh.isEmpty(), "an empty default leaves a caller nothing "
	                           "to try and no way to say so");

	const auto is_local = [](const QString &host) {
		return host.startsWith(QStringLiteral("localhost")) ||
		       host.startsWith(QStringLiteral("127.")) ||
		       host.startsWith(QStringLiteral("[::1]"));
	};

	QVERIFY2(is_local(fresh.first()),
	         qPrintable(QStringLiteral("the first candidate is %1, which "
	                                   "is not local")
	                            .arg(fresh.first())));

	int remote_before_local = 0;
	bool seen_remote = false;
	for (const QString &host : fresh) {
		if (is_local(host)) {
			if (seen_remote) {
				++remote_before_local;
			}
		} else {
			seen_remote = true;
		}
	}

	QCOMPARE(remote_before_local, 0);

	/* Somebody else's list is kept as given, in their order. */
	const QStringList mine{QStringLiteral("box.lan:7373"),
	                       QStringLiteral("elsewhere:7373")};
	bbq_settings::set_server_hosts(mine);
	QCOMPARE(bbq_settings::server_hosts(), mine);

	/*
	 * And clearing it comes back to the default rather than to nothing.
	 *
	 * This assertion holds whether or not set_server_hosts removes the
	 * key, because the READER is what supplies the default -- checked
	 * by deleting that branch and watching this pass. Recorded so the
	 * next reader does not mistake it for cover the writer does not
	 * have.
	 */
	bbq_settings::set_server_hosts(QStringList());
	QCOMPARE(bbq_settings::server_hosts(), fresh);
}

/*
 * "+N MORE" HAS TO BE ABOUT SOMETHING (sec 16.42).
 *
 * The verdict names the best window and counts the rest. Until the
 * tooltip existed, the rest were reachable only by panning the plot
 * until a shaded band happened into view -- a count with nothing behind
 * it, which tells a reader there is more and not where.
 *
 * The assertion is the RELATIONSHIP between the two, not either one.
 * The label says `windows.size() - 1` more; the tooltip must carry
 * `windows.size()` lines. Pinning the text of either would go stale on
 * any wording change and would still allow the two to disagree, which
 * is the only way this can actually be wrong.
 */
void test_window::the_tooltip_lists_every_window_the_count_promises() {
	const QTimeZone utc = QTimeZone::UTC;

	std::vector<bbq_window> windows;
	for (int at = 0; at < 3; ++at) {
		bbq_window window;
		window.start_utc = 1700000000 + at * 86400;
		window.end_utc = window.start_utc + 2 * 3600;
		window.score = 0.6 + at * 0.1;
		windows.push_back(window);
	}

	const QString listed = bbq_grill_window_list(windows, utc);
	const QStringList lines = listed.split(QLatin1Char('\n'));

	/* One line per window, INCLUDING the best -- the tooltip is a list
	 * rather than a remainder, so a reader comparing them is not asked
	 * to hold the label in their head. */
	QCOMPARE(lines.size(), int(windows.size()));

	/* And the count the label would print is the rest of them. */
	const int promised = int(windows.size()) - 1;
	QCOMPARE(lines.size() - 1, promised);

	/* Each window is really described, not padded with blank lines. */
	for (const QString &line : lines) {
		QVERIFY2(line.contains(QStringLiteral("score")),
		         qPrintable(QStringLiteral("a line with no score: %1")
		                            .arg(line)));
	}

	/* One window means no "+N more" and a single line, not an empty
	 * tooltip: the best one is still worth naming. */
	const QString alone = bbq_grill_window_list({windows.front()}, utc);
	QCOMPARE(alone.split(QLatin1Char('\n')).size(), 1);

	/* And no windows is empty rather than a blank line. */
	QVERIFY(bbq_grill_window_list({}, utc).isEmpty());
}

/*
 * AND SOMETHING HAS TO CALL IT (sec 16.42.1).
 *
 * The test above proves the list is built correctly. It passed with the
 * setToolTip line deleted, because a correct function is not a working
 * feature -- this project's own rule, met by a test written an hour
 * after quoting it.
 *
 * This one drives the private refresh the feed's signal drives, and
 * asserts the label ends up holding what the builder produces. It is
 * the only assertion here that fails when the two are disconnected.
 */
void test_window::refreshing_the_status_actually_hands_the_label_that_list() {
	QTemporaryDir directory;
	QVERIFY(directory.isValid());

	bbq_main_window window;
	QVERIFY(window.feed()->open_history(
	        directory.filePath(QStringLiteral("h.sqlite"))));

	/*
	 * Warm, dry and still through the usable hours, so the policy finds
	 * windows at all -- a fixture that scores nothing would take the
	 * "no grilling window" branch and assert about the empty case while
	 * looking like it covered the full one.
	 */
	/*
	 * FROM NOW, not from a fixed epoch. refresh_status searches
	 * `now .. now + 3 days`, so a fixture pinned to 2023 describes
	 * weather the code never looks at -- the search found nothing, the
	 * label took its "no window" branch, and the assertion failed
	 * against perfectly correct code. The first draft did exactly that.
	 */
	const qint64 begins = QDateTime::currentSecsSinceEpoch() - 3600;

	std::vector<bbq_sample> samples;
	for (int at = 0; at < 3 * 24; ++at) {
		bbq_sample sample;
		sample.start_utc = begins + at * 3600;
		sample.duration_s = 3600;
		sample.temperature = 25.0;
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
	window.feed()->m_composite = composite;

	window.refresh_status();

	const QString shown = window.m_verdict->toolTip();

	QVERIFY2(!shown.isEmpty(),
	         "the verdict names a window and its tooltip lists none, so "
	         "nothing hands the label the list");

	const std::vector<bbq_window> windows = bbq_grill_windows(
	        composite, composite.zone(), QDateTime::currentSecsSinceEpoch(),
	        QDateTime::currentSecsSinceEpoch() + 3 * 24 * 3600,
	        bbq_grill_policy());

	/*
	 * Compared against the builder rather than against a literal: what
	 * must hold is that the label shows what the builder produces, and
	 * a pinned string would fail on any wording change while still
	 * allowing the two to diverge.
	 */
	if (!windows.empty()) {
		QCOMPARE(shown,
		         bbq_grill_window_list(windows, composite.zone()));
	}
}

/*
 * THE DIP, WHICH WAS COMPUTED AND READ BY NOTHING (sec 16.43).
 *
 * `bbq_window::worst` came from the scorer with the mean and had no
 * consumer anywhere -- the same shape as the palette colour no painter
 * used. Two windows with one mean are not one afternoon: 0.70 steady
 * and 0.70 with a shower through it are different plans.
 *
 * Asserted in both directions, because a line that always says "dips
 * to" says nothing, and one that never does hides the thing this
 * exists to show.
 */
void test_window::a_window_that_dips_says_so_and_a_steady_one_does_not() {
	const QTimeZone utc = QTimeZone::UTC;

	bbq_window steady;
	steady.start_utc = 1700000000;
	steady.end_utc = steady.start_utc + 3 * 3600;
	steady.score = 0.70;
	steady.worst = 0.70;

	bbq_window dips = steady;
	dips.worst = 0.31;

	const QString steady_line = bbq_grill_window_list({steady}, utc);
	const QString dips_line = bbq_grill_window_list({dips}, utc);

	QVERIFY2(!steady_line.contains(QStringLiteral("dips")),
	         qPrintable(QStringLiteral("a steady window claims a dip: %1")
	                            .arg(steady_line)));

	QVERIFY2(dips_line.contains(QStringLiteral("dips")),
	         qPrintable(QStringLiteral("a window that halves mid-way says "
	                                   "nothing about it: %1")
	                            .arg(dips_line)));
	QVERIFY2(dips_line.contains(QStringLiteral("0.31")),
	         qPrintable(dips_line));

	/*
	 * A dip too small to print differently is not a dip. The rule is
	 * "it reads as a different number", which needs no threshold to
	 * justify -- and this is the case that would break if somebody
	 * swapped it for one.
	 */
	bbq_window barely = steady;
	barely.worst = 0.7004;
	QVERIFY2(!bbq_grill_window_list({barely}, utc)
	                  .contains(QStringLiteral("dips")),
	         "a dip invisible at the printed precision is still printed");
}
