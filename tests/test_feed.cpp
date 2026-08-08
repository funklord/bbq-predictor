#include <QTest>

#include "wu/feed.h"

/*
 * Which coordinate the forecast bands are aimed at (project.md sec
 * 2.6.7.3).
 *
 * Nothing here touches the network: set_station and set_geocode are
 * bookkeeping, and refresh() -- the only method that would fetch -- is
 * never called. The feed builds its own network manager on
 * construction, but an idle manager opens nothing.
 */
class test_feed : public QObject {
	Q_OBJECT

private slots:
	void a_derived_coordinate_does_not_survive_the_station_changing();
	void a_pinned_coordinate_does();
	void resetting_the_same_station_changes_nothing();
};

void test_feed::a_derived_coordinate_does_not_survive_the_station_changing() {
	bbq_wu_feed feed;

	feed.set_station(QStringLiteral("ISTOCK822"));
	feed.set_geocode(59.33, 18.07, false);
	QVERIFY(feed.has_geocode());

	feed.set_station(QStringLiteral("IGOTHENB12"));

	/*
	 * The failure this replaces: the coordinate stayed, so refresh()
	 * aimed the forecast bands at the old station's garden while the
	 * observed band read the new one -- and because a geocode was still
	 * held, the handler that derives one from the station response never
	 * ran, so the right coordinate was never learned at all.
	 */
	QVERIFY2(!feed.has_geocode(),
	         "the old station's coordinate survived the station changing");
}

void test_feed::a_pinned_coordinate_does() {
	bbq_wu_feed feed;

	/* An override, or --geocode: chosen by configuration, owned by no
	 * station, and so not the station's to invalidate. */
	feed.set_geocode(59.33, 18.07, true);
	feed.set_station(QStringLiteral("ISTOCK822"));
	QVERIFY(feed.has_geocode());

	feed.set_station(QStringLiteral("IGOTHENB12"));
	QVERIFY2(feed.has_geocode(),
	         "a pinned coordinate was discarded by a station change");
}

void test_feed::resetting_the_same_station_changes_nothing() {
	bbq_wu_feed feed;

	feed.set_station(QStringLiteral("ISTOCK822"));
	feed.set_geocode(59.33, 18.07, false);

	/*
	 * The UI writes the station on editingFinished, which fires when the
	 * field merely loses focus. Treating that as a change would throw
	 * away a good coordinate every time somebody clicked past the box.
	 */
	feed.set_station(QStringLiteral("ISTOCK822"));
	QVERIFY(feed.has_geocode());
}

QTEST_GUILESS_MAIN(test_feed)
#include "test_feed.moc"
