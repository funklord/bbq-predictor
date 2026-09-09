#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>

#include "store/history.h"
#include "wu/feed.h"
#include "wu/fetch_verdict.h"

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
	void a_clock_that_moved_back_does_not_stall_a_band();
	void a_finished_day_is_told_from_one_still_running();
	void a_late_endpoint_is_told_from_a_quiet_station();
	void a_band_never_asked_for_is_not_a_band_that_answered();
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
	void a_station_that_stops_reporting_is_named();
	void a_day_the_store_already_holds_whole_is_not_asked_for();
	void a_day_with_a_hole_in_it_is_still_asked_for();
	void the_pinned_queue_is_built_from_that_decision();
	void initTestCase();
	void a_fix_that_has_not_moved_does_not_rediscover();
	void a_first_fix_and_a_distant_one_both_do();
	void the_origin_moves_only_when_an_answer_arrives();
	void a_reader_who_never_moves_still_rediscovers_eventually();

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

	/*
	 * TWO STEPS, because the complaint waits for the round
	 * (sec 16.106). check_day_is_whole measures the shortfall and holds
	 * it; report_observed_staleness says so once the round has settled,
	 * which is the first moment the current band is there to say
	 * WHETHER THE HOLE WILL CLOSE. Driving only the first step would
	 * assert that nothing is said, which is true and is not the claim.
	 */
	feed.check_day_is_whole(full);
	feed.report_observed_staleness();
	QCOMPARE(complaints.count(), 0);

	/*
	 * The same day truncated where the real one was -- a little over six
	 * hours in, seventeen short of its end.
	 */
	std::vector<bbq_sample> cut(whole.begin(), whole.begin() + 78);
	bbq_series stale(bbq_band::observed, QStringLiteral("wunderground"));
	stale.set_samples(cut);

	feed.check_day_is_whole(stale);
	feed.report_observed_staleness();

	QCOMPARE(complaints.count(), 1);
	QVERIFY2(complaints.at(0).at(1).toString().contains(QStringLiteral("hole")),
	         "the complaint does not say what is wrong");

	/*
	 * THE SAME SERIES, JUDGED AGAIN, GETS THE SAME ANSWER (sec 16.63).
	 *
	 * This asserted the opposite until the swap was found: the day was
	 * remembered in a member and cleared when it was read, so a second
	 * call fell through to the staleness branch instead -- and the
	 * comment here called that correct. It was the bug, written down as
	 * intended behaviour.
	 *
	 * There is no member now. Which question a reply answers comes out
	 * of the reply, so asking twice cannot change the answer, and two
	 * short days really do earn two complaints because in a real round
	 * they are two replies.
	 */
	feed.check_day_is_whole(stale);
	feed.report_observed_staleness();

	int holes = 0;
	for (const QList<QVariant> &said : complaints) {
		if (said.at(1).toString().contains(QStringLiteral("hole"))) {
			++holes;
		}
	}

	QCOMPARE(holes, 2);
	QCOMPARE(complaints.count(), 2);
	/*
	 * AND WHETHER THE HOLE WILL CLOSE (sec 16.106).
	 *
	 * "the archive has a hole in it" is true and reads as a fault in
	 * this program's storage. On the day this was written the window
	 * carried it while the cause was the provider's history endpoint
	 * fourteen hours behind its own current one -- which fills in by
	 * itself. A reader told the archive is holed and not told it is
	 * self-healing has been given the alarming half.
	 *
	 * The station answering is what says which it is, and it is
	 * fetched in the same round.
	 */
	bbq_sample fresh;
	fresh.start_utc = QDateTime::currentSecsSinceEpoch() - 120;
	fresh.duration_s = 300;
	fresh.temperature = 15.0;

	bbq_series live(bbq_band::current, QStringLiteral("wunderground"));
	live.set_samples({fresh});
	feed.m_composite.set_series(std::move(live));

	feed.check_day_is_whole(stale);
	feed.report_observed_staleness();
	QCOMPARE(complaints.count(), 3);

	const QString explained = complaints.at(2).at(1).toString();
	QVERIFY2(explained.contains(QStringLiteral("hole")),
	         qPrintable(QStringLiteral("the shortfall stopped being reported: "
	                                   "%1").arg(explained)));
	QVERIFY2(explained.contains(QStringLiteral("should fill in")),
	         qPrintable(QStringLiteral("a self-healing hole was not said to "
	                                   "be one: %1").arg(explained)));

	/*
	 * The control: the earlier complaints, made with no current band in
	 * the composite, must NOT carry that reassurance. Without this the
	 * test would pass against a program that appended it always.
	 */
	QVERIFY2(!complaints.at(1).at(1).toString().contains(
	                 QStringLiteral("should fill in")),
	         "a hole with no evidence about the station was called healing");
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

