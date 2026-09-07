#ifndef BBQ_UI_LOCATOR_H
#define BBQ_UI_LOCATOR_H

#include <QObject>
#include <QString>

class QGeoPositionInfoSource;

/*
 * Where the device is, asked once, for discovery only (sec 14.3).
 *
 * This answers "which stations are near me" and NOTHING ELSE. It does
 * not move the forecast: somebody watching a station in Stockholm while
 * standing in Gothenburg should still be shown Stockholm's weather, and
 * letting a fix change the forecast coordinate would be the
 * two-places-on-one-axis failure of sec 2.6.7 arriving through a
 * sensor.
 *
 * COARSE, and once rather than continuously. The nearest station is
 * hundreds of metres away at best, so precision buys nothing here, and
 * a program that watched the position would be spending battery to
 * re-answer a question whose answer barely changes.
 *
 * Every failure is the same failure from outside: no source compiled
 * in, no source on the machine, permission refused, or no fix before
 * the deadline all end as `unavailable` with a reason to show. The
 * fallback is the same in each case -- search for a place by name --
 * and it is the only route on a desktop, so it is a path that gets
 * used rather than one that waits to be discovered broken.
 */
class bbq_locator : public QObject {
	Q_OBJECT

public:
	explicit bbq_locator(QObject *parent = nullptr);

	/*
	 * Asks for one fix. Answers exactly once with `located` or
	 * `unavailable`, whichever comes first, and never both -- a caller
	 * that has to guard against being told twice is a caller that will
	 * eventually forget to.
	 */
	void locate_once(int timeout_ms = 20000);

signals:
	void located(double latitude, double longitude);
	void unavailable(const QString &reason);

private:
	void answer_located(double latitude, double longitude);
	void answer_unavailable(const QString &reason);

	/*
	 * Split from locate_once because asking for the permission is
	 * ASYNCHRONOUS: on Android the answer arrives when the user has
	 * dealt with a dialog, which may be never.
	 */
	void start_source(int timeout_ms);

	QGeoPositionInfoSource *m_source = nullptr;
	bool m_answered = false;

	/*
	 * Which request a deadline belongs to (sec 16.73).
	 *
	 * `locate_once` clears `m_answered`, and the previous call's
	 * timeout is still outstanding: it would fire into the NEW request
	 * and answer it "no position within the time allowed" before that
	 * request had had its own time. One call site exists today, so this
	 * is a hazard rather than a fault -- and the promise above is that
	 * a caller never has to guard against being told twice, which is
	 * not a promise to keep only while nobody asks twice.
	 */
	int m_generation = 0;
};

#endif
