#ifndef BBQ_UI_THEME_H
#define BBQ_UI_THEME_H

#include <QString>
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
 * Apply to the whole application, so the widgets around the graph agree
 * with it. `automatic` releases the override rather than pinning the
 * current answer, which is what makes the device's own setting keep
 * working after the user has visited this one.
 */
void bbq_theme_apply(bbq_theme theme);

#endif
