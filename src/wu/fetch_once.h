#ifndef BBQ_WU_FETCH_ONCE_H
#define BBQ_WU_FETCH_ONCE_H

#include <QString>

#include <functional>

class bbq_composite;
class bbq_history;

/*
 * Fetch every band once, report what arrived, and exit.
 *
 * A diagnostic, not a feature. It exists because "get the data from WU"
 * has to be verifiable before anything is designed on top of it: this
 * runs the real key extraction against the real endpoints and says what
 * came back, so the shape of the data is observed rather than assumed
 * (which is the same standard sec 2.6 was held to).
 *
 * Terminates on its own. Every request settles or the whole run is cut
 * off by a wall-clock timeout, so this cannot sit waiting forever on a
 * connection that never answers.
 *
 * An empty station skips the observed band, which is the normal state
 * rather than an error (sec 2.6.6). Passing a station with no geocode
 * derives one from the station's own coordinates (sec 2.6.7).
 */
/*
 * Exits 0 when every band answered, 3 when some band failed but the
 * composite still covers now, 1 when the run stored nothing usable, and
 * 2 for a configuration error. 3 exists because a station going quiet is
 * ordinary and the other bands still land in the archive -- the systemd
 * timer forgives it by name (sec 15.6), which it could not do if a quiet
 * station and a total outage returned the same thing.
 *
 * `history_path` is the store to archive into. Empty takes the real one,
 * which is deliberate: this fetches genuine measurements, and throwing
 * away real observations because they arrived through a diagnostic
 * would be the wrong way round (project.md sec 12.11).
 */
/*
 * `after_round`, when given, runs once the round has settled, with what
 * it fetched and the store it wrote to, and its result replaces a zero
 * or partial exit when it fails. It is how --publish rides on a fetch
 * without this file knowing the publisher exists, which keeps the
 * Android build, where nothing publishes, free of it.
 */
using bbq_after_round = std::function<int(const bbq_composite &, const bbq_history &)>;

/*
 * What a round cost and found, for a caller that schedules rounds against
 * a budget (bbq-predictor-publish, sec 15.14).
 */
struct bbq_round_report {
	/* Requests sent to Weather Underground: the key page and the API. */
	int wu_requests = 0;

	/* Whether the station itself said anything: observed or current. */
	bool station_data = false;
};

struct bbq_fetch_options {
	QString history_path;
	bbq_after_round after_round;

	/*
	 * Read from vibes.se first, as the applet does (sec 15.12). The
	 * Android service sets it; the command line does not, because the
	 * command line is what the publisher runs, and a publisher reading
	 * its own output would never fetch anything new.
	 */
	bool use_server = false;

	/*
	 * Only the failures and one summary line, for a publisher running
	 * fifty stations every five minutes into syslog. The exit code is
	 * the same either way.
	 */
	bool quiet = false;

	/*
	 * Fetch only what is due by each band's interval, remembered in the
	 * store across processes (sec 15.16). The publisher's round; the
	 * diagnostic asks for everything, which is what it is for.
	 */
	bool scheduled = false;

	/* Filled in when given. */
	bbq_round_report *report = nullptr;
};

int bbq_wu_fetch_once(const QString &station_id, const QString &geocode, int timeout_s,
                      const bbq_fetch_options &options = {});

#endif
