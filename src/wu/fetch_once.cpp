#include "wu/fetch_once.h"

#include <QDate>
#include <QDateTime>
#include <QEventLoop>
#include <QTextStream>
#include <QTimeZone>
#include <QTimer>

#include "model/composite.h"
#include "wu/feed.h"

namespace {

QString stamp(qint64 when_utc) {
	if (when_utc == 0) {
		return QStringLiteral("--");
	}

	const QTimeZone utc(QTimeZone::UTC);
	const QDateTime when = QDateTime::fromSecsSinceEpoch(when_utc, utc);
	return when.toString(QStringLiteral("MM-dd HH:mm"));
}

int count_gaps(const bbq_series &series) {
	int gaps = 0;

	for (std::size_t i = 0; i < series.size(); ++i) {
		if (series.has_gap_after(i)) {
			++gaps;
		}
	}

	return gaps;
}

QString describe_series(const bbq_series &series) {
	if (series.is_empty()) {
		return QStringLiteral("no samples");
	}

	QString text = QString::number(series.size());
	text += QStringLiteral(" samples, step ");
	text += QString::number(series.nominal_step_s());
	text += QStringLiteral("s, ");
	text += stamp(series.begin_utc());
	text += QStringLiteral("Z..");
	text += stamp(series.end_utc());
	text += QStringLiteral("Z, gaps ");
	text += QString::number(count_gaps(series));
	return text;
}

QString describe_reading(const bbq_composite &composite, qint64 when_utc) {
	const bbq_reading reading = composite.at(when_utc);
	if (!reading.is_valid()) {
		return QStringLiteral("no band covers it");
	}

	QString text;

	if (reading.sample->temperature.has_value()) {
		text += QString::number(*reading.sample->temperature, 'f', 1);
		text += QStringLiteral(" C");
	} else {
		text += QStringLiteral("-- C");
	}

	text += QStringLiteral(", ");

	if (reading.sample->precip_rate.has_value()) {
		text += QString::number(*reading.sample->precip_rate, 'f', 2);
		text += QStringLiteral(" mm/h");
	} else {
		text += QStringLiteral("-- mm/h");
	}

	text += QStringLiteral("   from ");
	text += QString::fromLatin1(bbq_band_name(reading.series->band()));
	text += QStringLiteral(" (");
	text += reading.series->provider();
	text += QStringLiteral(")");
	return text;
}

/*
 * Walk the whole composite and report every instant no band covers.
 *
 * This exists because a single probe at "now" proves nothing: sec 3.9's
 * hole is intermittent, so a run that happens to land inside covered
 * time reports success exactly as loudly as a run that is genuinely
 * whole. Scanning says where the holes ARE, which is a claim that can
 * be checked rather than a green light that might be luck.
 *
 * Bounded by construction: it steps a fixed stride from the composite's
 * first sample to its last, so the iteration count is the coverage
 * divided by the stride and nothing about the data can extend it.
 */
void report_holes(QTextStream &out, const bbq_composite &composite, qint64 now_utc) {
	const qint64 begin = composite.begin_utc();
	const qint64 end = composite.end_utc();
	if (begin == 0 || end <= begin) {
		out << "  holes      nothing to scan\n";
		return;
	}

	/*
	 * A stride, so every boundary reported below carries up to this
	 * much error. Fine for locating holes; useless for judging whether
	 * one instant in particular is covered -- see now_covered.
	 */
	const qint64 stride = 60;
	int holes = 0;
	qint64 hole_start = 0;
	qint64 worst = 0;

	for (qint64 t = begin; t <= end; t += stride) {
		const bool covered = composite.at(t).is_valid();

		if (!covered && hole_start == 0) {
			hole_start = t;
		}

		if (covered && hole_start != 0) {
			const qint64 width = t - hole_start;
			++holes;
			if (width > worst) {
				worst = width;
			}
			out << "  hole       " << stamp(hole_start) << "Z..";
			out << stamp(t) << "Z  (" << (width / 60) << " min)\n";
			hole_start = 0;
		}
	}

	out << "  holes      " << holes;
	if (holes > 0) {
		out << ", worst " << (worst / 60) << " min";
	}
	out << "\n";

	/*
	 * Asked directly rather than read off the scan above.
	 *
	 * Inferring it from the stepped boundaries said IN A HOLE while the
	 * composite was answering "now" perfectly well from the current
	 * band: now had landed in the sub-stride sliver between the last
	 * uncovered step and the real edge of coverage. That is a check
	 * being wrong about code that was right, and the fix belongs in the
	 * check.
	 */
	const bool now_covered = composite.at(now_utc).is_valid();

	out << "  now        ";
	if (now_covered) {
		out << "covered\n";
	} else {
		out << "IN A HOLE -- sec 3.9 is not fixed\n";
	}

	/*
	 * The claim sec 3.9's fix actually makes, checked on its own.
	 *
	 * "now is covered" above can be true by luck -- the nowcast may
	 * happen to reach back over this instant on this particular run.
	 * What the current band promises is narrower and always testable:
	 * that IT covers now, so that when the other bands leave a hole
	 * there, something honest is sitting in it.
	 */
	const bbq_series *current = composite.band(bbq_band::current);
	out << "  current    ";
	if (current == nullptr || current->is_empty()) {
		out << "absent -- nothing anchors the present\n";
	} else if (current->at(now_utc) != nullptr) {
		out << "covers now, so the hole at now cannot reopen\n";
	} else {
		out << "present but does NOT cover now -- it is stale\n";
	}
}

} // namespace

