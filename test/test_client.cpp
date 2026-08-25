#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTest>

#include "wu/client.h"
#include "wu/key_source.h"

/*
 * The queue of requests waiting for a key (project.md sec 2.3.1).
 *
 * This exists because of a defect found by reading rather than running,
 * and the defect's whole character was that the happy path proved
 * nothing: every real run acquired a key on the first attempt, so the
 * broken half was never reached. These drive the key source's outcomes
 * directly instead of waiting for a network that always cooperated.
 *
 * No request leaves the machine. Two things see to that, and both are
 * needed: the key source is pointed at a dead proxy on localhost, and the
 * event loop is never run, so a request that is started never progresses
 * and no reply ever arrives. Every assertion below is synchronous.
 *
 * The first attempt used QTEST_APPLESS_MAIN and was worse than useless:
 * without a QCoreApplication, QNetworkAccessManager returns NULL replies,
 * so the code under test was connecting to nullptr and the assertions
 * held for a reason that had nothing to do with the queue. It passed. A
 * test that passes because the machinery underneath it is broken is the
 * vacuous pass in its purest form, and it was visible only as a warning.
 */
class test_client : public QObject {
	Q_OBJECT

private slots:
	void requests_wait_when_there_is_no_key();
	void the_queue_is_drained_once_by_whichever_signal_arrives();
};

void test_client::requests_wait_when_there_is_no_key() {
	QNetworkAccessManager net;
	net.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy,
	                           QStringLiteral("127.0.0.1"), 1));
	bbq_wu_key_source keys(&net);
	bbq_wu_client client(&net, &keys);

	QVERIFY2(!keys.has_key(), "a fresh key source must not start with a key");
	QCOMPARE(client.waiting(), 0);

	client.fetch_hourly(59.33, 18.07);
	client.fetch_nowcast(59.33, 18.07);

	/*
	 * Both, not one. The fan-out is the thing that multiplied the
	 * original defect: a cold start queues four of these behind a single
	 * acquisition (sec 2.6).
	 */
	QCOMPARE(client.waiting(), 2);
}

void test_client::the_queue_is_drained_once_by_whichever_signal_arrives() {
	QNetworkAccessManager net;
	net.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy,
	                           QStringLiteral("127.0.0.1"), 1));
	bbq_wu_key_source keys(&net);
	bbq_wu_client client(&net, &keys);

	QSignalSpy failures(&client, &bbq_wu_client::failed);

	client.fetch_hourly(59.33, 18.07);
	client.fetch_nowcast(59.33, 18.07);
	QCOMPARE(client.waiting(), 2);

	/* The key could not be had. Each waiting request hears about it. */
	emit keys.failed(QStringLiteral("no key in the bundle"));

	QCOMPARE(failures.count(), 2);
	QCOMPARE(client.waiting(), 0);

	/*
	 * The decisive one, and the shape of the original defect.
	 *
	 * The old code connected a PAIR of handlers per queued request and
	 * let each tear down only its own, so the failure above left two
	 * live `acquired` handlers behind. This next line fired them, and
	 * they re-sent two requests that had already reported failure --
	 * which, with no key yet, put them straight back on the queue.
	 *
	 * Nothing has given the key source a key, so a correct client has
	 * nothing to drain and does nothing at all.
	 */
	emit keys.acquired(QStringLiteral("0123456789abcdef0123456789abcdef"));

	QCOMPARE(client.waiting(), 0);
	QCOMPARE(failures.count(), 2);

	/*
	 * And the mirror: a second failure must not re-report the requests
	 * the first one already answered.
	 */
	emit keys.failed(QStringLiteral("still no key"));

	QCOMPARE(failures.count(), 2);
	QCOMPARE(client.waiting(), 0);
}

QTEST_GUILESS_MAIN(test_client)
#include "test_client.moc"
