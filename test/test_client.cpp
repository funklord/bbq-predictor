#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QSet>
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
	void a_query_parameter_is_read_back_by_name();
	void a_failed_key_page_says_what_the_server_answered();
	void only_a_key_a_weather_endpoint_used_is_taken();
	void the_site_key_is_taken_by_name_and_nothing_else_from_the_blob();
	void a_page_without_a_key_is_tried_again();
	void a_remembered_key_must_look_like_one();
	void each_attempt_asks_another_page();
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

/*
 * The two shapes WU served on 2026-10-22, cut down to the URLs that
 * matter (sec 2.6.1.3). The shell's one key belongs to an advertising
 * configuration URL and every weather endpoint refuses it; taking it is
 * what turned "the page had no key" into a round of 401s.
 */
static const char shell_page[] =
        "<script>{\"rulesUrl\":\"https://weather.com/api/v1/mew/entity/"
        "ad_third_party_config/6c750bdb?apiKey=5c241d00000000000000000000000000\"}"
        "</script>";

static const char full_page[] =
        "<script>{\"rulesUrl\":\"https://weather.com/api/v1/mew/entity/"
        "ad_third_party_config/6c750bdb?apiKey=5c241d00000000000000000000000000\"}"
        "{\"u\":\"https://api.weather.com/v3/location/near?"
        "apiKey=ff90f200000000000000000000000000&geocode=37.779%2C-122.419\"}"
        "</script>";

void test_client::only_a_key_a_weather_endpoint_used_is_taken() {
	QCOMPARE(bbq_wu_key_source::extract_key(QString::fromLatin1(full_page)),
	         QStringLiteral("ff90f200000000000000000000000000"));

	/* Not the advertising key, even though it is the only one there. */
	QCOMPARE(bbq_wu_key_source::extract_key(QString::fromLatin1(shell_page)), QString());
}

/*
 * Sec 2.6.1.5: the shell WU mostly serves carries the site's API_KEY in a
 * config blob, beside other 32-hex values the weather endpoints refuse.
 * The layout is the page's own, 2026-10-23, the values made up.
 */
static const char site_shell[] =
        "<script>(function () { const data = {\"API_KEY\":\"f6d2ef00000000000000000000000000\","
        "\"ZUORA_PAYMENT_PAGE_ID\":\"8a128800000000000000000000000000\"}; })();</script>"
        "<script>{\"rulesUrl\":\"https://weather.com/api/v1/mew/entity/"
        "ad_third_party_config/6c750bdb?apiKey=5c241d00000000000000000000000000\"}</script>";

void test_client::the_site_key_is_taken_by_name_and_nothing_else_from_the_blob() {
	QCOMPARE(bbq_wu_key_source::extract_key(QString::fromLatin1(site_shell)),
	         QStringLiteral("f6d2ef00000000000000000000000000"));

	/* A request URL's key still wins where the page has both. */
	const QString both = QString::fromLatin1(site_shell) + QString::fromLatin1(full_page);
	QCOMPARE(bbq_wu_key_source::extract_key(both),
	         QStringLiteral("ff90f200000000000000000000000000"));

	/* The blob's other values are not keys, however much they look like one. */
	QCOMPARE(bbq_wu_key_source::extract_key(QStringLiteral(
	                 "{\"ZUORA_PAYMENT_PAGE_ID\":\"8a128800000000000000000000000000\","
	                 "\"API_KEY_OLD\":\"0123456789abcdef0123456789abcdef\"}")),
	         QString());
}

void test_client::a_page_without_a_key_is_tried_again() {
	QNetworkAccessManager net;
	net.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy,
	                           QStringLiteral("127.0.0.1"), 1));
	bbq_wu_key_source keys(&net);
	QSignalSpy failures(&keys, &bbq_wu_key_source::failed);

	/*
	 * The first of three attempts brought the shell. WU alternates, so it
	 * is worth asking again -- and nothing is reported yet.
	 */
	keys.m_attempts = 1;
	keys.page_arrived(QString::fromLatin1(shell_page));
	QCOMPARE(failures.count(), 0);
	QVERIFY2(!keys.has_key(), "the advertising key was taken");

	/* The retry goes to the dead proxy and the budget runs out. */
	QVERIFY2(failures.wait(15000), "the key source never gave up");
	QCOMPARE(keys.m_attempts, 3);

	/* The last of three brings the shell: said as what it was. */
	QSignalSpy last(&keys, &bbq_wu_key_source::failed);
	keys.page_arrived(QString::fromLatin1(shell_page));
	QCOMPARE(last.count(), 1);
	QVERIFY2(last.at(0).at(0).toString().contains(QStringLiteral("no weather key")),
	         qPrintable(last.at(0).at(0).toString()));
}

void test_client::a_remembered_key_must_look_like_one() {
	QNetworkAccessManager net;
	bbq_wu_key_source keys(&net);
	QSignalSpy thrown(&keys, &bbq_wu_key_source::invalidated);

	QVERIFY(!keys.remember(QString()));
	QVERIFY(!keys.remember(QStringLiteral("ff90f2")));
	QVERIFY(!keys.remember(QStringLiteral("FF90F200000000000000000000000000")));
	QVERIFY(!keys.has_key());

	/* Throwing away nothing tells nobody. */
	keys.invalidate();
	QCOMPARE(thrown.count(), 0);

	QVERIFY(keys.remember(QStringLiteral("ff90f200000000000000000000000000")));
	QCOMPARE(keys.key(), QStringLiteral("ff90f200000000000000000000000000"));

	keys.invalidate();
	QCOMPARE(thrown.count(), 1);
	QVERIFY(!keys.has_key());
}

