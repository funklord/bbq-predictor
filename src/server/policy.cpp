#include "server/policy.h"

#include <algorithm>

void bbq_server_record_success(bbq_server_state &state, qint64 now_utc) {
	state.last_success_utc = now_utc;
	state.outage_since_utc = 0;
	state.last_failure_utc = 0;
	state.failures = 0;
	state.next_attempt_utc = 0;
}

void bbq_server_record_failure(bbq_server_state &state, qint64 now_utc,
                               const bbq_server_policy &policy) {
	if (state.outage_since_utc == 0) {
		state.outage_since_utc = now_utc;
	}

	state.last_failure_utc = std::max(state.last_failure_utc, now_utc);
	++state.failures;
	state.next_attempt_utc =
	        now_utc + bbq_server_retry_delay(state.failures, policy);
}

void bbq_server_record_network_ok(bbq_server_state &state, qint64 now_utc) {
	state.network_ok_utc = std::max(state.network_ok_utc, now_utc);
}

qint64 bbq_server_retry_delay(int failures, const bbq_server_policy &policy) {
	/*
	 * A minute, then two, five, ten, and the cap. Short at first because
	 * the commonest failure is a restart that is over in seconds; capped
	 * because a client that backs off to hours would also notice the
	 * server's return hours late.
	 */
	static const qint64 steps[] = {60, 120, 300, 600};
	const int count = int(sizeof steps / sizeof steps[0]);

	if (failures <= 0) {
		return 0;
	}

	if (failures > count) {
		return policy.max_retry_s;
	}

	return std::min(steps[failures - 1], policy.max_retry_s);
}

bool bbq_server_may_attempt(const bbq_server_state &state, qint64 now_utc) {
	return now_utc >= state.next_attempt_utc;
}

bool bbq_server_should_scrape(const bbq_server_state &state,
                              const bbq_server_policy &policy) {
	if (state.outage_since_utc == 0) {
		return false;
	}

	if (state.failures < policy.min_failures) {
		return false;
	}

	/*
	 * Seen failing across the window: the span between the first and
	 * the latest OBSERVED failure, never between the first failure and
	 * now. The clock running while the app is closed is not evidence.
	 */
	if (state.last_failure_utc - state.outage_since_utc < policy.give_up_s) {
		return false;
	}

	/* The network answered during the outage, so the server is what is missing. */
	return state.network_ok_utc >= state.outage_since_utc;
}
