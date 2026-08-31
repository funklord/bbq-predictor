#include "ui/theme.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <QTextStream>

bbq_theme bbq_theme_resolve(const QString &preference) {
	const QString wanted = preference.trimmed().toLower();

	if (wanted == QStringLiteral("light")) {
		return bbq_theme::light;
	}
	if (wanted == QStringLiteral("dark")) {
		return bbq_theme::dark;
	}

	return bbq_theme::automatic;
}

const char *bbq_theme_name(bbq_theme theme) {
	switch (theme) {
	case bbq_theme::automatic:
		return "auto";
	case bbq_theme::light:
		return "light";
	case bbq_theme::dark:
		return "dark";
	}

	return "auto";
}

Qt::ColorScheme bbq_theme_scheme(bbq_theme theme) {
	switch (theme) {
	case bbq_theme::light:
		return Qt::ColorScheme::Light;
	case bbq_theme::dark:
		return Qt::ColorScheme::Dark;
	case bbq_theme::automatic:
		break;
	}

	/*
	 * Ask the platform, and treat "no opinion" as light.
	 *
	 * Qt returns Unknown where the platform does not say, and a caller
	 * choosing a palette has to pick something -- leaving three cases
	 * for a two-valued question would push the same decision out to
	 * every call site, differently each time.
	 */
	const QStyleHints *hints = QGuiApplication::styleHints();
	if (hints != nullptr) {
		const Qt::ColorScheme hinted = hints->colorScheme();
		if (hinted != Qt::ColorScheme::Unknown) {
			return hinted;
		}
	}

	/*
	 * The desktop's own file, asked before defaulting to light rather
	 * than instead of the hint. A Trinity or KDE 3 session tells Qt
	 * nothing, so the hint above is Unknown there and the default below
	 * would light up a white rectangle at night on a desktop that has
	 * said, in the only place it says it, that it is dark.
	 */
	const Qt::ColorScheme written =
	    bbq_scheme_from_desktop_files(bbq_scheme_sources());
	if (written != Qt::ColorScheme::Unknown) {
		return written;
	}

	return Qt::ColorScheme::Light;
}

namespace {

/*
 * A palette, set explicitly rather than left to the style.
 *
 * setColorScheme alone moved the graph and left the controls light --
 * a dark plot in a light window, which is worse than either and was the
 * first thing a rendering showed. Whether a style honours the hint is
 * the style's business; what the applet looks like is not, so the
 * colours are stated.
 *
 * Deterministic across platforms is the right trade for this program:
 * the graph's own palette is measured and fixed for the same reason
 * (sec 3.8.3), and a phone applet that renders differently on every
 * device is harder to reason about than one that always looks the same.
 */
QPalette palette_for_scheme(Qt::ColorScheme scheme) {
	QPalette palette;

	if (scheme != Qt::ColorScheme::Dark) {
		const QColor ground(0xf2, 0xf2, 0xf2);
		const QColor ink(0x1e, 0x1e, 0x1e);

		palette.setColor(QPalette::Window, ground);
		palette.setColor(QPalette::WindowText, ink);
		palette.setColor(QPalette::Base, QColor(0xff, 0xff, 0xff));
		palette.setColor(QPalette::AlternateBase, ground);
		palette.setColor(QPalette::Text, ink);
		palette.setColor(QPalette::Button, ground);
		palette.setColor(QPalette::ButtonText, ink);
		palette.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xff));
		palette.setColor(QPalette::ToolTipText, ink);
		return palette;
	}

	/*
	 * The same near-black the plot uses, so the window reads as one
	 * surface rather than a panel sitting on a different one.
	 */
	const QColor ground(0x1c, 0x1e, 0x20);
	const QColor raised(0x26, 0x2a, 0x2e);
	const QColor ink(0xe4, 0xe6, 0xe8);

	palette.setColor(QPalette::Window, ground);
	palette.setColor(QPalette::WindowText, ink);
	palette.setColor(QPalette::Base, QColor(0x16, 0x18, 0x1a));
	palette.setColor(QPalette::AlternateBase, raised);
	palette.setColor(QPalette::Text, ink);
	palette.setColor(QPalette::Button, raised);
	palette.setColor(QPalette::ButtonText, ink);
	palette.setColor(QPalette::ToolTipBase, raised);
	palette.setColor(QPalette::ToolTipText, ink);
	palette.setColor(QPalette::Highlight, QColor(0x2f, 0x6f, 0xb5));
	palette.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));

	/* Disabled text has to stay legibly dimmer, not vanish. */
	const QColor faded(0x7a, 0x7e, 0x82);

	palette.setColor(QPalette::Disabled, QPalette::WindowText, faded);
	palette.setColor(QPalette::Disabled, QPalette::Text, faded);
	palette.setColor(QPalette::Disabled, QPalette::ButtonText, faded);

	return palette;
}

} // namespace

