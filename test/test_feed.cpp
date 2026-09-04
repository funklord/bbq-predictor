#include <QTemporaryDir>
#include <QSignalSpy>
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
	void a_new_station_has_never_been_asked();
	void no_band_describing_the_old_place_survives_the_change();
	void the_correction_is_queued_for_scoring_like_any_forecast();
	void a_finished_day_that_comes_back_short_says_so();
	void a_store_that_takes_fewer_rows_than_given_says_so();

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

	/*
	 * DEFORMED TO PASS THE GATE, and meant to be changed back (sec 6).
	 *
	 * This wants to be a braced list:
	 *
	 *     const QStringList queued = {QStringLiteral("ITEST1"),
	 *                                 QStringLiteral("ITEST2"),
	 *                                 QStringLiteral("ITEST3")};
	 *
	 * `style_gate.py` counts a braced INITIALISER as a nesting level,
	 * so the aligned continuation above is rejected while the identical
	 * continuation after `(` is accepted. The code was correct and the
	 * tool was not. Signalled to claude-guidelines; a first fix
	 * attempt failed across eleven trees, so this will be here a while.
	 * When the lexer is fixed, restore the braced form.
	 */
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

void test_feed::a_new_station_has_never_been_asked() {
	/*
	 * The freshness record is per PRODUCT; the question it answers is
	 * per STATION (sec 14.9).
	 *
	 * refresh() asks for the observed and current-station products
	 * unconditionally, which is what makes a station change fetch at
	 * once -- but it refuses to run while a round is outstanding, and
	 * changing the station is something somebody does exactly while
	 * looking at a slow one. Then the next heartbeat consults due(),
	 * finds the OLD station was asked a minute ago, and declines. The
	 * new station's measurements arrive an interval late, having been
	 * ruled fresh on the strength of a question about somewhere else.
	 */
	bbq_wu_feed feed;
	const qint64 now = 1700000000;

	feed.set_station(QStringLiteral("ITEST1"));

	/*
	 * The record is written directly rather than by fetching, which is
	 * the whole reason this class is a friend: attempt() would put a
	 * request on the wire, and what is under test is the bookkeeping
	 * either side of one.
	 */
	feed.m_attempted.insert(static_cast<int>(bbq_wu_product::observed), now);
	feed.m_attempted.insert(static_cast<int>(bbq_wu_product::current_station),
	                        now);

	QVERIFY2(!feed.due(bbq_wu_product::observed, now + 60),
	         "a product asked for a minute ago was already due again");

	feed.set_station(QStringLiteral("ITEST2"));

	QVERIFY2(feed.due(bbq_wu_product::observed, now + 60),
	         "the new station inherited the old one's freshness");
	QVERIFY2(feed.due(bbq_wu_product::current_station, now + 60),
	         "the new station inherited the old one's freshness");
}

