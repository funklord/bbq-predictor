#include <QApplication>
#include <QPixmap>
#include <QTimer>
#include <QStringList>
#include <QTextStream>

#include "ui/main_window.h"
#include "ui/tray_icon.h"
#include "wu/feed.h"
#include "wu/fetch_once.h"

/*
 * bbqpredictor -- a Qt Widgets weather applet for the tray and a window.
 *
 * The high-resolution temperature and rain graphs are the point of the
 * program; everything here is the shell around them. See project.md.
 *
 * Nothing is fetched yet. This binary comes up, shows an empty graph that
 * says it is empty, and sits in the tray.
 */

#ifndef BBQ_VERSION_STRING
/*
 * Defined by bbqpredictor.pro from the VERSION file. The fallback keeps a
 * bare compile working while making it obvious the build did not go
 * through `make`, rather than quietly claiming a version it does not know.
 */
#define BBQ_VERSION_STRING "unknown"
#endif

namespace {

void print_usage(QTextStream &out) {
	out << "usage: bbqpredictor [--version] [--help]\n";
	out << "       bbqpredictor --fetch-once [--station ID]";
	out << " [--geocode LAT,LON]\n";
	out << "\n";
	out << "A weather applet for the system tray and a window.\n";
	out << "\n";
	out << "  --version      print the version and exit\n";
	out << "  --help         print this and exit\n";
	out << "  --fetch-once   fetch every band once, report what arrived,\n";
	out << "                 and exit. A diagnostic, not a feature.\n";
	out << "  --station ID   the pinned weather station, e.g. ISTOCK822\n";
	out << "  --geocode      LAT,LON for the forecast bands; derived from\n";
	out << "                 the station when omitted\n";
	out << "  --shot FILE    fetch, render the window to a PNG, and exit.\n";
	out << "                 A diagnostic: looking at the picture is how\n";
	out << "                 layout defects actually get found.\n";
}

/* The value after `name`, or empty when absent or last. */
QString option_value(const QStringList &arguments, const QString &name) {
	const int index = arguments.indexOf(name);
	if (index < 0 || index + 1 >= arguments.size()) {
		return QString();
	}

	return arguments.at(index + 1);
}

} // namespace

int main(int argc, char *argv[]) {
	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("bbqpredictor"));
	QApplication::setApplicationVersion(QStringLiteral(BBQ_VERSION_STRING));

	/*
	 * Answered before anything is constructed, so both work without a
	 * display -- which is what makes them usable from a build chroot or a
	 * CI job that has no X or Wayland session.
	 */
	QTextStream out(stdout);
	const QStringList arguments = QApplication::arguments();

	if (arguments.contains(QStringLiteral("--version"))) {
		out << "bbqpredictor " << BBQ_VERSION_STRING << "\n";
		return 0;
	}

	if (arguments.contains(QStringLiteral("--help"))) {
		print_usage(out);
		return 0;
	}

	/*
	 * Answered before any widget is built, so it runs headless -- in a
	 * build chroot, over ssh, or anywhere without a display. Bounded by
	 * its own timeout rather than by whatever invokes it.
	 */
	if (arguments.contains(QStringLiteral("--fetch-once"))) {
		return bbq_wu_fetch_once(
		        option_value(arguments, QStringLiteral("--station")),
		        option_value(arguments, QStringLiteral("--geocode")), 30);
	}

	/*
	 * A tray applet must not exit when its window is closed -- closing
	 * the window is how it gets put away, not how it is quit.
	 *
	 * That holds only where there IS a tray. Without one the window is
	 * the entire user interface, and keeping the process alive after it
	 * closes leaves something running with no way to see it, reach it or
	 * quit it short of kill. So the answer follows the tray (sec 4.1),
	 * and is decided before the window is shown.
	 */
	QApplication::setQuitOnLastWindowClosed(!bbq_tray_icon::is_available());

	const QString station = option_value(arguments, QStringLiteral("--station"));
	const QString geocode = option_value(arguments, QStringLiteral("--geocode"));

	bbq_main_window window;
	bbq_tray_icon tray;

	QObject::connect(&tray, &bbq_tray_icon::toggle_requested,
	                 &window, &bbq_main_window::toggle_visibility);

	/*
	 * Said out loud on the session that cannot show a tray, rather than
	 * discovered as an icon that never appears (project.md sec 4.1). With
	 * no tray and no window there would be no way back to the program at
	 * all, so the window is shown in that case instead of hidden.
	 */
	if (bbq_tray_icon::is_available()) {
		tray.show();
		window.show();
	} else {
		QTextStream error(stderr);
		error << "bbqpredictor: no system tray on this session.\n";
		error << "bbqpredictor:   On GNOME this needs a StatusNotifierItem\n";
		error << "bbqpredictor:   shell extension. Running as a plain window.\n";
		window.show();
	}

	window.begin(station, geocode);

	/*
	 * Render and exit. Bounded twice over: the shot is taken when the
	 * feed settles, and a wall-clock timer takes it regardless so a
	 * request that never answers cannot leave this running forever.
	 */
	const QString shot = option_value(arguments, QStringLiteral("--shot"));
	if (!shot.isEmpty()) {
		bool taken = false;

		const auto take = [&window, shot, &taken]() {
			if (taken) {
				return;
			}
			taken = true;

			const QPixmap picture = window.grab();
			QTextStream report(stdout);
			if (picture.save(shot)) {
				report << "shot: wrote " << shot << "\n";
			} else {
				report << "shot: could not write " << shot << "\n";
			}
			QApplication::quit();
		};

		QObject::connect(window.feed(), &bbq_wu_feed::settled, &window,
		                 [take]() { QTimer::singleShot(300, take); });
		QTimer::singleShot(30000, &window, take);
	}

	return app.exec();
}