void test_feed::a_station_that_stops_reporting_is_named() {
	/*
	 * A QUIET STATION READS AS HEALTHY (project.md sec 12.13.3).
	 *
	 * Measured on the night this was written: the watched station's
	 * newest observation sat at 02:04 while a neighbour was current to
	 * 03:19. Every fetch succeeded and returned the same rows, so the
	 * staleness check -- which answers "when did we last fetch" --
	 * stayed green throughout.
	 */
	bbq_wu_feed feed;
	feed.set_station(QStringLiteral("ITESTQUIET"));

	QSignalSpy complaints(&feed, &bbq_wu_feed::band_failed);

	/*
	 * A NAMED MOMENT, NOT THE CLOCK (sec 16.105).
	 *
	 * A station is quiet when its newest sample is TODAY'S and more
	 * than 45 minutes old, and in the first 45 minutes after midnight
	 * no sample can be both -- so this test, which looks back 78
	 * minutes, cannot be satisfied before 01:18. It failed at 00:59 on
	 * nothing to do with the code, which is 78 minutes of every day.
	 *
	 * Half past midday, so every offset below stays inside its own day
	 * whatever hour the suite runs at.
	 */
	const qint64 now =
	        QDateTime(QDate::currentDate(), QTime(12, 30)).toSecsSinceEpoch();

	const auto reporting_until = [](qint64 last) {
		std::vector<bbq_sample> rows;
		for (int i = 0; i < 12; ++i) {
			bbq_sample sample;
			sample.start_utc = last - (11 - i) * 300;
			sample.duration_s = 300;
			sample.temperature = 12.0;
			rows.push_back(sample);
		}

		bbq_series made(bbq_band::observed, QStringLiteral("wunderground"));
		made.set_samples(std::move(rows));
		return made;
	};

	/*
	 * TWO STEPS, because the verdict waits for the round (sec 16.101).
	 *
	 * check_day_is_whole measures how far behind the observed band is
	 * and records it; report_observed_staleness says so once the round
	 * has settled, which is the first moment the current band is
	 * present to name the cause. Driving only the first step here would
	 * assert that nothing is said, which is true and is not the claim.
	 */
	feed.check_day_is_whole(reporting_until(now - 300), now);
	feed.report_observed_staleness(now);
	QCOMPARE(complaints.count(), 0);

	/* Quiet: the gap that went unremarked. */
	feed.check_day_is_whole(reporting_until(now - 78 * 60), now);
	feed.report_observed_staleness(now);
	QCOMPARE(complaints.count(), 1);

	const QString said = complaints.at(0).at(1).toString();
	QVERIFY2(said.contains(QStringLiteral("ITESTQUIET")),
	         qPrintable(QStringLiteral("the complaint does not name the "
	                                   "station: %1").arg(said)));

	/*
	 * And a BACKFILL is not judged this way. Yesterday's newest
	 * observation is a day old by definition, so the same series would
	 * be called quiet on every single backfill if the two checks were
	 * confused.
	 */
	const QDate yesterday = QDate::currentDate().addDays(-1);
	const qint64 midday =
	        QDateTime(yesterday, QTime(12, 0)).toSecsSinceEpoch();

	/*
	 * NOTHING IS ARRANGED FOR IT HERE, and that is the change worth
	 * noticing. This test named the fault exactly -- and set the member
	 * by hand first, so it was asking whether the check works when its
	 * caller gets the day right. The caller was what got it wrong.
	 */
	feed.check_day_is_whole(reporting_until(midday), now);
	feed.report_observed_staleness(now);
	QCOMPARE(complaints.count(), 2);
	QVERIFY2(complaints.at(1).at(1).toString().contains(QStringLiteral("hole")),
	         "a backfill was judged as a quiet station rather than as a day");

	/*
	 * AND THE SAME STALENESS, WITH THE STATION ANSWERING, NAMES THE
	 * ENDPOINT INSTEAD (sec 16.101).
	 *
	 * This is the case that cost a reader an hour: the observed band
	 * half a day behind while the station's own current endpoint had a
	 * reading minutes old. Same measurement, different culprit, and the
	 * sentence has to say which.
	 */
	bbq_sample fresh;
	fresh.start_utc = now - 120;
	fresh.duration_s = 300;
	fresh.temperature = 15.0;

	bbq_series live(bbq_band::current, QStringLiteral("wunderground"));
	live.set_samples({fresh});
	feed.m_composite.set_series(std::move(live));

	feed.check_day_is_whole(reporting_until(now - 78 * 60), now);
	feed.report_observed_staleness(now);
	QCOMPARE(complaints.count(), 3);

	const QString blamed = complaints.at(2).at(1).toString();
	QVERIFY2(blamed.contains(QStringLiteral("history endpoint")),
	         qPrintable(QStringLiteral("a live station was still called quiet: "
	                                   "%1").arg(blamed)));
	QVERIFY2(blamed.contains(QStringLiteral("ITESTQUIET")),
	         qPrintable(QStringLiteral("the complaint stopped naming the "
	                                   "station: %1").arg(blamed)));
}

