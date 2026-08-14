#ifndef BBQ_WU_CLIENT_H
#define BBQ_WU_CLIENT_H

#include <QJsonDocument>
#include <QList>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class bbq_wu_key_source;

/*
 * Which request this is. A fetch-routing tag, NOT the storage model --
 * project.md sec 3's bands are a separate question, deliberately not
 * being answered yet.
 */
enum class bbq_wu_product {
	observed,
	current_station,
	current_point,
	nowcast,
	hourly,

	/*
	 * Discovery, not weather (sec 13). These two answer with a list of
	 * places rather than a series, so they never reach read_for() and
	 * arrive on their own signals -- a product that produced an empty
	 * bbq_series would be a band that silently never drew.
	 */
	nearby,
	place_search,

	/*
	 * A pinned station's history, which is NOT the watched station's
	 * (sec 13.4). Distinct from `observed` so the handler can tell them
	 * apart: they arrive on the same signal and archiving one under the
	 * other's id would file measurements against a station that never
	 * made them.
	 */
	observed_pinned,
};

const char *bbq_wu_product_name(bbq_wu_product product);

/*
 * Fetches raw responses from Weather Underground (project.md sec 2.6).
 *
 * Raw on purpose. This layer obtains what the service actually returns
 * and hands it over undigested; how it is stored and processed is the
 * next question and is not pre-empted here. The endpoints, their
 * cadences and their traps are recorded in sec 2.6 and were measured
 * rather than assumed.
 *
 * The key is acquired lazily through bbq_wu_key_source and re-acquired
 * once on a 401, which is what a rotation looks like from out here
 * (sec 2.3).
 */
class bbq_wu_client : public QObject {
	Q_OBJECT

public:
	bbq_wu_client(QNetworkAccessManager *net, bbq_wu_key_source *keys,
	              QObject *parent = nullptr);

	/*
	 * Geocode-keyed bands. Neither needs a station (sec 2.6.6): the
	 * observed band is the optional one.
	 */
	void fetch_hourly(double latitude, double longitude);
	void fetch_nowcast(double latitude, double longitude);

	/*
	 * The observed band, keyed by the pinned station (sec 2.6.5).
	 * `date` is yyyyMMdd, which is the spelling the endpoint wants.
	 */
	void fetch_observed(const QString &station_id, const QString &date);

	/*
	 * The same request for a station that is pinned rather than
	 * watched. One at a time, so the caller always knows whose answer
	 * this is (sec 13.4).
	 */
	void fetch_observed_pinned(const QString &station_id, const QString &date);

	/*
	 * The present moment (project.md sec 3.9). Two endpoints, and they
	 * are not equivalent -- measured on 2026-08-07:
	 *
	 *   the station's own current observation carries metric.precipRate,
	 *   an instantaneous rate, which is what the model stores;
	 *
	 *   the geocode one carries no rate at all, only precip1Hour and
	 *   its longer siblings, which are trailing accumulations and
	 *   cannot honestly become a rate for now.
	 *
	 * So the station path is the real one and the geocode path is a
	 * temperature-only fallback for a config with no station pinned.
	 */
	void fetch_current_station(const QString &station_id);
	void fetch_current_point(double latitude, double longitude);

	/*
	 * The personal weather stations around a coordinate, and the
	 * coordinates of a named place (sec 13). Discovery rather than
	 * measurement: the first turns "here" into a list to choose from,
	 * the second turns a typed place into a "here".
	 */
	void fetch_nearby(double latitude, double longitude);
	void fetch_places(const QString &query);

	/*
	 * How many requests are waiting for a key. Zero at rest.
	 *
	 * Exposed because it is the only externally visible consequence of
	 * the queue being drained correctly, and sec 2.3.1's defect was
	 * precisely a queue that refilled itself behind everybody's back.
	 */
	int waiting() const { return static_cast<int>(m_waiting.size()); }

signals:
	/*
	 * Raw response for a product. Undigested by design -- see above.
	 */
	void ready(bbq_wu_product product, const QJsonDocument &response);

	/* Discovery answers, kept off `ready` because they are not series. */
	void stations_ready(const QJsonDocument &response);
	void places_ready(const QJsonDocument &response);

	/*
	 * A product could not be fetched. Bands fail independently: a dead
	 * station must not take the forecast bands down with it, which is
	 * sec 2.6.6's promise and sec 2.6.7.2's reason for caching the
	 * derived geocode.
	 */
	void failed(bbq_wu_product product, const QString &reason);

private:
	/*
	 * A request that arrived before there was a key to send it with.
	 *
	 * Held in a list drained by whichever key-source signal arrives,
	 * rather than by a pair of per-request connections. Sec 2.3.1: the
	 * pair could only tear down the half that fired, so the other half
	 * survived and later re-sent a request that had already failed, or
	 * failed one that had already succeeded.
	 */
	struct pending {
		bbq_wu_product product = bbq_wu_product::hourly;
		QString path;
		QString query;
		bool may_retry = true;
	};

	void send(bbq_wu_product product, const QString &path,
	          const QString &query, bool may_retry);

	QNetworkAccessManager *m_net;
	bbq_wu_key_source *m_keys;
	QList<pending> m_waiting;
};

#endif
