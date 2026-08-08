#include <QTemporaryDir>
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
	void the_observed_band_is_served_from_the_store();
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

void test_feed::the_observed_band_is_served_from_the_store() {
	/*
	 * The wiring of sec 12.8, checked without the network and without
	 * waiting a month for history to accumulate.
	 *
	 * The feed's own fetch path needs a provider to answer, so what is
	 * exercised here is the half that does not: rows already in the
	 * store must reach the composite when the view asks for a range
	 * that contains them. That is the whole of what panning into the
	 * past does.
	 */
	QTemporaryDir directory;

	bbq_wu_feed feed;
	QVERIFY2(feed.open_history(directory.filePath(QStringLiteral("h.sqlite"))),
	         qPrintable(feed.history_error()));
	feed.set_station(QStringLiteral("ITEST1"));

	/*
	 * A day of observations from well before anything a live fetch
	 * would return -- the point being that no fetch could produce these.
	 */
	const qint64 long_ago = 1600000000;
	std::vector<bbq_sample> samples;
	for (int i = 0; i < 288; ++i) {
		bbq_sample sample;
		sample.start_utc = long_ago + i * 300;
		sample.duration_s = 300;
		sample.temperature = 15.0 + (i % 10);
		samples.push_back(sample);
	}

	bbq_series archive(bbq_band::observed, QStringLiteral("wunderground"));
	archive.set_samples(std::move(samples));

	bbq_history writer;
	QVERIFY(writer.open(directory.filePath(QStringLiteral("h.sqlite"))));
	QCOMPARE(writer.record_observations(QStringLiteral("ITEST1"), archive), 288);

	/* Nothing has asked for that range, so nothing is loaded. */
	QVERIFY(feed.composite().band(bbq_band::observed) == nullptr);

	feed.set_view_range(long_ago, long_ago + 288 * 300);

	const bbq_series *served = feed.composite().band(bbq_band::observed);
	QVERIFY2(served != nullptr, "the store's observations never reached the "
	                            "composite");
	QCOMPARE(static_cast<int>(served->samples().size()), 288);
	QCOMPARE(served->begin_utc(), long_ago);

	/*
	 * Freshness is when the band was FETCHED, not when it was read back
	 * off disk. A store read that stamped itself as new would make sec
	 * 2.4's staleness check report a dead feed as healthy every time the
	 * view moved.
	 */
	QCOMPARE(served->fetched_utc(), static_cast<qint64>(0));

	/*
	 * A view already inside what is loaded must not go back to the
	 * database. It is called on every mouse move of a drag.
	 */
	const qint64 middle = long_ago + 144 * 300;
	feed.set_view_range(middle, middle + 3600);
	QCOMPARE(static_cast<int>(
	                 feed.composite().band(bbq_band::observed)->samples().size()),
	         288);
}

QTEST_GUILESS_MAIN(test_feed)
#include "test_feed.moc"
