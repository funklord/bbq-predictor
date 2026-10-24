#ifndef BBQ_WU_FETCH_VERDICT_H
#define BBQ_WU_FETCH_VERDICT_H

#include <QString>
#include <QStringList>

/*
 * What a single fetch run amounted to (project.md sec 16.39).
 *
 * Header-only and separate from fetch_once.cpp because that file has a
 * main() and so cannot be linked into the suite. The decision is the
 * part worth testing -- it is what the systemd timer reads -- and
 * keeping it here is what lets a test reach it at all.
 */
enum class bbq_fetch_outcome {
	/* Every band that exists was asked for, and answered. */
	complete,

	/*
	 * Something is missing, and the composite still describes now. The
	 * unit forgives this by name: a station going quiet for an hour is
	 * ordinary, and the other bands still land rows in the archive.
	 */
	partial,

	/* Nothing covers now, so the run stored nothing usable. */
	useless,
};

/*
 * `forecast_bands_asked` is the condition that was missing and cost
 * sec 16.39: bbq_wu_feed::refresh starts the forecast bands only when it
 * has a geocode, so without one four of six bands are never requested.
 * A band never requested does not fail and does not arrive, so it left
 * `failures` at zero and `timed_out` false -- and the run reported
 * everything answered having asked for a third of it.
 *
 * A band that WAS asked and neither answered nor failed holds the event
 * loop open until the timeout, so `timed_out` already covers that case
 * and this one is exactly the unasked.
 */
inline bbq_fetch_outcome bbq_fetch_verdict(int failures, bool timed_out,
                                           bool forecast_bands_asked,
                                           bool covers_now) {
	if (failures == 0 && !timed_out && forecast_bands_asked) {
		return bbq_fetch_outcome::complete;
	}

	/*
	 * Coverage of now decides the rest, rather than a count of what
	 * failed: it is the question the archive cares about -- something
	 * described this moment and was written down.
	 */
	return covers_now ? bbq_fetch_outcome::partial
	                  : bbq_fetch_outcome::useless;
}

/*
 * The bands a run that timed out should name as not having answered:
 * those in `order` that neither arrived nor settled without data, loudly
 * (band_failed) or quietly (band_quiet), project.md sec 19.
 *
 * Absence from the composite was the test once, and a failed band is
 * absent too, so a band the run had printed FAIL for was named as one
 * that "had not failed, had not answered yet" -- asserting the negative
 * from evidence that could not tell the two apart.
 */
inline QStringList bbq_fetch_unanswered(const QStringList &order, const QStringList &arrived,
                                        const QStringList &settled_empty) {
	QStringList unanswered;
	for (const QString &band : order) {
		if (!arrived.contains(band) && !settled_empty.contains(band)) {
			unanswered << band;
		}
	}
	return unanswered;
}

/*
 * What a run that timed out says, from that list. An empty list is a
 * real case -- every band arrived or settled and something else held the
 * loop -- and it gets a sentence of its own rather than the usual one
 * over nothing.
 */
inline QString bbq_fetch_timeout_message(int timeout_s, const QStringList &unanswered) {
	if (unanswered.isEmpty()) {
		return QStringLiteral("fetch-once: no answer within %1s, though every band "
		                      "had arrived, failed or come back empty -- what was "
		                      "outstanding was not a band\n")
		        .arg(timeout_s);
	}
	return QStringLiteral("fetch-once: no answer within %1s from: %2\n"
	                      "fetch-once: they had not failed -- they had not "
	                      "answered yet\n")
	        .arg(timeout_s)
	        .arg(unanswered.join(QLatin1Char(' ')));
}

#endif
