#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QNetworkProxy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "model/settings.h"
#include "graph/forecast_graph.h"
#include "model/composite.h"
#include "store/history.h"
#include "ui/main_window.h"
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
	void changing_station_clears_the_old_curves();
	void changing_station_clears_the_old_error();
	void pinning_marks_the_station_in_the_store();

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
