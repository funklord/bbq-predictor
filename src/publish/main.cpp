#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QStringList>
#include <QTextStream>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <functional>
#include <memory>

#include "cli/options.h"
#include "model/composite.h"
#include "model/settings.h"
#include "net/tls_backend.h"
#include "publish/demand.h"
#include "publish/publish.h"
#include "publish/signing_key.h"
#include "store/history.h"
#include "wu/fetch_once.h"

/*
 * bbq-predictor-publish -- fetch every band once and publish it signed
 * (project.md sec 15.12, 15.13).
 *
 * The applet's fetching half without its window: core.pri's sources and
 * the publisher's own, linked against QtCore, QtNetwork and QtSql and
 * nothing that draws. It runs on a server with no display, and the
 * applet's --publish dragged the desktop's libraries onto that server for
 * a program that never opened a window there.
 */

#ifndef BBQ_VERSION_STRING
#define BBQ_VERSION_STRING "unknown"
#endif

namespace {

void print_usage(QTextStream &out) {
	out << "usage: bbq-predictor-publish --dir DIR --signing-key FILE"
	       " [--station ID]\n";
	out << "                             [--geocode LAT,LON]"
	       " [--history-path FILE]\n";
	out << "                             [--demand FILE]\n";
	out << "       bbq-predictor-publish --generate-signing-key FILE\n";
	out << "       bbq-predictor-publish --version | --help\n";
	out << "\n";
	out << "Fetch every band once into the archive, then write the station's\n";
	out << "signed files into DIR for bbq-predictor-serve. With --demand, also\n";
	out << "the stations clients asked bbq-predictor-serve for, within a budget.\n";
	out << "\n";
	out << "See bbq-predictor-publish(1).\n";
}

/*
 * The demand file's contents, and the file emptied (sec 15.14). Absent is
 * no demand rather than an error: the daemon may not be running yet. A
 * line the daemon appends between the read and the truncation is lost,
 * and costs only an hour, since the station is asked about again.
 */
bool take_demand(const QString &path, QByteArray *text, QString *why) {
	const QByteArray name = QFile::encodeName(path);
	const int fd = ::open(name.constData(), O_RDWR | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT) {
			return true;
		}
		*why = QStringLiteral("%1: %2").arg(path, QString::fromLocal8Bit(strerror(errno)));
		return false;
	}

	struct stat file;
	if (::fstat(fd, &file) != 0 || !S_ISREG(file.st_mode)) {
		::close(fd);
		*why = QStringLiteral("%1: not a regular file").arg(path);
		return false;
	}

	/* The daemon stops at its cap; a little over it is read, and no more. */
	const qint64 limit = bbq_demand_read_max;
	text->resize(int(limit));
	qint64 got = 0;
	while (got < limit) {
		const ssize_t n = ::read(fd, text->data() + got, size_t(limit - got));
		if (n <= 0) {
			break;
		}
		got += n;
	}
	text->resize(int(got));

	const bool emptied = ::ftruncate(fd, 0) == 0;
	::close(fd);
	if (!emptied) {
		*why = QStringLiteral("%1: cannot empty it").arg(path);
		return false;
	}
	return true;
}

/*
 * One run over every station the demand says to fetch now, within the
 * hour's budget and a deadline that keeps the run inside the timer's five
 * minutes. The schedule lives in the archive, so it outlives the process.
 *
 * Exits 0 when every station fetched answered fully, 3 when something was
 * partial or a station said nothing, and 1 when the configured station
 * stored nothing usable or a publish failed. A station somebody asked for
 * going quiet is ordinary -- the unit forgives 3 and fails on 1 -- so it
 * never makes the run a failure on its own.
 */