/*
 * Fill `store` with `rows` five-minute observations for `station` on
 * `day`.
 *
 * 288 of them reach 23:55, five minutes short of midnight and well
 * inside the three-hour tolerance. 78 is the number the real stale cache
 * returned (sec 12.13.1) and leaves the day seventeen hours short.
 */
static void fill_day(bbq_history &store, const QString &station,
                     const QDate &day, int rows) {
	const qint64 begins = QDateTime(day, QTime(0, 0)).toSecsSinceEpoch();

	std::vector<bbq_sample> samples;
	for (int i = 0; i < rows; ++i) {
		bbq_sample sample;
		sample.start_utc = begins + i * 300;
		sample.duration_s = 300;
		sample.temperature = 15.0;
		samples.push_back(sample);
	}

	bbq_series series(bbq_band::observed, QStringLiteral("wunderground"));
	series.set_samples(samples);
	store.record_observations(station, series);
}

void test_feed::a_day_the_store_already_holds_whole_is_not_asked_for() {
	/*
	 * THE WIRING, AND THE ONE THAT MUST GO RED IF THE SKIP IS REMOVED
	 * (project.md sec 15.7.1).
	 *
	 * The defect was not that the arithmetic was wrong -- there was no
	 * arithmetic. attempt_backfill fetched yesterday unconditionally, so
	 * every launch spent a request on a day the archive already held
	 * complete, and the archive UPSERTS: re-fetching changed no row and
	 * left no trace. A row count cannot tell the two apart, which is why
	 * this asserts on a request NOT going out instead.
	 *
	 * is_busy() is the observable. attempt_backfill increments the
	 * outstanding count synchronously before it asks, so a fetch is
	 * visible the instant it is decided on and no reply is needed to see
	 * it.
	 */
	QTemporaryDir scratch;
	QVERIFY(scratch.isValid());

	bbq_wu_feed feed;
	feed.set_station(QStringLiteral("ITEST1"));
	QVERIFY(feed.open_history(scratch.filePath(QStringLiteral("h.sqlite"))));

	const QDate yesterday = QDate::currentDate().addDays(-1);
	fill_day(feed.history(), QStringLiteral("ITEST1"), yesterday, 288);

	QVERIFY2(!feed.is_busy(), "the fixture itself started a fetch");

	feed.attempt_backfill(QDateTime::currentSecsSinceEpoch());

	QVERIFY2(!feed.is_busy(),
	         "a day the store already holds whole was asked for again");
}