/*
 * Three attempts, three pages (sec 2.6.1.4). WU's CDN hands any one page
 * out fully rendered or as a shell, so asking one page three times was
 * three draws on that page's odds; the pages differ in how often they
 * carry a key, and asking each in turn spreads the draws. Read from the
 * requests themselves, against a proxy that refuses all three.
 */
void test_client::each_attempt_asks_another_page() {
	QNetworkAccessManager net;
	net.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy,
	                           QStringLiteral("127.0.0.1"), 1));
	bbq_wu_key_source keys(&net);

	/*
	 * Read as each reply finishes, not afterwards: the key source deletes
	 * a reply once it has handled it, so a spy's pointers are dangling by
	 * the time the wait returns -- which is how this test first crashed.
	 */
	QStringList pages;
	connect(&net, &QNetworkAccessManager::finished, this,
	        [&pages](QNetworkReply *reply) { pages << reply->request().url().toString(); });
	QSignalSpy failures(&keys, &bbq_wu_key_source::failed);
	keys.acquire();
	QVERIFY2(failures.wait(15000), "the key source never gave up");
	QCOMPARE(pages.size(), 3);
	QCOMPARE(QSet<QString>(pages.begin(), pages.end()).size(), 3);
	for (const QString &page : pages) {
		QVERIFY2(page.startsWith(QStringLiteral("https://www.wunderground.com/")),
		         qPrintable(page));
	}

	/* The page measured most often to carry a key goes first. */
	QCOMPARE(pages.first(), QStringLiteral("https://www.wunderground.com/forecast"));
	QCOMPARE(keys.sent(), 3);
}

QTEST_GUILESS_MAIN(test_client)
#include "test_client.moc"

/*
 * Reading one parameter out of a request's query.
 *
 * The reason it exists is that two observed requests go out per round,
 * for the day in progress and the backfill day behind it, and both
 * carry the same product. A silent station fails both, and without the
 * date the two lines are identical -- a reader who takes them for a
 * duplicate is right about the text and wrong about the facts.
 */
void test_client::a_query_parameter_is_read_back_by_name() {
	const QString query = QStringLiteral("stationId=ISTOCK877&date=20260907");

	QCOMPARE(bbq_wu_query_parameter(query, QStringLiteral("date")),
	         QStringLiteral("20260907"));
	QCOMPARE(bbq_wu_query_parameter(query, QStringLiteral("stationId")),
	         QStringLiteral("ISTOCK877"));

	/* Absent is empty, and the caller prints nothing rather than "for". */
	QVERIFY(bbq_wu_query_parameter(query, QStringLiteral("units")).isEmpty());
	QVERIFY(bbq_wu_query_parameter(QString(), QStringLiteral("date"))
	                .isEmpty());

	/*
	 * A NAME THAT IS A SUFFIX OF ANOTHER must not match it. "date" ends
	 * "updateDate", and a scan looking for the text anywhere would
	 * return the wrong day -- which is worse than returning none, since
	 * the whole point is telling two days apart.
	 */
	const QString awkward =
	        QStringLiteral("updateDate=19990101&date=20260907");
	QCOMPARE(bbq_wu_query_parameter(awkward, QStringLiteral("date")),
	         QStringLiteral("20260907"));
}

/*
 * The message when the key page cannot be fetched.
 *
 * Qt's text ends "- server replied: " and then whatever the server
 * said, which for a bare 404 is nothing -- so the reader gets a
 * sentence that stops at a colon and reads as truncated. Seen on this
 * path, the one sec 2.2 documents as fragile:
 *
 *     no API key: Error transferring https://www.wunderground.com/
 *     forecast - server replied:
 *
 * Asked of the function rather than of a staged network failure. A
 * connection that never reaches a server has no status, and that case
 * must keep Qt's text unchanged -- which is the half a test written
 * only for the 404 would let rot.
 */
void test_client::a_failed_key_page_says_what_the_server_answered() {
	const QString dangling =
	        QStringLiteral("Error transferring https://www.wunderground.com/"
	                       "forecast - server replied:");

	const QString named = bbq_wu_transfer_error(dangling, 404);
	QVERIFY2(named.contains(QStringLiteral("HTTP 404")),
	         qPrintable(QStringLiteral("the status is not in \"%1\"").arg(named)));
	QVERIFY2(!named.endsWith(QLatin1Char(':')),
	         qPrintable(QStringLiteral("\"%1\" still stops at a colon")
	                            .arg(named)));

	/* A server that DID say something keeps what it said, plus the status. */
	const QString spoke = bbq_wu_transfer_error(
	        QStringLiteral("Error transferring x - server replied: Forbidden"),
	        403);
	QVERIFY(spoke.contains(QStringLiteral("Forbidden")));
	QVERIFY(spoke.contains(QStringLiteral("HTTP 403")));

	/*
	 * And no status means the transfer never reached a server, so Qt's
	 * text is the whole of what is known and must come back untouched.
	 */
	const QString unreached =
	        QStringLiteral("Connection refused");
	QCOMPARE(bbq_wu_transfer_error(unreached, 0), unreached);
	QCOMPARE(bbq_wu_transfer_error(dangling, 0), dangling);
}
