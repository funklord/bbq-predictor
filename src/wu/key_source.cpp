#include "wu/key_source.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <thread>
#endif

#include <QNetworkRequest>
#include <QPointer>
#include <QCoreApplication>
#include <QTimer>
#include <QRegularExpression>
#include <QUrl>

namespace {

/*
 * How many transfers one acquisition may spend, and how long it waits
 * between them (sec 2.6.1.1). Three by the copyright holder's
 * instruction, after the page was measured refusing half the time.
 */
const int key_attempts = 3;
const int key_retry_pause_ms = 900;
const int key_timeout_ms = 15000;

/*
 * THE PAGES THE KEY IS TAKEN FROM, ONE PER ATTEMPT (sec 2.6.1.4).
 *
 * WU's CDN hands any page out either fully rendered, with the request
 * URLs a key is read from, or as a shell without them, and which a page
 * gets changes within minutes: the hourly page carried a key 3 times of 3
 * and twenty minutes later 0 of 6. So one page asked three times was
 * three draws on whatever that page was serving at the time, and three
 * pages are three different draws. Ordered by the tally over two
 * evenings: /forecast about half, the hourly page sometimes, the city
 * weather page one in three. The home page, a station's dashboard,
 * history, maps and /10day carried it in none.
 *
 * The city is incidental: only the embedded request URLs are read, never
 * anything about the place, so Stockholm serves a client anywhere.
 */
const char *const key_pages[] = {
	"https://www.wunderground.com/forecast",
	"https://www.wunderground.com/hourly/se/stockholm",
	"https://www.wunderground.com/weather/se/stockholm",
};

/* Attempt 1 asks the first page, 2 the second, and round again past the end. */
const char *key_page(int attempt) {
	const int count = int(sizeof key_pages / sizeof key_pages[0]);
	return key_pages[(attempt > 0 ? attempt - 1 : 0) % count];
}

/*
 * A browser User-Agent, and the honesty of this is worth stating rather
 * than leaving as an unexplained string.
 *
 * Identifying ourselves truthfully would be the better manners, and it
 * is what a well-behaved client does -- MET Norway, one of the
 * candidate providers in sec 2.8, requires exactly that. But the page
 * this fetches is served to browsers, and the whole approach is already
 * the compromised path recorded in sec 2.2. Pretending otherwise here
 * would not make it less so.
 *
 * A provider reached with a legitimate key gets a truthful agent.
 */
const char *const browser_agent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

} // namespace

bbq_wu_key_source::bbq_wu_key_source(QNetworkAccessManager *net,
                                     QObject *parent)
        : QObject(parent), m_net(net) {
}

void bbq_wu_key_source::invalidate() {
	if (m_key.isEmpty()) {
		return;
	}

	m_key.clear();
	emit invalidated();
}

bool bbq_wu_key_source::remember(const QString &key) {
	static const QRegularExpression shape(QStringLiteral("^[0-9a-f]{32}$"));

	if (!shape.match(key).hasMatch()) {
		return false;
	}

	m_key = key;
	return true;
}

void bbq_wu_key_source::acquire() {
	if (m_in_flight) {
		return;
	}

	m_attempts = 0;
	send();
}

void bbq_wu_key_source::send() {
	m_in_flight = true;
	++m_attempts;
	++m_sent;

#ifdef Q_OS_ANDROID
	/*
	 * THROUGH THE PLATFORM ON ANDROID (sec 2.6.1.2).
	 *
	 * WU answers this program's Qt requests with 404 while the phone's
	 * own curl gets 200 from the same wifi seconds apart. The client is
	 * what differs, and the largest difference on Android is that this
	 * project ships its own OpenSSL rather than using the platform's.
	 * HttpURLConnection uses the platform stack.
	 *
	 * ON A WORKER THREAD, because Android throws
	 * NetworkOnMainThreadException for exactly this, and because a
	 * synchronous fetch on the UI thread would freeze the window for as
	 * long as the page takes. The answer is posted back to this
	 * object's thread, and guarded by a QPointer: the source can be
	 * destroyed while a page is in flight, and a reply to a deleted
	 * object is a crash rather than a wasted fetch.
	 */
	QPointer<bbq_wu_key_source> alive(this);
	const char *const address = key_page(m_attempts);

	std::thread([alive, address]() {
		const QJniObject page = QJniObject::callStaticObjectMethod(
		        "se/vibes/bbq_predictor/PageFetch", "get",
		        "(Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;",
		        QJniObject::fromString(QString::fromLatin1(address))
		                .object<jstring>(),
		        QJniObject::fromString(QString::fromLatin1(browser_agent))
		                .object<jstring>(),
		        jint(key_timeout_ms));

		const QString body = page.isValid() ? page.toString() : QString();

		QMetaObject::invokeMethod(
		        qApp,
		        [alive, body]() {
			if (alive) {
				alive->page_arrived(body);
			}
		},
		        Qt::QueuedConnection);
	}).detach();
#else
	QNetworkRequest request((QUrl(QString::fromLatin1(key_page(m_attempts)))));
	request.setHeader(QNetworkRequest::UserAgentHeader,
	                  QString::fromLatin1(browser_agent));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                     QNetworkRequest::NoLessSafeRedirectPolicy);

	QNetworkReply *reply = m_net->get(request);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		/*
		 * THE BODY OF AN ERROR TOO, WHEN THE SERVER SENT ONE
		 * (sec 2.6.1.5). WU's 404 page carries the site's API_KEY like
		 * every other page it serves, and /forecast answers 404 most of
		 * the time now, so discarding error bodies threw away a key that
		 * had arrived. Only a reply with no HTTP status -- refused, timed
		 * out, never reached a server -- is a transfer with nothing in it.
		 */
		const int status =
		        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QString body = reply->error() == QNetworkReply::NoError || status > 0
		                             ? QString::fromUtf8(reply->readAll())
		                             : QString();

		m_transfer_error =
		        reply->error() == QNetworkReply::NoError
		                ? QString()
		                : bbq_wu_transfer_error(
		                          reply->errorString(),
		                          reply->attribute(
		                                       QNetworkRequest::
		                                               HttpStatusCodeAttribute)
		                                  .toInt());

		page_arrived(body);
	});