void test_feed::a_day_with_a_hole_in_it_is_still_asked_for() {
	/*
	 * The other half, and the reason the skip cannot be "never backfill
	 * twice": a day that came back short is exactly what the backfill
	 * exists to repair, so it must still be asked for.
	 *
	 * Asserted on the DECISION rather than on a request going out,
	 * because this branch fetches and the suite does not touch the
	 * network. The wiring is covered by the test above and the pinned
	 * one below; this covers what the decision says.
	 */
	QTemporaryDir scratch;
	QVERIFY(scratch.isValid());

	bbq_wu_feed feed;
	feed.set_station(QStringLiteral("ITEST1"));
	QVERIFY(feed.open_history(scratch.filePath(QStringLiteral("h.sqlite"))));

	const QDate yesterday = QDate::currentDate().addDays(-1);

	/* Nothing at all: the case the backfill exists for. */
	QCOMPARE(feed.backfill_day_wanted(QStringLiteral("ITEST1")), yesterday);

	/* Short by seventeen hours, as the real stale cache was. */
	fill_day(feed.history(), QStringLiteral("ITEST1"), yesterday, 78);
	QCOMPARE(feed.backfill_day_wanted(QStringLiteral("ITEST1")), yesterday);

	/* Filled in: now there is nothing worth asking for. */
	fill_day(feed.history(), QStringLiteral("ITEST1"), yesterday, 288);
	QVERIFY2(!feed.backfill_day_wanted(QStringLiteral("ITEST1")).isValid(),
	         "a whole day is still being asked for");

	/*
	 * A store that cannot answer says ASK rather than skip. Declining on
	 * the strength of not knowing would turn an unopened store into a
	 * silent refusal to ever backfill.
	 */
	bbq_wu_feed unopened;
	unopened.set_station(QStringLiteral("ITEST1"));
	QCOMPARE(unopened.backfill_day_wanted(QStringLiteral("ITEST1")), yesterday);
}

void test_feed::the_pinned_queue_is_built_from_that_decision() {
	/*
	 * The pinned path is where the cost was largest -- every pinned
	 * station was another request per launch -- and it is a separate
	 * call site, so the watched station's test says nothing about it. A
	 * skip that worked for the watched station and not for pinned ones
	 * would look exactly like a working fix from outside.
	 */
	QTemporaryDir scratch;
	QVERIFY(scratch.isValid());

	bbq_wu_feed feed;
	feed.set_station(QStringLiteral("IWATCHED"));
	QVERIFY(feed.open_history(scratch.filePath(QStringLiteral("h.sqlite"))));

	const qint64 seen = QDateTime::currentSecsSinceEpoch();

	bbq_station pinned;
	pinned.id = QStringLiteral("IPINNED1");
	pinned.first_seen_utc = seen;
	pinned.last_seen_utc = seen;
	QVERIFY(feed.history().remember_station(pinned));
	QVERIFY(feed.history().set_station_pinned(pinned.id, true));

	const QDate yesterday = QDate::currentDate().addDays(-1);

	/* Holding nothing for that day, it is worth fetching. */
	QCOMPARE(feed.pinned_worth_fetching(), QStringList{pinned.id});

	/* Holding yesterday whole, it is not. */
	fill_day(feed.history(), pinned.id, yesterday, 288);
	QVERIFY2(feed.pinned_worth_fetching().isEmpty(),
	         "a pinned station whose day is whole is still queued");

	/*
	 * And the watched station is never queued here whatever the store
	 * holds -- it is fetched properly and far more often, so queueing it
	 * would spend a request to learn what it already knows.
	 */
	bbq_station watched;
	watched.id = QStringLiteral("IWATCHED");
	watched.first_seen_utc = seen;
	watched.last_seen_utc = seen;
	QVERIFY(feed.history().remember_station(watched));
	QVERIFY(feed.history().set_station_pinned(watched.id, true));

	QVERIFY2(feed.pinned_worth_fetching().isEmpty(),
	         "the watched station was queued as a pinned one");
}

