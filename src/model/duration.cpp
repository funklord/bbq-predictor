#include "model/duration.h"

#include <QCoreApplication>

QString bbq_describe_duration(qint64 seconds) {
	/*
	 * Negative is a clock that has moved, not a duration. Saying "-3
	 * minutes" in a sentence about how long something has been quiet
	 * would be a worse answer than the vaguer true one.
	 */
	if (seconds < 0) {
		return QCoreApplication::translate("bbq_duration", "no time at all");
	}

	const qint64 minutes = seconds / 60;

	if (minutes < 60) {
		return QCoreApplication::translate("bbq_duration", "%1 min")
		        .arg(minutes);
	}

	const qint64 hours = minutes / 60;

	/*
	 * The minutes stay while the hours are few, because "3 h 5 min" is
	 * a different afternoon from "3 h 55 min" and both round to three.
	 * Past two days they are noise: nobody plans around the minutes of
	 * something three days old.
	 */
	if (hours < 48) {
		return QCoreApplication::translate("bbq_duration", "%1 h %2 min")
		        .arg(hours)
		        .arg(minutes % 60);
	}

	return QCoreApplication::translate("bbq_duration", "%1 days")
	        .arg(hours / 24);
}
