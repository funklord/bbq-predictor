#include "ui/theme.h"

#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>

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
	if (hints != nullptr && hints->colorScheme() == Qt::ColorScheme::Dark) {
		return Qt::ColorScheme::Dark;
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
