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
	void a_refused_key_page_is_tried_three_times();
	void api_requests_ask_for_identity_encoding();
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

void test_client::a_refused_key_page_is_tried_three_times() {
	/*
	 * The page refuses about half the time (project.md sec 2.6.1.1),
	 * and a refusal used to cost a whole round: nothing in one can run
	 * without a key. Three attempts by the copyright holder's
	 * instruction.
	 *
	 * A proxy pointing at a closed port on loopback makes every
	 * transfer fail at once and without leaving the machine, which is
	 * what lets the retry be exercised at all -- the real failure is
	 * intermittent and cannot be asked for.
	 */
	QNetworkAccessManager net;
	net.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy,
	                           QStringLiteral("127.0.0.1"), 1));
	bbq_wu_key_source keys(&net);

	QSignalSpy failures(&keys, &bbq_wu_key_source::failed);
	keys.acquire();

	/*
	 * Waited for rather than assumed: the attempts are spaced, so a
	 * test that looked immediately would see one and call it three.
	 */
	QVERIFY2(failures.wait(15000), "the key source never gave up");

	QCOMPARE(failures.count(), 1);
	QCOMPARE(keys.m_attempts, 3);

	/*
	 * And it gives up rather than trying for ever. A retry that never
	 * stops is a slower way of hanging.
	 */
	QVERIFY2(!keys.has_key(), "a refused page must not leave a key behind");
}

void test_client::api_requests_ask_for_identity_encoding() {
	/*
	 * The compressed variant of a history URL is STALE (sec 2.6.5).
	 *
	 * Measured against one URL and one key, a minute apart, differing
	 * only in Accept-Encoding: identity gave 288 observations and gzip
	 * gave 78, ending seventeen hours earlier. Qt asks for gzip by
	 * default, so the archive was handed a fraction of each day while
	 * every band answered and every status was 200.
	 *
	 * Asserted on the REQUEST rather than on a response, because the
	 * fault is in what we ask for. A test that fetched would be asking
	 * a CDN's cache what mood it was in.
	 */
	QNetworkAccessManager net;
	net.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy,
	                           QStringLiteral("127.0.0.1"), 1));
	bbq_wu_key_source keys(&net);
	bbq_wu_client client(&net, &keys);

	/* A key, so the request is built rather than queued. */
	keys.m_key = QStringLiteral("0123456789abcdef0123456789abcdef");

	QSignalSpy sent(&net, &QNetworkAccessManager::finished);
	client.fetch_observed(QStringLiteral("ITEST1"), QStringLiteral("20260903"));

	QVERIFY2(sent.wait(15000), "the request never completed, even against a "
	                           "dead proxy");

	const QNetworkRequest asked =
	        sent.at(0).at(0).value<QNetworkReply *>()->request();

	QCOMPARE(asked.rawHeader("Accept-Encoding"), QByteArray("identity"));
}

QTEST_GUILESS_MAIN(test_client)
#include "test_client.moc"
