#include "wu/key_source.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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

	QNetworkRequest request((QUrl(QString::fromLatin1(key_page_url))));
	request.setHeader(QNetworkRequest::UserAgentHeader,
	                  QString::fromLatin1(browser_agent));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                     QNetworkRequest::NoLessSafeRedirectPolicy);

	QNetworkReply *reply = m_net->get(request);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();
		m_in_flight = false;

		if (reply->error() != QNetworkReply::NoError) {
			/*
			 * RETRIED, because the page refuses intermittently
			 * (sec 2.6.1.1). Measured: three of six consecutive
			 * attempts failed against a working network, while curl
			 * fetched the same page every time -- so a single refusal
			 * says nothing about whether the next one will succeed.
			 *
			 * Only a TRANSFER failure is retried. A page that arrives
			 * and carries no key is the failure sec 2.2 predicted, and
			 * asking for the same page again would return the same page
			 * -- retrying that would turn a clear diagnosis into three
			 * of them.
			 */
			if (m_attempts < key_attempts) {
				/*
				 * A pause between tries rather than a burst. This is
				 * somebody else's quota (sec 2.5), and three requests
				 * in the same millisecond is the shape of thing that
				 * gets an address blocked rather than served.
				 */
				QTimer::singleShot(key_retry_pause_ms, this,
				                   [this]() { send(); });
				return;
			}

			emit failed(reply->errorString());
			return;
		}

		const QString page = QString::fromUtf8(reply->readAll());
		const QString key = extract_key(page);

		if (key.isEmpty()) {
			/*
			 * Said as what it is. This is the failure sec 2.2
			 * predicted -- the page reorganised, or the key moved --
			 * and it is a different thing from the network being
			 * down, so it must not share a message with it.
			 */
			emit failed(tr("no API key found in the page; the "
			               "extraction pattern has stopped matching"));
			return;
		}

		m_key = key;
		emit acquired(m_key);
	});
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