void test_feed::no_band_describing_the_old_place_survives_the_change() {
	/*
	 * The whole composite, not just the observed band (sec 14.8.1).
	 *
	 * Fixing the observed band alone left the same fault in five
	 * others, and in the worst one: `current` is fetched by STATION id
	 * and outranks everything at the present instant, so a stale one
	 * answers "what is it doing now" with another station's
	 * thermometer. The forecast bands are fetched by COORDINATE, and
	 * the coordinate is dropped by this same function -- so they
	 * describe a place the feed has stopped claiming.
	 */
	bbq_wu_feed feed;
	feed.set_station(QStringLiteral("ITEST1"));
	feed.set_geocode(59.33, 18.07, false);

	const bbq_band every[] = {
		bbq_band::observed, bbq_band::current, bbq_band::nowcast_fine,
		bbq_band::nowcast,  bbq_band::extended, bbq_band::hourly,
	};

	for (bbq_band band : every) {
		bbq_series series(band, QStringLiteral("test"));
		series.set_samples(observed_of(1600000000, 4, 15.0).samples());
		feed.m_composite.set_series(std::move(series));
	}

	for (bbq_band band : every) {
		const bbq_series *held = feed.composite().band(band);
		QVERIFY2(held != nullptr && !held->is_empty(), "setup failed");
	}

	feed.set_station(QStringLiteral("ITEST2"));

	for (bbq_band band : every) {
		const bbq_series *held = feed.composite().band(band);
		const QString name = QString::fromLatin1(bbq_band_name(band));
		QVERIFY2(held == nullptr || held->is_empty(),
		         qPrintable(QStringLiteral("the %1 band survived the station "
		                                   "changing").arg(name)));
	}

	/*
	 * AND THE OTHER WAY, because a fix that simply emptied the whole
	 * composite would pass everything above and be wrong.
	 *
	 * A PINNED coordinate is not the station's and is not dropped, so
	 * the bands fetched for it still describe the place they were asked
	 * about. Only the two that are asked for by station id may go.
	 */
	bbq_wu_feed pinned;
	pinned.set_geocode(59.33, 18.07, true);
	pinned.set_station(QStringLiteral("ITEST1"));

	for (bbq_band band : every) {
		bbq_series series(band, QStringLiteral("test"));
		series.set_samples(observed_of(1600000000, 4, 15.0).samples());
		pinned.m_composite.set_series(std::move(series));
	}

	pinned.set_station(QStringLiteral("ITEST2"));

	const bbq_band by_coordinate[] = {
		bbq_band::nowcast_fine, bbq_band::nowcast,
		bbq_band::extended,     bbq_band::hourly,
	};

	for (bbq_band band : by_coordinate) {
		const bbq_series *held = pinned.composite().band(band);
		const QString name = QString::fromLatin1(bbq_band_name(band));
		QVERIFY2(held != nullptr && !held->is_empty(),
		         qPrintable(QStringLiteral("the %1 band was dropped though its "
		                                   "coordinate was pinned").arg(name)));
	}

	QVERIFY2(pinned.composite().band(bbq_band::current) == nullptr ||
	                 pinned.composite().band(bbq_band::current)->is_empty(),
	         "the station's own current band survived a station change");
}

void test_feed::the_correction_is_queued_for_scoring_like_any_forecast() {
	/*
	 * THE ONE CLAIM NOTHING CHECKED (project.md sec 12.19).
	 *
	 * The corrected band was computed for the screen and never
	 * archived, so the store held scores for every band the providers
	 * supply and none for the one this project produces itself. The
	 * program's only original claim -- that removing a measured bias
	 * improves a forecast -- was the only claim in it nobody was
	 * measuring.
	 */
	QTemporaryDir directory;

	bbq_wu_feed feed;
	QVERIFY2(feed.open_history(directory.filePath(QStringLiteral("h.sqlite"))),
	         qPrintable(feed.history_error()));

	const QString station = QStringLiteral("ITESTCORR");
	feed.set_station(station);

	const qint64 now = 1700000000;

	/*
	 * A measured bias to correct BY. Below the minimum the correction
	 * is empty by design, so this seeds enough of it to act on.
	 */
	QVERIFY(feed.history().set_verification(station, bbq_band::hourly,
	                                        QStringLiteral("temperature"),
	                                        bbq_lead_bucket::hour, 40, 2.0,
	                                        2.0, 2.0));
	QVERIFY(feed.history().set_verification(station, bbq_band::hourly,
	                                        QStringLiteral("temperature"),
	                                        bbq_lead_bucket::three_hours, 40,
	                                        2.0, 2.0, 2.0));

	/* A forecast to correct. */
	std::vector<bbq_sample> samples;
	for (int i = 0; i < 8; ++i) {
		bbq_sample sample;
		sample.start_utc = now + i * 3600;
		sample.duration_s = 3600;
		sample.temperature = 15.0;
		samples.push_back(sample);
	}

	bbq_series hourly(bbq_band::hourly, QStringLiteral("test"));
	hourly.set_samples(std::move(samples));
	feed.m_composite.set_series(std::move(hourly));

	QCOMPARE(feed.history().pending_count(station), 0);

	const int queued = feed.record_corrected(now);

	QVERIFY2(queued > 0, "the correction was not queued for scoring at all");

	/*
	 * Asserted on the BAND, not just on the count. Recording the hourly
	 * band again would satisfy a bare count and would measure the thing
	 * that was already measured.
	 */
	QCOMPARE(feed.history().pending_count(station, bbq_band::corrected), queued);

	/*
	 * And nothing else. Recording the hourly band again would satisfy a
	 * bare count while measuring the thing that was already measured.
	 */
	QCOMPARE(feed.history().pending_count(station, bbq_band::hourly), 0);
	QCOMPARE(feed.history().pending_count(station), queued);
}