#endif
}

QString bbq_wu_transfer_error(const QString &qt_message, int http_status) {
	if (http_status <= 0) {
		/*
		 * No status at all: the transfer never reached a server, so
		 * Qt's text is the whole of what is known.
		 */
		return qt_message;
	}

	/*
	 * A trailing "server replied:" with nothing after it is Qt telling
	 * the reader the server said nothing. Said plainly rather than left
	 * dangling, and the status carries the meaning either way.
	 */
	QString said = qt_message.trimmed();
	if (said.endsWith(QStringLiteral("server replied:"))) {
		said.chop(QStringLiteral(" server replied:").size());
		said = said.trimmed();
	}

	return said + QStringLiteral(" (HTTP %1)").arg(http_status);
}

void bbq_wu_key_source::page_arrived(const QString &page) {
	m_in_flight = false;

	const QString key = page.isEmpty() ? QString() : extract_key(page);

	if (key.isEmpty()) {
		/*
		 * RETRIED, because the page refuses intermittently
		 * (sec 2.6.1.1). Measured: three of six consecutive attempts
		 * failed against a working network, while curl fetched the same
		 * page every time -- so a single refusal says nothing about
		 * whether the next one will succeed.
		 *
		 * A page that arrives WITHOUT a key is retried too, since
		 * 2026-10-22 (sec 2.6.1.3). This said asking again would return
		 * the same page, and it does not: WU alternates between the full
		 * render and a shell with no weather key, and ten spaced fetches
		 * that evening carried the key once. The three-attempt budget is
		 * unchanged, so a page that really has reorganised costs three
		 * fetches and the same diagnosis.
		 */
		if (m_attempts < key_attempts) {
			/*
			 * A pause between tries rather than a burst. This is
			 * somebody else's quota (sec 2.5), and three requests in
			 * the same millisecond is the shape of thing that gets an
			 * address blocked rather than served.
			 */
			QTimer::singleShot(key_retry_pause_ms, this,
			                   [this]() { send(); });
			return;
		}

		if (page.isEmpty()) {
			emit failed(m_transfer_error.isEmpty()
			                    ? tr("the key page could not be fetched")
			                    : m_transfer_error);
			return;
		}

		/*
		 * Said as what it is: the page arrived and carried no key a
		 * weather endpoint was seen using. That is a different thing
		 * from the network being down, so it must not share a message
		 * with it -- and it is not necessarily the pattern's fault,
		 * which is what this said until the shell page showed a page
		 * can lack the key while the pattern is fine.
		 */
		emit failed(tr("the key page carried no weather key on %n attempt(s)", nullptr,
		               m_attempts));
		return;
	}

	m_key = key;
	emit acquired(m_key);
}

QString bbq_wu_key_source::extract_key(const QString &page) {
	/*
	 * Prefer a key attached to a weather endpoint. The page carries
	 * several 32-hex values and they do not all work on the endpoints
	 * this program wants (sec 2.6.1), so the URL path is the evidence
	 * that a particular one does.
	 */
	static const QRegularExpression weather_url(
	        QStringLiteral("api\\.weather\\.com/(?:v[23])/[^\"'\\s]*?"
	                       "apiKey=([0-9a-f]{32})"));

	const QRegularExpressionMatch match = weather_url.match(page);
	if (match.hasMatch()) {
		return match.captured(1);
	}

	/*
	 * FAILING THAT, THE SITE'S OWN "API_KEY", BY THAT NAME AND NO OTHER
	 * (sec 2.6.1.5). The shell WU serves most of the time now carries no
	 * request URLs, and does carry `const data = {"API_KEY":"..."}` --
	 * a different key from the URLs' one, measured 2026-10-23 answering
	 * 200 on every endpoint this program calls, the station's current
	 * and history, hourly, fifteen-minute, point current and both
	 * discovery endpoints. In twenty spaced fetches of four pages, every
	 * page carried one of the two.
	 *
	 * By name, because the same blob holds other 32-hex values -- a
	 * payment page id, an ad script, a place id -- all refused by the
	 * weather endpoints, and those are the trap sec 2.6.1 refused the
	 * blob outright for, when the key it looked at there did not work.
	 * This one does, and is the only one taken.
	 */
	static const QRegularExpression site_key(
	        QStringLiteral("\"API_KEY\"\\s*:\\s*\"([0-9a-f]{32})\""));
	const QRegularExpressionMatch named = site_key.match(page);
	if (named.hasMatch()) {
		return named.captured(1);
	}

	/*
	 * NOTHING ELSE, and there was something else until 2026-10-22
	 * (sec 2.6.1.3): any apiKey= in any URL. WU began serving a 99 KB
	 * shell of its pages most of the time, whose one key belongs to an
	 * advertising-config URL on weather.com and is refused by every
	 * weather endpoint. The fallback took it, every station request
	 * came back 401, and the round said "Host requires authentication"
	 * where the truth was "the page carried no weather key".
	 *
	 * Which is the failure sec 2.6.1 warned of, one level out: a
	 * fallback that silently supplies a broken key is worse than
	 * failing, because it turns "the scrape broke" into "the endpoints
	 * all return 401 for no visible reason". The blob's other named
	 * values stay refused for the same reason.
	 */
	return QString();
}