void bbq_theme_apply(bbq_theme theme) {
	QStyleHints *hints = QGuiApplication::styleHints();
	if (hints == nullptr) {
		return;
	}

	QGuiApplication::setPalette(palette_for_scheme(bbq_theme_scheme(theme)));

	/*
	 * UNSET for automatic, rather than setting the current answer.
	 *
	 * Pinning what the device happens to say today would freeze it: a
	 * phone switching to dark at sunset would stop being followed, and
	 * the setting called "auto" would be the one thing that no longer
	 * was. Releasing the override is what keeps the platform in charge.
	 */
	if (theme == bbq_theme::automatic) {
		hints->unsetColorScheme();
		return;
	}

	hints->setColorScheme(bbq_theme_scheme(theme));
}

namespace {

/*
 * Rec.709 luminance, the comparison harmonization.md specifies. Plain
 * integers rather than QColor: nothing here needs a colour object, and
 * the arithmetic is the whole of the decision.
 */
double luminance_709(int r, int g, int b) {
	return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/*
 * "0,42,78" -> three channels, or false. TDE and KDE 3 write plain
 * decimal triples; anything else is not this format and is not guessed
 * at.
 */
/*
 * "#232323" -> three channels, or false. LXQt writes hex where TDE writes
 * decimal triples: two spellings of one statement, so both are read.
 */
bool parse_hex(const QString &value, int *r, int *g, int *b) {
	const QString s = value.trimmed();
	if (s.size() != 7 || !s.startsWith(QLatin1Char('#'))) {
		return false;
	}

	bool ok = false;
	const uint n = s.mid(1).toUInt(&ok, 16);
	if (!ok) {
		return false;
	}

	*r = int((n >> 16) & 0xff);
	*g = int((n >> 8) & 0xff);
	*b = int(n & 0xff);
	return true;
}

bool parse_triple(const QString &value, int *r, int *g, int *b) {
	const QStringList parts = value.split(QLatin1Char(','));
	if (parts.size() != 3) {
		return false;
	}

	bool ok_r = false;
	bool ok_g = false;
	bool ok_b = false;
	*r = parts.at(0).trimmed().toInt(&ok_r);
	*g = parts.at(1).trimmed().toInt(&ok_g);
	*b = parts.at(2).trimmed().toInt(&ok_b);
	if (!ok_r || !ok_g || !ok_b) {
		return false;
	}

	return *r >= 0 && *r <= 255 && *g >= 0 && *g <= 255 && *b >= 0 && *b <= 255;
}

}  // namespace

QStringList bbq_lxqt_palette_files(const QString &config,
                                    const QStringList &data_dirs) {
	QFile file(config);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return {};
	}

	QTextStream in(&file);
	QString section;
	QString name;
	while (!in.atEnd()) {
		const QString line = in.readLine().trimmed();
		if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
			section = line.mid(1, line.size() - 2);
			continue;
		}

		if (section.compare(QStringLiteral("General"), Qt::CaseInsensitive) != 0) {
			continue;
		}

		const int eq = line.indexOf(QLatin1Char('='));
		if (eq < 0) {
			continue;
		}

		if (line.left(eq).trimmed().compare(QStringLiteral("theme"),
		                                     Qt::CaseInsensitive) == 0) {
			name = line.mid(eq + 1).trimmed();
		}
	}

	if (name.isEmpty()) {
		return {};
	}

	/*
	 * A palette name is a filename component. One carrying a separator
	 * would reach outside the palette directories, and none legitimately
	 * does: refuse rather than resolve.
	 */
	if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
	    name.startsWith(QLatin1Char('.'))) {
		return {};
	}

	QStringList out;
	for (const QString &dir : data_dirs) {
		out << dir + QStringLiteral("/lxqt/palettes/") + name;
	}

	return out;
}

