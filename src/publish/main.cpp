#include <QCoreApplication>
#include <QDateTime>
#include <QStringList>
#include <QTextStream>

#include <memory>

#include "cli/options.h"
#include "model/settings.h"
#include "net/tls_backend.h"
#include "publish/publish.h"
#include "publish/signing_key.h"
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
	out << "       bbq-predictor-publish --generate-signing-key FILE\n";
	out << "       bbq-predictor-publish --version | --help\n";
	out << "\n";
	out << "Fetch every band once into the archive, then write the station's\n";
	out << "signed files into DIR for bbq-predictor-serve.\n";
	out << "\n";
	out << "See bbq-predictor-publish(1).\n";
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

	bbq_ensure_tls_backend();

	/*
	 * Whatever the round's outcome: the months come from the store, which
	 * a failed round leaves as it was, and the publisher keeps the last
	 * head when no band arrived.
	 */
	const bbq_after_round after_round = [dir, station, key](const bbq_composite &composite,
	                                                       const bbq_history &history) {
		QTextStream out(stdout);
		QTextStream error(stderr);
		return bbq_publish(dir, station, &composite, history, *key,
		                   QDateTime::currentSecsSinceEpoch(), out, error);
	};

	return bbq_wu_fetch_once(station, bbq_option_value(arguments, QStringLiteral("--geocode")),
	                         30,
	                         bbq_option_value(arguments, QStringLiteral("--history-path")),
	                         after_round);
}