int publish_demanded(const QString &station, const QString &geocode,
                         const QString &history_path, const QString &demand_path,
                         const std::function<bbq_after_round(const QString &, bool)> &publishing) {
	QTextStream out(stdout);
	QTextStream error(stderr);
	const qint64 started = QDateTime::currentSecsSinceEpoch();

	bbq_history store;
	if (!store.open(history_path)) {
		error << "publish: cannot open the archive: " << store.last_error() << "\n";
		return 1;
	}

	QByteArray text;
	QString why;
	if (!take_demand(demand_path, &text, &why)) {
		error << "publish: demand: " << why << "\n";
	}

	bbq_demand demand = bbq_demand_parse(store.state_value(QStringLiteral("demand")));
	int turned_away = 0;
	const int admitted = bbq_demand_absorb(demand, text, started, &turned_away);
	bbq_demand_expire(demand, started);
	bbq_demand_configure(demand, station, started);

	/*
	 * Saved before the first fetch, not only after the last. The file
	 * was emptied as it was read, so until this lands the archive is
	 * the only copy of what clients asked for -- and a round runs for
	 * minutes with nothing bounding it from outside, so a crash or a
	 * shutdown in the middle lost every request it had taken.
	 */
	if (!store.set_state_value(QStringLiteral("demand"), bbq_demand_serialize(demand))) {
		error << "publish: cannot save the demand: " << store.last_error() << "\n";
		return 1;
	}

	const std::vector<QString> due = bbq_demand_due(demand, started);
	std::vector<bbq_demand_result> results;
	int configured_code = 0;
	int imperfect = 0;
	int publish_failed = 0;

	for (const QString &id : due) {
		if (QDateTime::currentSecsSinceEpoch() - started > bbq_demand_deadline_s) {
			break;
		}

		bbq_round_report report;
		bbq_fetch_options options;
		options.history_path = history_path;
		options.after_round = publishing(id, true);
		options.quiet = true;
		options.scheduled = true;
		options.report = &report;

		const int code = bbq_wu_fetch_once(id, id == station ? geocode : QString(), 30, options);
		results.push_back({id, report.wu_requests, report.station_data});

		if (id == station) {
			configured_code = code;
		}
		imperfect += code == 1 || code == 3 ? 1 : 0;
		publish_failed += code != 0 && code != 1 && code != 3 ? 1 : 0;
	}

	const qint64 finished = QDateTime::currentSecsSinceEpoch();
	bbq_demand_record(demand, results, finished);
	/*
	 * A failure here forgets the hour's spending as well as the schedule,
	 * and the next run would spend the budget again: a failed run, not a
	 * quiet one.
	 */
	if (!store.set_state_value(QStringLiteral("demand"), bbq_demand_serialize(demand))) {
		error << "publish: cannot save the demand: " << store.last_error() << "\n";
		return 1;
	}

	out << "publish: " << results.size() << " of " << due.size() << " due station(s) fetched, "
	    << demand.stations.size() << " wanted, " << admitted << " newly asked for, ";
	if (turned_away > 0) {
		out << turned_away << " turned away at the ceiling of " << bbq_demand_max_stations
		    << ", ";
	}
	out << bbq_demand_spent(demand, finished) << " of " << bbq_demand_budget_per_hour
	    << " WU requests spent this hour, every " << bbq_demand_interval(demand) / 60
	    << " min\n";

	if (publish_failed > 0 || configured_code == 1) {
		return 1;
	}
	return imperfect > 0 ? 3 : 0;
}

} // namespace

