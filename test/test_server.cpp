#include <QTest>

#include "server/policy.h"

/*
 * When the applet gives up on vibes.se and scrapes (project.md sec
 * 15.12). Each case is one of the ways a client could fall back when it
 * should not have -- which is the failure the server exists to prevent,
 * multiplied by every client at once -- or fail to come back.
 */
/*
 * Every absolute time is offset from a real epoch. The state uses 0 for
 * "no outage", so a failure AT 0 was indistinguishable from none, and
 * the first version of these cases lost its first failure that way --
 * a restart storm starting at 0 passed for the wrong reason.
 */
static qint64 t(qint64 offset_s) {
	return 1760000000 + offset_s;
}

class test_server : public QObject {
	Q_OBJECT

private slots:
	void a_server_that_answers_is_never_abandoned();
	void a_sustained_outage_with_the_network_up_falls_back();
	void few_failures_over_a_long_window_do_not_fall_back();
	void many_failures_in_a_short_window_do_not_fall_back();
	void a_closed_app_is_not_a_witness();
	void no_network_is_not_an_outage();
	void network_seen_only_before_the_outage_does_not_count();
	void one_success_ends_the_fallback();
	void retries_back_off_and_cap();
	void a_retry_waits_its_turn();
	void an_old_head_is_stale_and_a_future_one_is_not();

private:
	/* Fail every `step_s` from offset `from_utc` for `count` attempts. */
	static void fail(bbq_server_state &state, qint64 from_utc, qint64 step_s,
	                 int count);
};

void test_server::fail(bbq_server_state &state, qint64 from_utc,
                       qint64 step_s, int count) {
	for (int i = 0; i < count; ++i) {
		bbq_server_record_failure(state, t(from_utc + i * step_s));
	}
}

void test_server::a_server_that_answers_is_never_abandoned() {
	bbq_server_state state;
	QVERIFY(!bbq_server_should_scrape(state));

	bbq_server_record_success(state, t(1000));
	bbq_server_record_network_ok(state, t(1000));
	QVERIFY(!bbq_server_should_scrape(state));
}

void test_server::a_sustained_outage_with_the_network_up_falls_back() {
	bbq_server_state state;

	/* Every 5 minutes for 35 minutes: 8 failures spanning 35 min. */
	fail(state, 0, 5 * 60, 8);
	bbq_server_record_network_ok(state, t(3600));

	QVERIFY(bbq_server_should_scrape(state));
}

void test_server::few_failures_over_a_long_window_do_not_fall_back() {
	bbq_server_state state;

	/* Opened twice a day for two days: four failures. */
	fail(state, 0, 12 * 3600, 4);
	bbq_server_record_network_ok(state, t(3600));

	QVERIFY(!bbq_server_should_scrape(state));
}

void test_server::many_failures_in_a_short_window_do_not_fall_back() {
	bbq_server_state state;

	/* A restart storm: fifty failures in under twenty minutes. */
	fail(state, 0, 20, 50);
	bbq_server_record_network_ok(state, t(30));

	QVERIFY(!bbq_server_should_scrape(state));
}

void test_server::a_closed_app_is_not_a_witness() {
	bbq_server_state state;

	/*
	 * Eight failures in the first ten minutes, then nothing for two days
	 * because nobody opened the app. The span the client SAW is ten
	 * minutes, however much time has passed.
	 */
	fail(state, 0, 75, 8);
	bbq_server_record_network_ok(state, t(600));

	QVERIFY(!bbq_server_should_scrape(state));
}

void test_server::no_network_is_not_an_outage() {
	bbq_server_state state;

	/* A long flight: the server failed throughout, and so did everything. */
	fail(state, 0, 5 * 60, 20);

	QVERIFY(!bbq_server_should_scrape(state));
}

void test_server::network_seen_only_before_the_outage_does_not_count() {
	bbq_server_state state;

	bbq_server_record_network_ok(state, t(100));
	fail(state, 1000, 5 * 60, 20);

	QVERIFY(!bbq_server_should_scrape(state));

	/* And once something answers during it, the outage is the server's. */
	bbq_server_record_network_ok(state, t(5000));
	QVERIFY(bbq_server_should_scrape(state));
}

void test_server::one_success_ends_the_fallback() {
	bbq_server_state state;

	fail(state, 0, 5 * 60, 20);
	bbq_server_record_network_ok(state, t(600));
	QVERIFY(bbq_server_should_scrape(state));

	bbq_server_record_success(state, t(20 * 5 * 60));
	QVERIFY(!bbq_server_should_scrape(state));
	QCOMPARE(state.failures, 0);
	QCOMPARE(state.outage_since_utc, qint64(0));
	QVERIFY(bbq_server_may_attempt(state, t(20 * 5 * 60)));
}

void test_server::retries_back_off_and_cap() {
	QCOMPARE(bbq_server_retry_delay(0), qint64(0));
	QCOMPARE(bbq_server_retry_delay(1), qint64(60));
	QCOMPARE(bbq_server_retry_delay(2), qint64(120));
	QCOMPARE(bbq_server_retry_delay(3), qint64(300));
	QCOMPARE(bbq_server_retry_delay(4), qint64(300));
	QCOMPARE(bbq_server_retry_delay(500), qint64(300));

	/* The cap holds even below the fixed steps. */
	bbq_server_policy tight;
	tight.max_retry_s = 90;
	QCOMPARE(bbq_server_retry_delay(2, tight), qint64(90));
}

/*
 * Two hours, the holder's figure: a head generated two hours ago still
 * counts, one a second older does not. One stamped ahead of now -- this
 * clock behind the server's -- is not stale, since being early is no
 * evidence of being frozen.
 */
void test_server::an_old_head_is_stale_and_a_future_one_is_not() {
	QVERIFY(!bbq_server_head_is_stale(t(0), t(2 * 3600)));
	QVERIFY(bbq_server_head_is_stale(t(0), t(2 * 3600 + 1)));
	QVERIFY(!bbq_server_head_is_stale(t(600), t(0)));

	bbq_server_policy patient;
	patient.stale_head_s = 6 * 3600;
	QVERIFY(!bbq_server_head_is_stale(t(0), t(3 * 3600), patient));
}

void test_server::a_retry_waits_its_turn() {
	bbq_server_state state;
	QVERIFY(bbq_server_may_attempt(state, t(0)));

	bbq_server_record_failure(state, t(1000));
	QVERIFY(!bbq_server_may_attempt(state, t(1059)));
	QVERIFY(bbq_server_may_attempt(state, t(1060)));
}

QTEST_GUILESS_MAIN(test_server)
#include "test_server.moc"
