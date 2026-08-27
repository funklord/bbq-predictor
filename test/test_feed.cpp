#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

#include "store/history.h"
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
	void every_station_with_a_queue_is_scored_not_just_the_watched_one();
	void one_station_s_measurements_do_not_outlive_the_station();

private:
	static bbq_series forecast_of(qint64 start, int count, double temperature);
	static bbq_series observed_of(qint64 start, int count, double temperature);
};

bbq_series test_feed::forecast_of(qint64 start, int count, double temperature) {
	std::vector<bbq_sample> samples;
	for (int i = 0; i < count; ++i) {
		bbq_sample sample;
		sample.start_utc = start + i * 3600;
		sample.duration_s = 3600;
		sample.temperature = temperature;
		samples.push_back(sample);
	}

	bbq_series made(bbq_band::hourly, QStringLiteral("test"));
	made.set_samples(std::move(samples));
	return made;
}

bbq_series test_feed::observed_of(qint64 start, int count, double temperature) {
	std::vector<bbq_sample> samples;
	for (int i = 0; i < count; ++i) {
		bbq_sample sample;
		sample.start_utc = start + i * 3600;
		sample.duration_s = 300;
		sample.temperature = temperature;
		samples.push_back(sample);
	}

	bbq_series seen(bbq_band::observed, QStringLiteral("wunderground"));
	seen.set_samples(std::move(samples));
	return seen;
}

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

void test_feed::every_station_with_a_queue_is_scored_not_just_the_watched_one() {
	/*
	 * THE DEFECT (sec 14.5).
	 *
	 * Pinning fetches a station's observations so that forecasts made
	 * while it was watched can still be scored after the view has moved
	 * on -- that is what the Pin control's own tooltip promises. The
	 * scoring ran for the watched station alone, so those observations
	 * were archived and never used, and the queue behind them never
	 * emptied. Nothing reported it: the fetches succeeded, the rows
	 * arrived, and the statistics simply stayed where they were.
	 *
	 * Three stations, and only the first is being watched. ITEST3 is
	 * neither watched nor pinned -- an abandoned queue, which leaked
	 * for ever under the old rule because expire() never reached it
	 * either.
	 */
	QTemporaryDir directory;

	bbq_wu_feed feed;
	QVERIFY2(feed.open_history(directory.filePath(QStringLiteral("h.sqlite"))),
	         qPrintable(feed.history_error()));
	feed.set_station(QStringLiteral("ITEST1"));

	const qint64 issued = 1000000;
	const qint64 valid = issued + 3600;

	QStringList queued;
	queued << QStringLiteral("ITEST1") << QStringLiteral("ITEST2")
	       << QStringLiteral("ITEST3");

	for (const QString &one : queued) {
		QCOMPARE(feed.history().record_forecast(one, forecast_of(valid, 4, 15.0),
		                                        issued),
		         4);
		feed.history().record_observations(one, observed_of(valid, 4, 17.0));
	}

	/* Pinned, which is the case the tooltip makes a promise about. */
	bbq_station second;
	second.id = QStringLiteral("ITEST2");
	QVERIFY(feed.history().remember_station(second));
	QVERIFY(feed.history().set_station_pinned(second.id, true));

	QCOMPARE(feed.history().stations_with_pending(), queued);

	QCOMPARE(feed.verify_all(), 12);

	for (const QString &one : queued) {
		QCOMPARE(feed.history().pending_count(one), 0);

		const bbq_verification scored = feed.history().verification(
		        one, bbq_band::hourly, QStringLiteral("temperature"),
		        bbq_lead_bucket::hour);

		QVERIFY2(scored.count > 0,
		         qPrintable(QStringLiteral("%1 was never scored").arg(one)));
		QCOMPARE(scored.bias, -2.0);
	}
}

void test_feed::one_station_s_measurements_do_not_outlive_the_station() {
	/*
	 * TWO PLACES ON ONE AXIS, arriving through the store instead of
	 * through the coordinate (sec 2.6.7, sec 14.8).
	 *
	 * set_station already drops the derived geocode, and says why: a
	 * coordinate belonging to the old station does not describe this
	 * one. Its MEASUREMENTS do not either, and nothing dropped them.
	 * The observed band sat in the composite until a fetch for the new
	 * station happened to replace it -- so a station changed while the
	 * network was down went on drawing the previous station's
	 * thermometer, under the new station's name, with the old
	 * station's fetch time making it look fresh.
	 */
	QTemporaryDir directory;

	bbq_wu_feed feed;
	QVERIFY2(feed.open_history(directory.filePath(QStringLiteral("h.sqlite"))),
	         qPrintable(feed.history_error()));
	feed.set_station(QStringLiteral("ITEST1"));

	const qint64 long_ago = 1600000000;
	feed.history().record_observations(QStringLiteral("ITEST1"),
	                                   observed_of(long_ago, 24, 15.0));

	feed.set_view_range(long_ago, long_ago + 24 * 3600);

	const bbq_series *first = feed.composite().band(bbq_band::observed);
	QVERIFY2(first != nullptr && !first->is_empty(),
	         "the first station's observations never reached the composite");

	/*
	 * No fetch, which is the whole point: the question is what the
	 * composite holds in the window before one arrives, and on a
	 * machine with no network that window never closes.
	 */
	feed.set_station(QStringLiteral("ITEST2"));

	const bbq_series *after = feed.composite().band(bbq_band::observed);
	QVERIFY2(after == nullptr || after->is_empty(),
	         "the old station's measurements survived the station changing");

	/*
	 * And it must read as MISSING rather than as merely empty, because
	 * that is the difference between a graph that is thin and one that
	 * is quietly wrong (sec 2.6.6).
	 */
	const std::vector<bbq_band> missing = feed.composite().missing_bands();
	QVERIFY2(std::find(missing.begin(), missing.end(), bbq_band::observed) !=
	                 missing.end(),
	         "the observed band was not reported missing after the change");
}

QTEST_GUILESS_MAIN(test_feed)
#include "test_feed.moc"