void test_feed::initTestCase() {
	/*
	 * A request that escapes cannot leave the machine.
	 *
	 * The rest of this file never fetches, but the discovery tests below
	 * assert on a request GOING OUT as well as on one being declined --
	 * and a suite that fired at Weather Underground on every run would
	 * be wrong whatever it was measuring, this project scraping a key it
	 * is not licensed to have. Same guard, and same reason, as
	 * test_window's.
	 */
	QNetworkProxy blocked(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"),
	                      1);
	QNetworkProxy::setApplicationProxy(blocked);
}

void test_feed::a_fix_that_has_not_moved_does_not_rediscover() {
	/*
	 * THE WIRING, AND THE ONE THAT MUST GO RED IF THE GATE IS REMOVED
	 * (project.md sec 15.7.4).
	 *
	 * Every launch asked the device where it was and sent whatever
	 * arrived straight to discovery, so a launch from the same kitchen
	 * spent a request to be told the list the archive already held.
	 *
	 * is_busy() is the observable, as it is for the backfill: the
	 * outstanding count rises synchronously before the request, so the
	 * decision is visible without any reply.
	 */
	QTemporaryDir scratch;
	QVERIFY(scratch.isValid());

	bbq_wu_feed feed;
	QVERIFY(feed.open_history(scratch.filePath(QStringLiteral("h.sqlite"))));

	/*
	 * Where discovery last ran: a real place, to a real precision, and
	 * a FRESH stamp.
	 *
	 * The stamp matters since sec 16.20: an origin older than a week is
	 * rediscovered however still the reader has been, so a fixture with
	 * an ancient one would be asking two questions at once and failing
	 * for the wrong reason. This case is about movement alone.
	 */
	QVERIFY(feed.history().set_discovery_origin(59.3293, 18.0686,
	                                            QDateTime::currentSecsSinceEpoch()));

	QVERIFY2(!feed.is_busy(), "the fixture itself started a fetch");

	/* The same spot, and then 300 m away -- inside a coarse fix's own error. */
	QCOMPARE(feed.discover_stations_if_moved(59.3293, 18.0686), false);
	QCOMPARE(feed.discover_stations_if_moved(59.3320, 18.0686), false);

	QVERIFY2(!feed.is_busy(),
	         "a fix that had not moved was sent to discovery anyway");
}

void test_feed::a_first_fix_and_a_distant_one_both_do() {
	/*
	 * The other half, and the reason the gate cannot be "never
	 * rediscover": a fresh install has no origin and must discover, and
	 * somebody who has actually travelled must too.
	 *
	 * Asserted on is_busy() rather than on the decision alone, so this
	 * is wiring as well -- a gate that declined everything would pass a
	 * decision-only test and leave the station list empty for ever.
	 */
	QTemporaryDir scratch;
	QVERIFY(scratch.isValid());

	bbq_wu_feed feed;
	QVERIFY(feed.open_history(scratch.filePath(QStringLiteral("h.sqlite"))));

	/* Never discovered: -1 reads as "must", not as "has not moved". */
	QCOMPARE(feed.moved_since_discovery(59.3293, 18.0686), -1.0);
	QCOMPARE(feed.discover_stations_if_moved(59.3293, 18.0686), true);
	QVERIFY2(feed.is_busy(), "a first fix did not discover");

	bbq_wu_feed travelled;
	QVERIFY(travelled.open_history(
	        scratch.filePath(QStringLiteral("t.sqlite"))));
	QVERIFY(travelled.history().set_discovery_origin(59.3293, 18.0686,
	                                                 QDateTime::currentSecsSinceEpoch()));

	/* Uppsala, some 63 km away: the list cannot be the same. */
	const double moved = travelled.moved_since_discovery(59.8586, 17.6389);
	QVERIFY2(moved > 50.0 && moved < 80.0,
	         qPrintable(QStringLiteral("distance is wrong: %1 km").arg(moved)));

	QCOMPARE(travelled.discover_stations_if_moved(59.8586, 17.6389), true);
	QVERIFY2(travelled.is_busy(), "a fix 63 km away did not discover");
}

