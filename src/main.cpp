#include <QApplication>
#include <QStringList>
#include <QTextStream>

#include "ui/main_window.h"
#include "ui/tray_icon.h"

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
	out << "\n";
	out << "A weather applet for the system tray and a window.\n";
	out << "\n";
	out << "  --version   print the version and exit\n";
	out << "  --help      print this and exit\n";
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
	 * A tray applet must not exit when its window is closed -- closing the
	 * window is how it gets put away, not how it is quit.
	 */
	QApplication::setQuitOnLastWindowClosed(false);

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

	return app.exec();
}