void test_feed::a_finished_day_that_comes_back_short_says_so() {
	/*
	 * A SHORT ANSWER IS NOT AN ERROR, which is why this exists
	 * (project.md sec 12.13.1).
	 *
	 * A stale cache variant returned 78 observations where the day held
	 * 288, and nothing could tell: every band answered, every status was
	 * 200, and 78 rows parse exactly as well as 288. The only evidence
	 * was a store that quietly stopped growing.
	 *
	 * Checked on TIME rather than count, because a station reporting
	 * every fifteen minutes is as normal as one reporting every five,
	 * and a threshold on rows would have to know which. Whatever the
	 * cadence, a day that has ENDED should be answered with observations
	 * reaching its end.
	 */
	bbq_wu_feed feed;
	feed.set_station(QStringLiteral("ITEST1"));

	QSignalSpy complaints(&feed, &bbq_wu_feed::band_failed);

	const QDate day(2026, 9, 3);
	const qint64 begins = QDateTime(day, QTime(0, 0)).toSecsSinceEpoch();

	/* A whole day: five-minute rows to within a few minutes of midnight. */
	std::vector<bbq_sample> whole;
	for (int i = 0; i < 288; ++i) {
		bbq_sample sample;
		sample.start_utc = begins + i * 300;
		sample.duration_s = 300;
		sample.temperature = 15.0;
		whole.push_back(sample);
	}

	bbq_series full(bbq_band::observed, QStringLiteral("wunderground"));
	full.set_samples(whole);

	feed.m_backfill_day = day;
	feed.check_day_is_whole(full);
	QCOMPARE(complaints.count(), 0);

	/*
	 * The same day truncated where the real one was -- a little over six
	 * hours in, seventeen short of its end.
	 */
	std::vector<bbq_sample> cut(whole.begin(), whole.begin() + 78);
	bbq_series stale(bbq_band::observed, QStringLiteral("wunderground"));
	stale.set_samples(cut);

	feed.m_backfill_day = day;
	feed.check_day_is_whole(stale);

	QCOMPARE(complaints.count(), 1);
	QVERIFY2(complaints.at(0).at(1).toString().contains(QStringLiteral("hole")),
	         "the complaint does not say what is wrong");

	/*
	 * And it fires once. The day is cleared when it is checked, so a
	 * second response for the same request cannot complain twice about
	 * a day nobody asked for again.
	 */
	feed.check_day_is_whole(stale);
	QCOMPARE(complaints.count(), 1);
}

void test_feed::a_store_that_takes_fewer_rows_than_given_says_so() {
	/*
	 * The store returns how many rows it wrote and every caller
	 * discarded it, while a failed insert set an error nothing read
	 * after opening (project.md sec 12.13.2). A write that lost half a
	 * day looked exactly like one that lost nothing.
	 */
	QTemporaryDir directory;

	bbq_wu_feed feed;
	QSignalSpy complaints(&feed, &bbq_wu_feed::band_failed);

	/*
	 * With no store open there is nothing to be wrong about, and a
	 * program that has not been given an archive must not complain on
	 * every fetch -- sec 12 makes the store optional on purpose.
	 */
	feed.note_partial_store(288, 0);
	QCOMPARE(complaints.count(), 0);

	QVERIFY(feed.open_history(directory.filePath(QStringLiteral("h.sqlite"))));

	/* Agreement is silence. */
	feed.note_partial_store(288, 288);
	QCOMPARE(complaints.count(), 0);

	/* Disagreement is not. */
	feed.note_partial_store(288, 140);
	QCOMPARE(complaints.count(), 1);

	const QString said = complaints.at(0).at(1).toString();
	QVERIFY2(said.contains(QStringLiteral("140")) &&
	                 said.contains(QStringLiteral("288")),
	         qPrintable(QStringLiteral("the complaint does not say how much "
	                                   "was lost: %1").arg(said)));
}

QTEST_GUILESS_MAIN(test_feed)
#include "test_feed.moc"
