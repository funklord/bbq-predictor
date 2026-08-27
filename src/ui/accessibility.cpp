#include "ui/accessibility.h"

#include <QAbstractSlider>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QProgressBar>
#include <QWidget>
#include <QtGlobal>

/*
 * Compiled on every platform even though it is installed on one, so
 * that the mechanism can be tested where the tests run. What is
 * platform-specific is whether Qt is asked to use it, not whether it
 * is correct.
 */
QAccessibleInterface *bbq_accessible_without_value(const QString &key,
                                                   QObject *object) {
	Q_UNUSED(key);

	/*
	 * QAbstractSlider covers QSlider, QScrollBar and QDial in one -- and
	 * the scrollbar is the one that matters most, because Qt creates
	 * those itself inside every scrollable view and no application
	 * choice can avoid them.
	 *
	 * A plain QAccessibleWidget carries no value interface, which is
	 * exactly the property that keeps Qt from building the node that
	 * crashes. Returning nullptr for anything else lets Qt fall through
	 * to its own factory, so nothing else is affected.
	 */
	if (QAbstractSlider *slider = qobject_cast<QAbstractSlider *>(object)) {
		return new QAccessibleWidget(slider, QAccessible::Slider);
	}

	if (QProgressBar *bar = qobject_cast<QProgressBar *>(object)) {
		return new QAccessibleWidget(bar, QAccessible::ProgressBar);
	}

	return nullptr;
}

void bbq_install_accessibility_workaround() {
#ifdef Q_OS_ANDROID
	/*
	 * Installed rather than replacing anything: Qt tries the most
	 * recently installed factory first and falls back to its own for
	 * every object this one declines.
	 */
	QAccessible::installFactory(bbq_accessible_without_value);
#endif
}