int main(int argc, char *argv[]) {
	QCoreApplication app(argc, argv);

	/*
	 * The applet's name, not this program's: QSettings and the default
	 * archive path are derived from it, and a publisher that looked
	 * somewhere else for a station the applet had configured would be a
	 * second configuration nobody asked for.
	 */
	QCoreApplication::setApplicationName(QStringLiteral("bbq-predictor"));
	QCoreApplication::setApplicationVersion(QStringLiteral(BBQ_VERSION_STRING));

	QTextStream out(stdout);
	QTextStream error(stderr);
	const QStringList arguments = QCoreApplication::arguments();

	if (arguments.contains(QStringLiteral("--version"))) {
		out << "bbq-predictor-publish " << BBQ_VERSION_STRING << "\n";
		out << "Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>\n";
		return 0;
	}

	if (arguments.contains(QStringLiteral("--help"))) {
		print_usage(out);
		return 0;
	}

	/*
	 * Made once, as the account that publishes; the public half it prints
	 * is what the applet is built with. Never overwrites: a lost key
	 * strands every client built with its public half.
	 */
	if (arguments.contains(QStringLiteral("--generate-signing-key"))) {
		const QString path =
		        bbq_option_value(arguments, QStringLiteral("--generate-signing-key"));
		if (path.isEmpty()) {
			error << "generate-signing-key: give the file to create\n";
			return 2;
		}

		bbq_signing_key key;
		QString why;
		if (!key.generate(path, &why)) {
			error << "generate-signing-key: " << why << "\n";
			return 1;
		}
		out << "signing key written to " << path << ", readable by its owner alone\n";
		out << "public key: " << QString::fromLatin1(key.public_key().toHex()) << "\n";
		return 0;
	}

	const QString dir = bbq_option_value(arguments, QStringLiteral("--dir"));
	const QString key_path = bbq_option_value(arguments, QStringLiteral("--signing-key"));
	if (dir.isEmpty() || key_path.isEmpty()) {
		error << "publish: --dir DIR and --signing-key FILE are both needed\n";
		print_usage(error);
		return 2;
	}

	/*
	 * The key is loaded BEFORE fetching, so a bad key costs nothing but
	 * its message, not a round of somebody else's quota.
	 */
	auto key = std::make_shared<bbq_signing_key>();
	QString why;
	if (!key->load(key_path, &why)) {
		error << "publish: " << why << "\n";
		return 2;
	}

	QString station = bbq_option_value(arguments, QStringLiteral("--station"));
	if (station.isEmpty()) {
		station = bbq_settings::station();
	}
	const QString geocode = bbq_option_value(arguments, QStringLiteral("--geocode"));
	const QString history_path = bbq_option_value(arguments, QStringLiteral("--history-path"));
	const QString demand_path = bbq_option_value(arguments, QStringLiteral("--demand"));

	bbq_ensure_tls_backend();

	/*
	 * Whatever the round's outcome: the months come from the store, which
	 * a failed round leaves as it was, and the publisher keeps the last
	 * head when no band arrived. Quiet drops the publish's own line along
	 * with the round's report.
	 */
	const auto publishing = [dir, key](const QString &id, bool quiet) -> bbq_after_round {
		return [dir, key, id, quiet](const bbq_composite &composite, const bbq_history &history) {
			/*
			 * Nothing to publish is not a publish. A station id a
			 * stranger asked for that never answers would otherwise
			 * leave a directory behind for every one of them (sec 15.14).
			 */
			bool any_band = false;
			for (bbq_band band : {bbq_band::observed, bbq_band::current, bbq_band::nowcast_fine,
			                      bbq_band::nowcast, bbq_band::extended, bbq_band::hourly}) {
				const bbq_series *series = composite.band(band);
				any_band = any_band || (series && !series->is_empty());
			}
			if (!any_band && history.observation_months(id).empty()) {
				return 0;
			}

			QString discarded;
			QTextStream out(stdout);
			if (quiet) {
				out.setString(&discarded);
			}
			QTextStream error(stderr);
			return bbq_publish(dir, id, &composite, history, *key,
			                   QDateTime::currentSecsSinceEpoch(), out, error);
		};
	};

	if (demand_path.isEmpty()) {
		bbq_fetch_options options;
		options.history_path = history_path;
		options.after_round = publishing(station, false);
		options.scheduled = true;
		return bbq_wu_fetch_once(station, geocode, 30, options);
	}

	return publish_demanded(station, geocode, history_path, demand_path, publishing);
}
