#ifndef BBQ_SERVER_POLICY_H
#define BBQ_SERVER_POLICY_H

#include <QtGlobal>

/*
 * When the applet stops waiting for vibes.se and scrapes (project.md
 * sec 15.12).
 *
 * The server exists so that clients do not scrape, and a client that
 * falls back at the first refused connection would scrape every time
 * the server restarts, every time a phone is in a tunnel and every time
 * a laptop opens on a hotel network that has not let it out yet. So the
 * fallback needs three things at once:
 *
 *   - the server has failed often, not once;
 *   - those failures were SEEN across a long window, not merely a first
 *     failure and a "now" with the app closed in between -- a phone
 *     that failed once on a plane and was opened two days later has
 *     observed one failure, not two days of outage;
 *   - something else answered over HTTPS during the outage, so the
 *     network was up and the server was what was missing. Without that
 *     a traveller with no connection falls back the moment one returns.
 *
 * Plain data and free functions, because the suite blocks the network
 * and a decision reachable only through a reply is one nothing can test.
 */

struct bbq_server_policy {
	/* How long failures must have been seen before scraping. */
	qint64 give_up_s = 30 * 60;

	/* And how many of them, so a long window with two failures is not enough. */
	int min_failures = 6;

	/* The retry interval grows to this and stays there. */
	qint64 max_retry_s = 5 * 60;

	/*
	 * A server whose newest head is older than this has failed, though it
	 * answered (holder, 2026-10-25). Answering is not enough: a publisher
	 * that stopped leaves the daemon serving its last files, and a daemon
	 * taken over can replay the last valid head for ever. Either froze a
	 * client on old bands, every round a success, and the fallback above
	 * never came. Two hours is about 24 of the publisher's five-minute
	 * rounds, and a round only counts toward that fallback, so a short
	 * provider outage at the server still changes nothing here.
	 */
	qint64 stale_head_s = 2 * 3600;
};

struct bbq_server_state {
	/* The last time the server answered with something usable; 0 never. */
	qint64 last_success_utc = 0;

	/* The first failure after the last success; 0 while it is answering. */
	qint64 outage_since_utc = 0;

	/* The most recent failure. */
	qint64 last_failure_utc = 0;

	/* Failures since outage_since_utc. */
	int failures = 0;

	/* The last time another host answered over HTTPS; 0 never. */
	qint64 network_ok_utc = 0;

	/* Do not ask the server again before this. */
	qint64 next_attempt_utc = 0;
};

void bbq_server_record_success(bbq_server_state &state, qint64 now_utc);

void bbq_server_record_failure(bbq_server_state &state, qint64 now_utc,
                               const bbq_server_policy &policy = {});

/* Something other than the server answered, so the network is up. */
void bbq_server_record_network_ok(bbq_server_state &state, qint64 now_utc);

/* Seconds to wait after the n-th consecutive failure, n counting from 1. */
qint64 bbq_server_retry_delay(int failures, const bbq_server_policy &policy = {});

bool bbq_server_may_attempt(const bbq_server_state &state, qint64 now_utc);

/*
 * Whether a head generated at `generated_utc` is too old to count as the
 * server answering. One stamped ahead of `now_utc` -- a clock behind the
 * server's -- is not stale.
 */
bool bbq_server_head_is_stale(qint64 generated_utc, qint64 now_utc,
                              const bbq_server_policy &policy = {});

/*
 * Whether the server has been absent long enough, while the network was
 * demonstrably up, that the applet should scrape for itself.
 *
 * The server is still asked first on every round after this turns true,
 * and one success turns it false again.
 */
bool bbq_server_should_scrape(const bbq_server_state &state,
                              const bbq_server_policy &policy = {});

#endif
