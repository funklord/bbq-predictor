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

signals:
	void acquired(const QString &key);
	void failed(const QString &reason);

private:
	/* One transfer of the key page; acquire() may spend several. */
	void send();

	/*
	 * Pull a key out of page HTML.
	 *
	 * Static and free of any network so it can be exercised against a
	 * saved page without touching the site.
	 *
	 * Takes the key from an embedded REQUEST URL rather than from the
	 * page's config blob, which is sec 2.6.1's finding and not a
	 * stylistic preference: the config object holds several named keys
	 * with different scopes, and one of them is a perfectly plausible
	 * 32-hex string that does not work on the weather endpoints. A key
	 * lifted from a URL is one the site itself just used successfully
	 * against that endpoint family.
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

	friend class bbq_wu_key_source_test;

	/* Counts the attempts of sec 2.6.1.1, which nothing else exposes. */
	friend class test_client;
};

#endif
