#include "ui/tray_icon.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPen>
#include <QPixmap>

bbq_tray_icon::bbq_tray_icon(QObject *parent)
        : QSystemTrayIcon(parent), menu(nullptr) {
	setIcon(placeholder_icon());

	/*
	 * The tooltip carries the freshness too, for the same reason the
	 * window does (project.md sec 2.4). The tray is where this program is
	 * glanced at rather than read, so it is the likelier place for a stale
	 * reading to be believed.
	 */
	setToolTip(tr("bbqpredictor -- no data yet"));

	menu = new QMenu();

	QAction *show_action = menu->addAction(tr("Show / hide"));
	connect(show_action, &QAction::triggered,
	        this, &bbq_tray_icon::toggle_requested);

	menu->addSeparator();

	QAction *quit_action = menu->addAction(tr("Quit"));
	connect(quit_action, &QAction::triggered, qApp, &QApplication::quit);

	setContextMenu(menu);

	/*
	 * Only a plain click toggles. A context-menu request must not, or the
	 * menu appears and the window flaps at the same time.
	 */
	connect(this, &QSystemTrayIcon::activated, this,
	        [this](QSystemTrayIcon::ActivationReason reason) {
		if (reason == QSystemTrayIcon::Trigger ||
		    reason == QSystemTrayIcon::DoubleClick) {
			emit toggle_requested();
		}
	});
}

bool bbq_tray_icon::is_available() {
	return QSystemTrayIcon::isSystemTrayAvailable();
}

QIcon bbq_tray_icon::placeholder_icon() {
	/*
	 * 22px is the conventional tray size on the desktops that have one.
	 * Drawn into a transparent pixmap so it sits on a panel of any colour.
	 */
	QPixmap pixmap(22, 22);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(QColor(210, 210, 210), 2));
	painter.setBrush(QBrush(QColor(190, 60, 40)));
	painter.drawEllipse(3, 3, 16, 16);

	return QIcon(pixmap);
}
