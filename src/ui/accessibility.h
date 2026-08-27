#ifndef BBQ_UI_ACCESSIBILITY_H
#define BBQ_UI_ACCESSIBILITY_H

/*
 * Keep Qt from describing a widget's VALUE to Android (sec 10.6).
 *
 * Qt's Android accessibility bridge builds an AccessibilityNodeInfo
 * RangeInfo for any widget exposing a value interface, using a
 * constructor that exists only from API 33. On anything older the call
 * throws, Qt does not clear the pending JNI exception, and the next JNI
 * call aborts the process -- so on Android 10 the program dies the
 * moment somebody touches a slider or a scrollbar, provided any
 * accessibility service is running.
 *
 * That is not an exotic configuration. A phone that will never see
 * another Android release is an ordinary phone, and screen readers,
 * password managers and gesture apps are all accessibility services.
 *
 * The fix is narrow: an accessibility factory that hands those widgets
 * a plain QAccessibleWidget, which has no value interface, so
 * `info.hasValue` is false and the node is never built. Names, roles,
 * focus and text still work -- only the numeric value is withheld, and
 * only on the platform that cannot be told it.
 *
 * A no-op everywhere else, and it should be removed when Qt carries the
 * version guard it already applies elsewhere in the same function.
 */
void bbq_install_accessibility_workaround();

/*
 * The factory itself, exposed so a test can check the property the
 * whole workaround rests on: that these widgets come back WITHOUT a
 * value interface. Installed only on Android; correct everywhere.
 */
class QAccessibleInterface;
class QObject;
class QString;

QAccessibleInterface *bbq_accessible_without_value(const QString &key,
                                                   QObject *object);

#endif