int bbq_wu_fetch_once(const QString &station_id, const QString &geocode,
                      int timeout_s) {
	QTextStream out(stdout);
	QTextStream error(stderr);

	if (station_id.isEmpty() && geocode.isEmpty()) {
		error << "fetch-once: nothing configured.\n";
		error << "fetch-once:   Give --station ID, or --geocode LAT,LON.\n";
		return 2;
	}

	/*
	 * Driven through bbq_wu_feed rather than through the WU client.
	 *
	 * This diagnostic predated the feed and drove that client directly,
	 * so it exercised four Weather Underground products and neither of
	 * the other two providers -- a check that no longer inspected what
	 * the application does, which is the shape of vacuous pass this
	 * project keeps refusing elsewhere.
	 *
	 * It also carried its own copy of the band dispatch and the geocode
	 * derivation. Two copies of an orchestration is two things to keep
	 * in step, and the one nobody runs is the one that drifts.
	 */
	bbq_wu_feed feed;
	feed.set_station(station_id);

	if (!geocode.isEmpty()) {
		const QStringList parts = geocode.split(QLatin1Char(','));
		if (parts.size() != 2) {
			error << "fetch-once: --geocode wants LAT,LON\n";
			return 2;
		}
		feed.set_geocode(parts.at(0).toDouble(), parts.at(1).toDouble());
	}

	int failures = 0;
	QEventLoop loop;

	QObject::connect(&feed, &bbq_wu_feed::band_failed, &loop,
	                 [&](const QString &band, const QString &reason) {
		error << QStringLiteral("  %1").arg(band, -10);
		error << QStringLiteral(" FAIL  ");
		error << reason;
		error << "\n";
		error.flush();
		++failures;
	});

	QObject::connect(&feed, &bbq_wu_feed::settled, &loop, &QEventLoop::quit);

	/*
	 * The bound that holds whatever happens. A request that never
	 * answers settles nothing, so the loop needs an end of its own --
	 * and it lives here rather than in whatever invokes the binary,
	 * because a wrapper only guards the way somebody did not run it.
	 */
	bool timed_out = false;
	QTimer::singleShot(timeout_s * 1000, &loop, [&]() {
		timed_out = true;
		loop.quit();
	});

	out << "fetch-once: fetching every band once, through the feed\n";
	out.flush();

	feed.refresh();
	loop.exec();

	if (timed_out) {
		error << QStringLiteral("fetch-once: timed out after %1s\n").arg(timeout_s);
		++failures;
	}

	const bbq_composite &composite = feed.composite();

	/*
	 * Reported from the composite rather than as each band lands, so
	 * every provider is described the same way whoever supplied it.
	 */
	const bbq_band order[] = {
		bbq_band::observed,  bbq_band::current, bbq_band::nowcast_fine,
		bbq_band::nowcast,   bbq_band::extended, bbq_band::hourly,
	};

	for (bbq_band band : order) {
		const bbq_series *series = composite.band(band);
		if (series == nullptr) {
			continue;
		}

		out << QStringLiteral("  %1")
		                .arg(QString::fromLatin1(bbq_band_name(band)), -10);
		out << QStringLiteral(" %1  ").arg(series->provider(), -13);
		out << describe_series(*series);
		out << "\n";
	}

	out.flush();

	/*
	 * What the composite makes of it. The three probes below are the
	 * point: the same query at three instants should be answered by
	 * three different bands, which is declared precedence (sec 3.3)
	 * doing its job, and each answer says which band gave it, which is
	 * provenance surviving resolution (sec 3.4).
	 */
	const qint64 now = QDateTime::currentSecsSinceEpoch();

	out << "\ncomposite:\n";
	out << "  coverage   " << stamp(composite.begin_utc()) << "Z..";
	out << stamp(composite.end_utc()) << "Z\n";

	const std::vector<bbq_band> missing = composite.missing_bands();
	out << "  missing    ";
	if (missing.empty()) {
		out << "none";
	} else {
		for (bbq_band band : missing) {
			out << bbq_band_name(band) << " ";
		}
	}
	out << "\n";

	report_holes(out, composite, now);

	out << "  at now     " << describe_reading(composite, now) << "\n";
	out << "  at +3h     " << describe_reading(composite, now + 3 * 3600) << "\n";
	out << "  at +12h    " << describe_reading(composite, now + 12 * 3600) << "\n";
	out << "  at -2h     " << describe_reading(composite, now - 2 * 3600) << "\n";

	if (failures > 0) {
		error << QStringLiteral("fetch-once: %1 band(s) failed\n").arg(failures);
		return 1;
	}

	out << "fetch-once: every band answered\n";
	return 0;
}
