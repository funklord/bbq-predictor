#ifndef BBQ_UI_THEME_H
#define BBQ_UI_THEME_H

#include <QColor>
#include <QString>
#include <QStringList>
#include <Qt>

/*
 * Light, dark, or whatever the device says (project.md sec 10.4).
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
 * thing sec 10.4 added a dark mode to stop.
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
 * WCAG relative luminance and contrast ratio (project.md sec 16.25).
 *
 * Gamma-correct, unlike the Rec.709 sum theme.cpp uses to decide whether
 * a desktop is dark. harmonization.md settles that the two are
 * interchangeable for a background against its own foreground, because
 * they disagree only below about 2.8:1 and a readable pair clears 3:1 --
 * and it says in the same breath that the caveat travels with the rule.
 * This is the case it names: a ratio measured against a floor, where the
 * arithmetic is the answer rather than a tie-break, so the constant
 * matters and the gamma-correct form is the one to use.
 *
 * The same expression tool/palette_contrast.py checks the palette with,
 * so a colour this clamps and a colour that gate passes are answering
 * one question.
 */
double bbq_relative_luminance(const QColor &colour);
double bbq_contrast_ratio(const QColor &first, const QColor &second);

/*
 * Move `ink` away from `ground` until it clears `floor`, keeping its
 * hue.
 *
 * For drawing onto something this program does not own. harmonization.md
 * allows a program to follow a ground it did not choose ONLY if it
 * clamps against that ground at draw time, which is what this is; the
 * home-screen widget is the caller (sec 16.25).
 *
 * Returns as far as it got when the floor is out of reach, rather than
 * refusing. An ink walked to white against a white ground is no worse
 * than the ink it started from, and a caller that got nothing back would
 * have to invent a fallback -- which is the guess this exists to avoid.
 */
QColor bbq_ensure_contrast(const QColor &ink, const QColor &ground,
                           double floor);

/*
 * Composite a translucent `src` onto an OPAQUE `dst`, giving the opaque
 * colour that painting src over dst would produce (sec 16.89).
 *
 * For a caller that knows what is underneath. Qt's raster engine fills
 * an opaque colour with a memory fill and a translucent one through the
 * blend path, and the gap between them is not small where the shape is
 * narrow: measured on this machine, seventeen strips 24 pixels wide and
 * 556 tall cost 18.0 ns per pixel blended and 1.3 ns per pixel opaque,
 * FOURTEEN TIMES, because a 24-pixel span amortises none of the blend's
 * per-span setup while a memory fill has almost none to amortise. The
 * same area as one wide rectangle blends at 1.3 ns/px, so it is the
 * combination of narrow and translucent that is expensive, not either
 * on its own.
 *
 * The blend is done by Qt on a single pixel and read back, rather than
 * reproduced here, so the answer is bit-exact on every platform instead
 * of on the ones whose rounding somebody checked -- see theme.cpp for
 * what that cost when it was tried the other way. The substitution the
 * caller relies on is held by bbq_flatten_matches_qt in test_view.
 *
 * `dst` is required to be opaque. Blending onto something translucent
 * is a different sum and this does not do it -- the caller that cannot
 * promise an opaque ground keeps painting translucent.
 */
QColor bbq_flatten_over(const QColor &src, const QColor &dst);

/*
 * Apply to the whole application, so the widgets around the graph agree
 * with it. `automatic` releases the override rather than pinning the
 * current answer, which is what makes the device's own setting keep
 * working after the user has visited this one.
 */
void bbq_theme_apply(bbq_theme theme);

#endif