void test_feed::the_origin_moves_only_when_an_answer_arrives() {
	/*
	 * Recording the origin at dispatch would let ONE failed request
	 * suppress discovery for good -- worse than the waste it replaces,
	 * and silent, since an empty station list looks the same as a place
	 * with no stations near it.
	 *
	 * So a discovery that has gone out but not come back must leave the
	 * origin where it was. The blocked proxy above guarantees no answer
	 * can arrive here.
	 */
	QTemporaryDir scratch;
	QVERIFY(scratch.isValid());

	bbq_wu_feed feed;
	QVERIFY(feed.open_history(scratch.filePath(QStringLiteral("h.sqlite"))));

	feed.discover_stations_at(59.3293, 18.0686);
	QVERIFY(feed.is_busy());

	double latitude = 0.0;
	double longitude = 0.0;
	QVERIFY2(!feed.history().discovery_origin(&latitude, &longitude),
	         "the origin was recorded before any answer arrived");
}

void test_feed::a_reader_who_never_moves_still_rediscovers_eventually() {
	/*
	 * WHAT THE MOVEMENT GATE MADE POSSIBLE (project.md sec 16.20).
	 *
	 * Sec 15.7.4 declines discovery when the fix has not moved a
	 * kilometre, which is right and answers only one of the two ways
	 * the list changes. The other is somebody putting up a station: a
	 * reader who never leaves their garden would never hear of it, and
	 * the gate made that permanent rather than merely likely.
	 *
	 * So the same spot is declined while the origin is fresh and
	 * accepted once it is a week old. One request in seven days against
	 * a scraped key, which is unmeasurable beside the ordinary cadence.
	 */
	QTemporaryDir scratch;
	QVERIFY(scratch.isValid());

	bbq_wu_feed feed;
	QVERIFY(feed.open_history(scratch.filePath(QStringLiteral("h.sqlite"))));

	const qint64 now = QDateTime::currentSecsSinceEpoch();

	/* Discovered here an hour ago: standing still is not a reason. */
	QVERIFY(feed.history().set_discovery_origin(59.3293, 18.0686, now - 3600));
	QCOMPARE(feed.discover_stations_if_moved(59.3293, 18.0686), false);
	QVERIFY2(!feed.is_busy(), "a fresh origin sent an unmoved fix to discovery");

	/* The same spot, eight days on. */
	QVERIFY(feed.history().set_discovery_origin(59.3293, 18.0686,
	                                            now - 8 * 24 * 3600));
	QCOMPARE(feed.discover_stations_if_moved(59.3293, 18.0686), true);
	QVERIFY2(feed.is_busy(), "a week-old list is never refreshed standing still");
}

QTEST_GUILESS_MAIN(test_feed)
#include "test_feed.moc"

/*
 * THE FETCH VERDICT, AND THE CASE IT USED TO GET WRONG (sec 16.39).
 *
 * This is what the systemd timer reads. `failures` counts bands that
 * were asked and refused, so a band never REQUESTED left it at zero,
 * left timed_out false, and the run reported "every band answered" and
 * exited 0 having asked for two of six.
 *
 * The whole table is asserted rather than the one broken cell, because
 * a fix aimed at one cell is how the other three quietly change.
 */
