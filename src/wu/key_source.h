#ifndef BBQ_WU_KEY_SOURCE_H
#define BBQ_WU_KEY_SOURCE_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;

/*
 * Obtains the Weather Underground API key by scraping it out of
 * wunderground.com's own page (project.md sec 2.2, sec 2.6.1).
 *
 * This violates their terms of service and it will break. That was
 * chosen deliberately with the alternatives on the table; sec 2.2 has
 * the reasoning and this class is only where it happens.
 *
 * The key is RUNTIME STATE, never a build constant (sec 2.3). It
 * rotates, so a key compiled in turns the program into a brick on
 * whatever Tuesday that happens, with no fix the user can apply.
 * Everything here exists to make re-acquiring one cheap.
 */
/*
 * What to say when the key page could not be fetched.
 *
 * Qt's own text ends "- server replied: " and then whatever the server
 * said, which for a bare 404 from a CDN is NOTHING -- so the reader is
 * handed a sentence that stops at a colon and looks truncated. Seen on
 * this path, which sec 2.2 already documents as the fragile one:
 *
 *     no API key: Error transferring https://www.wunderground.com/
 *     forecast - server replied:
 *
 * The status is the actionable half and this program names it
 * everywhere else -- "no data (HTTP 204)", the probe's "HTTP 401" -- so
 * it names it here too. Out of the reply handler because a message is
 * worth testing and a network failure is not worth staging to test one.
 */
QString bbq_wu_transfer_error(const QString &qt_message, int http_status);

class bbq_wu_key_source : public QObject {
	Q_OBJECT

public:
	explicit bbq_wu_key_source(QNetworkAccessManager *net,
	                           QObject *parent = nullptr);

	/*
	 * The key held right now, or empty if none has been obtained. No
	 * fetch is started by asking.
	 */
	const QString &key() const { return m_key; }
	bool has_key() const { return !m_key.isEmpty(); }

	/*
	 * Fetch a page and extract a key from it. Does nothing if a request
	 * is already in flight, so a burst of callers costs one request
	 * (sec 2.5 -- a scraped key is somebody else's quota).
	 */
	void acquire();

	/*
	 * Throw the current key away. Call this on a 401, which is what a
	 * rotation looks like from the outside, then acquire() again.
	 */
	void invalidate();

	/*
	 * Hold a key kept from an earlier process, without fetching anything
	 * (sec 2.6.1.3). Refused unless it is 32 lowercase hex, so a damaged
	 * store cannot hand the endpoints a string that was never a key.
	 * Returns whether it was taken.
	 */
	bool remember(const QString &key);

	/* Key pages requested so far, by either transport (sec 15.14). */
	int sent() const { return m_sent; }

signals:
	void acquired(const QString &key);
	void failed(const QString &reason);

	/*
	 * A held key was thrown away, so whoever kept a copy of it should
	 * stop offering it to the next process.
	 */
	void invalidated();

private:
	/* One transfer of the key page; acquire() may spend several. */
	void send();

	/*
	 * What arrived, from whichever transport fetched it (sec 2.6.1.2).
	 * Empty means the page did not arrive, which is the retryable case;
	 * a page that arrives without a key is not.
	 */
	void page_arrived(const QString &page);

	/*
	 * Pull a key out of page HTML.
	 *
	 * Static and free of any network so it can be exercised against a
	 * saved page without touching the site.
	 *
	 * A key from an embedded REQUEST URL first: one the site itself just
	 * used against that endpoint family. Failing that, the config blob's
	 * field named API_KEY and no other (sec 2.6.1.5), which the shell
	 * WU now mostly serves carries in place of request URLs, and which
	 * answers on every endpoint this program calls. The blob's other
	 * values are refused as sec 2.6.1 refused the whole blob: they are
	 * plausible 32-hex strings, and the weather endpoints reject them.
	 */
	static QString extract_key(const QString &page);

	QNetworkAccessManager *m_net;
	QString m_key;
	bool m_in_flight = false;

	/*
	 * Transfers spent on the CURRENT acquisition (sec 2.6.1.1). The
	 * page refuses about half the time, and a refusal costs a whole
	 * round because nothing else in it can run without a key.
	 */
	int m_attempts = 0;
	int m_sent = 0;

	/* Qt's word for the last refusal, where Qt did the fetching. */
	QString m_transfer_error;

	friend class bbq_wu_key_source_test;

	/* Counts the attempts of sec 2.6.1.1, which nothing else exposes. */
	friend class test_client;
};

#endif
