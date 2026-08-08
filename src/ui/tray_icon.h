#ifndef BBQ_TRAY_ICON_H
#define BBQ_TRAY_ICON_H

#include <QSystemTrayIcon>

#include "model/composite.h"

class QIcon;
class QMenu;

/*
 * The systray half of the applet (project.md sec 0, sec 4).
 *
 * A caveat lives here rather than in a commit message, because it decides
 * whether half the stated product exists on a given machine.
 * QSystemTrayIcon on Linux now means StatusNotifierItem: KDE Plasma
 * serves it natively, and GNOME does not without a shell extension. On a
 * bare GNOME session there is no tray for this to appear in, and Qt says
 * so only through is_available() below.
 *
 * Which desktop this targets is project.md sec 4.1 and is UNRESOLVED. The
 * answer decides whether a fallback presentation is required, so the
 * availability check is reported honestly rather than swallowed -- an
 * applet that silently is not there is the worst of the options.
 */
class bbq_tray_icon : public QSystemTrayIcon {
	Q_OBJECT

public:
	explicit bbq_tray_icon(QObject *parent = nullptr);

	/*
	 * Whether this session actually has a tray to sit in. Static because
	 * the answer is the platform's and does not need an instance.
	 */
	static bool is_available();

	/*
	 * Put the weather in the tray (project.md sec 4.2).
	 *
	 * The icon carried a placeholder dot from the first commit, which
	 * made the systray half of sec 0's brief a coloured circle. It
	 * shows the current temperature now, and sec 2.4's requirement
	 * that a failed refresh be visible IN THE TRAY as well as in the
	 * window is finally something the tray does rather than something
	 * the document asks for.
	 */
	void show_state(const bbq_composite &composite, const QString &verdict);

signals:
	/* The user clicked the icon and wants the window shown or hidden. */
	void toggle_requested();

private:
	/*
	 * The icon, painted rather than loaded.
	 *
	 * There is no artwork in this tree yet and no resource file to hold
	 * it. Drawing a placeholder keeps the skeleton free of a binary asset
	 * that would have to be replaced anyway, and a tray icon set to a null
	 * QIcon is invisible -- which looks exactly like the GNOME case above
	 * and would confuse the first person to hit it.
	 */
	static QIcon placeholder_icon();

	/*
	 * The temperature, drawn at the sizes a tray asks for. Rendered
	 * rather than composed from a font glyph so the digits fill the
	 * space -- a 22 pixel icon has no room for padding.
	 */
	static QIcon reading_icon(const QString &text, const QColor &ink);

	QMenu *menu;
};

#endif