void test_feed::a_band_never_asked_for_is_not_a_band_that_answered() {
	/* Everything asked, everything answered. */
	QCOMPARE(bbq_fetch_verdict(0, false, true, true),
	         bbq_fetch_outcome::complete);

	/*
	 * THE DEFECT. Nothing failed and nothing timed out, because four
	 * bands were never requested -- and now is still covered by the two
	 * that were. It used to be `complete`.
	 */
	QCOMPARE(bbq_fetch_verdict(0, false, false, true),
	         bbq_fetch_outcome::partial);

	/* The same, with nothing covering now: worse, not better. */
	QCOMPARE(bbq_fetch_verdict(0, false, false, false),
	         bbq_fetch_outcome::useless);

	/* A band asked and refused, with now still covered: the ordinary
	 * quiet-station case the unit forgives by name. */
	QCOMPARE(bbq_fetch_verdict(1, false, true, true),
	         bbq_fetch_outcome::partial);

	/* A band asked and refused, and nothing describes now. */
	QCOMPARE(bbq_fetch_verdict(1, false, true, false),
	         bbq_fetch_outcome::useless);

	/*
	 * A timeout is never complete however much arrived, because a band
	 * that was asked and neither answered nor failed is what holds the
	 * loop open -- which is why `timed_out` and "never asked" are two
	 * conditions rather than one.
	 */
	QCOMPARE(bbq_fetch_verdict(0, true, true, true),
	         bbq_fetch_outcome::partial);
	QCOMPARE(bbq_fetch_verdict(0, true, true, false),
	         bbq_fetch_outcome::useless);
}

/*
 * Which question a fetched observation series answers.
 *
 * The observed band issues two requests per round and both replies
 * arrive in one handler carrying the same product. This used to be
 * decided by a member naming the day that had been asked for, and one
 * member cannot answer for two requests in flight: whichever reply
 * landed first consumed it, so the checks were swapped. A COMPLETE
 * backfill day went to the staleness branch and was announced as a
 * station that had stopped reporting.
 *
 * The numbers below are the reproduction, not invented ones. ILIDIN21
 * returned 288 rows -- a whole day at a five-minute cadence -- whose
 * newest sample sat SEVEN SECONDS before the local day it belonged to
 * ended, and that was reported as nine and a half hours of silence.
 */
void test_feed::a_finished_day_is_told_from_one_still_running() {
	/* Local midnight on 2026-09-07 in Stockholm, which is 22:00Z. */
	const qint64 today_began =
	        QDateTime::fromString(QStringLiteral("2026-09-06T22:00:00Z"),
	                              Qt::ISODate)
	                .toSecsSinceEpoch();

	const qint64 backfill_newest =
	        QDateTime::fromString(QStringLiteral("2026-09-06T21:59:53Z"),
	                              Qt::ISODate)
	                .toSecsSinceEpoch();

	const qint64 today_newest =
	        QDateTime::fromString(QStringLiteral("2026-09-07T07:19:59Z"),
	                              Qt::ISODate)
	                .toSecsSinceEpoch();

	QVERIFY2(bbq_observed_day_has_ended(backfill_newest, today_began),
	         "a complete backfill day read as still running, so it would be "
	         "judged for staleness and its station called silent");

	QVERIFY2(!bbq_observed_day_has_ended(today_newest, today_began),
	         "today's part-day read as finished, so it would be judged for "
	         "completeness -- a test it cannot fail, since it has not ended");

	/*
	 * THE BOUNDARY ITSELF, which is where a swap would hide. The first
	 * instant of a day belongs to the day in progress; the instant
	 * before it does not.
	 */
	QVERIFY(!bbq_observed_day_has_ended(today_began, today_began));
	QVERIFY(bbq_observed_day_has_ended(today_began - 1, today_began));
}

/*
 * A stamp in the future is a moved clock, not a recent fetch.
 *
 * Freshness is `now - last >= interval`, and that subtraction alone
 * stalls: a stamp ahead of the clock makes it negative, so the band is
 * never due until the clock catches up past it -- hours, if the jump
 * was hours.
 *
 * Not contrived on the platform this runs on. Android restores the RTC
 * at boot and the network corrects it afterwards, so a stamp written
 * between the two is ahead of the clock that follows it. The applet
 * would sit there refreshing nothing, with a staleness line measuring
 * from a future moment.
 *
 * Asked through `due`, which is what the scheduler actually calls.
 */