QStringList bbq_scheme_sources() {
	const QString home = QDir::homePath();
	const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
	const QString cfg = xdg.isEmpty() ? home + QStringLiteral("/.config")
	                                   : QString::fromLocal8Bit(xdg);

	/* XDG's own defaults: a desktop that sets neither is exactly the kind
	 * this rung exists for. */
	const QByteArray dh = qgetenv("XDG_DATA_HOME");
	const QByteArray dd = qgetenv("XDG_DATA_DIRS");
	const QString data_home = dh.isEmpty()
	    ? home + QStringLiteral("/.local/share")
	    : QString::fromLocal8Bit(dh);
	const QString data_rest = dd.isEmpty()
	    ? QStringLiteral("/usr/local/share:/usr/share")
	    : QString::fromLocal8Bit(dd);
	QStringList data_dirs;
	data_dirs << data_home;
	data_dirs << data_rest.split(QLatin1Char(':'), Qt::SkipEmptyParts);

	QStringList sources;
	sources << cfg + QStringLiteral("/kdeglobals");
	sources << home + QStringLiteral("/.trinity/share/config/kdeglobals");
	sources << home + QStringLiteral("/.kde/share/config/kdeglobals");
	sources << bbq_lxqt_palette_files(
	    cfg + QStringLiteral("/lxqt/lxqt.conf"), data_dirs);
	return sources;
}

Qt::ColorScheme bbq_scheme_from_desktop_files(const QStringList &sources) {
	for (const QString &path : sources) {
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
			continue;
		}

		QTextStream in(&file);
		QString section;
		int br = -1;
		int bg = -1;
		int bb = -1;
		int fr = -1;
		int fg = -1;
		int fb = -1;

		while (!in.atEnd()) {
			const QString line = in.readLine().trimmed();
			if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
				section = line.mid(1, line.size() - 2);
				continue;
			}

			/*
			 * Only under [General]. The same key names appear in
			 * per-application sections, and taking whichever came last
			 * would answer about some other program's colours.
			 */
			const bool general = section.compare(QStringLiteral("General"),
			                                      Qt::CaseInsensitive) == 0;
			const bool palette = section.compare(QStringLiteral("Palette"),
			                                      Qt::CaseInsensitive) == 0;
			if (!general && !palette) {
				continue;
			}

			const int eq = line.indexOf(QLatin1Char('='));
			if (eq < 0) {
				continue;
			}

			const QString key = line.left(eq).trimmed();
			const QString value = line.mid(eq + 1).trimmed();
			if (general) {
				/* TDE and KDE 3. */
				if (key.compare(QStringLiteral("windowBackground"),
				                 Qt::CaseInsensitive) == 0) {
					parse_triple(value, &br, &bg, &bb);
				} else if (key.compare(QStringLiteral("windowForeground"),
				                        Qt::CaseInsensitive) == 0) {
					parse_triple(value, &fr, &fg, &fb);
				}
			} else if (key.compare(QStringLiteral("window_color"),
			                        Qt::CaseInsensitive) == 0) {
				/* LXQt. */
				parse_hex(value, &br, &bg, &bb);
			} else if (key.compare(QStringLiteral("window_text_color"),
			                        Qt::CaseInsensitive) == 0) {
				parse_hex(value, &fr, &fg, &fb);
			}
		}

		/* This file had no answer; the next one may. */
		if (br < 0 || fr < 0) {
			continue;
		}

		const double back = luminance_709(br, bg, bb);
		const double fore = luminance_709(fr, fg, fb);
		if (back < fore) {
			return Qt::ColorScheme::Dark;
		}
		if (back > fore) {
			return Qt::ColorScheme::Light;
		}

		/* Identical luminance states nothing. */
		return Qt::ColorScheme::Unknown;
	}

	/*
	 * ABSTAIN RATHER THAN GUESS. The errors are not symmetric: a wrong
	 * light answer is merely plain, while a wrong dark one is unreadable
	 * text on a pale ground. No readable file means no opinion, and
	 * bbq_theme_scheme keeps its own default.
	 */
	return Qt::ColorScheme::Unknown;
}
