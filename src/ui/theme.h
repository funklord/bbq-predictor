#ifndef BBQ_UI_THEME_H
#define BBQ_UI_THEME_H

#include <QString>
#include <QStringList>
#include <Qt>

/*
 * Light, dark, or whatever the device says (project.md sec 10.3).
 *
 * This reverses sec 3.8.3, which fixed the graph's palette to Weather
 * Underground's measured colours on a white plot and deliberately
 * refused to follow the desktop into dark mode. That reasoning was
 * sound and is still recorded: the white plot with pale hour bands IS
 * the look sec 0 asked for. What changed is that the applet now runs on
 * a phone, where a white rectangle at night is not a style choice
 * somebody made, it is a torch.
 *
 * The measured data colours are kept in both schemes. What changes is
 * the ground they are drawn on and the furniture around them -- the
 * red of the temperature curve is WU's red either way, because it is a
 * measurement of their chart rather than a decoration.
 */
enum class bbq_theme {
	automatic,
	light,
	dark,
};

/*
 * Reads a setting. Anything unrecognised means automatic, so a
 * hand-edited config with a typo lands somewhere defined rather than
 * somewhere undefined -- the same rule the layout setting follows.
 */
bbq_theme bbq_theme_resolve(const QString &preference);
const char *bbq_theme_name(bbq_theme theme);

/*
 * What `automatic` actually means right now: the platform's own answer,
 * and light where the platform has no opinion. Never returns Unknown,
 * so callers have two cases rather than three.
 */
Qt::ColorScheme bbq_theme_scheme(bbq_theme theme);

/*
 * The colour-scheme files the desktops here write, most authoritative
 * first. Separate from the parser below so a test can supply its own.
 *
 * Two dialects, because a desktop with no dark-mode status is the normal
 * case and each spells the same statement differently: TDE and KDE 3 use
 * [General] with decimal triples, LXQt uses [Palette] with #rrggbb.
 */
QStringList bbq_scheme_sources();


/*
 * Tier 4 of the shared dark-desktop rule (claude-guidelines
 * harmonization.md): the scheme the desktop wrote down, or Unknown.
 *
 * The platform hint above does not fail VISIBLY on a Trinity or KDE 3
 * session, which is what makes this worth asking separately. That
 * desktop exposes no Qt 6 platform theme and runs no XDG portal, so the
 * hint answers Unknown -- and the applet then defaults to light and
 * shows a white rectangle on a dark desktop at night, which is the exact
 * thing sec 10.3 added a dark mode to stop.
 *
 * THE COLOURS DECIDE, NEVER THE SCHEME NAME. `colorScheme=DarkBlue.kcsrc`
 * contains "Dark" by luck; plenty of dark schemes do not, and a name is
 * not a predicate about luminance.
 *
 * Returns Unknown when no file can be read or parsed -- ABSTAIN RATHER
 * THAN GUESS. The errors are not symmetric: a wrong light answer is
 * merely plain, a wrong dark one is unreadable text on a pale ground.
 * Unlike bbq_theme_scheme this DOES return Unknown, because it is a
 * source rather than the decision.
 */
Qt::ColorScheme bbq_scheme_from_desktop_files(const QStringList &sources);

/*
 * Apply to the whole application, so the widgets around the graph agree
 * with it. `automatic` releases the override rather than pinning the
 * current answer, which is what makes the device's own setting keep
 * working after the user has visited this one.
 */
void bbq_theme_apply(bbq_theme theme);

#endif