void test_feed::a_clock_that_moved_back_does_not_stall_a_band() {
	bbq_wu_feed feed;
	feed.set_station(QStringLiteral("ITEST1"));

	const qint64 now = 1780000000;
	const int product = static_cast<int>(bbq_wu_product::observed);

	/* Never attempted is due, which is the case the guard already had. */
	QVERIFY(feed.due(bbq_wu_product::observed, now));

	/* Attempted a moment ago is not due, or nothing would ever wait. */
	feed.m_attempted.insert(product, now - 5);
	QVERIFY2(!feed.due(bbq_wu_product::observed, now),
	         "a band fetched five seconds ago is due again, so nothing "
	         "throttles the provider");

	/*
	 * And the clock moved back an hour under it. The stamp is now in
	 * the future, which cannot mean the band is fresh -- nothing can
	 * have been fetched at a moment that has not happened.
	 */
	feed.m_attempted.insert(product, now + 3600);
	QVERIFY2(feed.due(bbq_wu_product::observed, now),
	         "a stamp an hour in the future reads as a recent fetch, so the "
	         "band stalls until the clock catches up to it");

	/* Far enough back to be due the ordinary way, still due. */
	feed.m_attempted.insert(product, now - 24 * 3600);
	QVERIFY(feed.due(bbq_wu_product::observed, now));
}

/*
 * A stale observed band has two causes and they want opposite
 * sentences (project.md sec 16.101).
 *
 * The station may have gone quiet, or Weather Underground's history
 * endpoint may be running behind its own current one -- which sec 16.79
 * measured at thirteen and a half hours across three stations at once,
 * so it is the provider rather than any station. The program said the
 * first whatever the cause, and a reader had to go to the provider by
 * hand to find out which.
 *
 * The station's own current reading tells them apart, and it is fetched
 * in the same round. Tested as a free function for the reason
 * bbq_observed_day_has_ended is one: this suite blocks the network, so
 * a decision only reachable through a reply is one nothing can test.
 */
void test_feed::a_late_endpoint_is_told_from_a_quiet_station() {
	const qint64 now = 1800000000;
	const qint64 half_a_day = 12 * 3600;

	/* Answering minutes ago while its history is half a day behind. */
	QVERIFY2(bbq_history_is_behind(half_a_day, now - 300, now),
	         "a station answering now was called quiet");

	/* Silent on both endpoints: the station really is quiet. */
	QVERIFY2(!bbq_history_is_behind(half_a_day, now - half_a_day, now),
	         "a genuinely quiet station was blamed on the provider");

	/*
	 * Both endpoints behind the same cache, to the second. Not evidence
	 * against the history endpoint, and the station keeps the benefit of
	 * the doubt it had before this existed.
	 */
	QVERIFY2(!bbq_history_is_behind(half_a_day, now - half_a_day, now),
	         "an equally stale current reading was read as evidence");

	/*
	 * No current reading at all, which arrives as nought. Held here
	 * knowing it passes on the arithmetic rather than on a guard --
	 * nought is further from now than any staleness this is asked about
	 * -- because the behaviour is what the caller depends on and it
	 * should not become wrong quietly.
	 */
	QVERIFY2(!bbq_history_is_behind(half_a_day, 0, now),
	         "a round with no current reading still named the provider");

	/* And a clock that moved backwards is further still. */
	QVERIFY2(!bbq_history_is_behind(half_a_day, -5000, now),
	         "a negative timestamp named the provider");

	/*
	 * The boundary, swept rather than sampled: the verdict must turn
	 * exactly where the current reading becomes fresher than the
	 * observed band is stale, and nowhere else.
	 */
	int checked = 0;
	for (qint64 fresh = 0; fresh <= 2 * half_a_day; fresh += 600) {
		const bool expected = fresh < half_a_day;
		if (bbq_history_is_behind(half_a_day, now - fresh, now) != expected) {
			QFAIL(qPrintable(
			        QStringLiteral("a current reading %1 s old gave the wrong "
			                       "verdict against a band %2 s behind")
			                .arg(fresh)
			                .arg(half_a_day)));
		}
		++checked;
	}

	QCOMPARE(checked, 145);
}

