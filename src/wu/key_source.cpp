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
 * The page the key is taken from. Any forecast page carries the
 * server-rendered payload this needs; the location in it is irrelevant,
 * since only the embedded request URLs are read.
 */
/*
 * How many transfers one acquisition may spend, and how long it waits
 * between them (sec 2.6.1.1). Three by the copyright holder's
 * instruction, after the page was measured refusing half the time.
 */
const int key_attempts = 3;
const int key_retry_pause_ms = 900;
const int key_timeout_ms = 15000;

const char *const key_page_url = "https://www.wunderground.com/forecast";

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
	m_key.clear();
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

	std::thread([alive]() {
		const QJniObject page = QJniObject::callStaticObjectMethod(
		        "se/vibes/bbq_predictor/PageFetch", "get",
		        "(Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;",
		        QJniObject::fromString(QString::fromLatin1(key_page_url))
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
	QNetworkRequest request((QUrl(QString::fromLatin1(key_page_url))));
	request.setHeader(QNetworkRequest::UserAgentHeader,
	                  QString::fromLatin1(browser_agent));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                     QNetworkRequest::NoLessSafeRedirectPolicy);

	QNetworkReply *reply = m_net->get(request);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		const QString body = reply->error() == QNetworkReply::NoError
		                             ? QString::fromUtf8(reply->readAll())
		                             : QString();

		m_transfer_error = reply->error() == QNetworkReply::NoError
		                           ? QString()
		                           : reply->errorString();

		page_arrived(body);
	});
#endif
}

void bbq_wu_key_source::page_arrived(const QString &page) {
	m_in_flight = false;

	if (page.isEmpty()) {
		/*
		 * RETRIED, because the page refuses intermittently
		 * (sec 2.6.1.1). Measured: three of six consecutive attempts
		 * failed against a working network, while curl fetched the same
		 * page every time -- so a single refusal says nothing about
		 * whether the next one will succeed.
		 *
		 * Only a TRANSFER failure is retried. A page that arrives and
		 * carries no key is the failure sec 2.2 predicted, and asking
		 * for the same page again would return the same page --
		 * retrying that would turn a clear diagnosis into three of
		 * them.
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

		emit failed(m_transfer_error.isEmpty()
		                    ? tr("the key page could not be fetched")
		                    : m_transfer_error);
		return;
	}

	const QString key = extract_key(page);

	if (key.isEmpty()) {
		/*
		 * Said as what it is. This is the failure sec 2.2 predicted --
		 * the page reorganised, or the key moved -- and it is a
		 * different thing from the network being down, so it must not
		 * share a message with it.
		 */
		emit failed(tr("no API key found in the page; the extraction "
		               "pattern has stopped matching"));
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

	QRegularExpressionMatch match = weather_url.match(page);
	if (match.hasMatch()) {
		return match.captured(1);
	}

	/*
	 * Failing that, any apiKey= in a URL at all. Still a key the site
	 * used, just not one seen against a path we recognise -- which is
	 * what a reorganisation would look like before anybody has updated
	 * the pattern above.
	 *
	 * Deliberately NOT falling back to the config blob's named keys.
	 * Those are the ones that look right and do not work, and a
	 * fallback that silently supplies a broken key is worse than
	 * failing: it turns "the scrape broke" into "the endpoints all
	 * return 401 for no visible reason".
	 */
	static const QRegularExpression any_url(
	        QStringLiteral("apiKey=([0-9a-f]{32})"));

	match = any_url.match(page);
	if (match.hasMatch()) {
		return match.captured(1);
	}

	return QString();
}
