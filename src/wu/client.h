#ifndef BBQ_WU_CLIENT_H
#define BBQ_WU_CLIENT_H

#include <QJsonDocument>
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

signals:
	/*
	 * Raw response for a product. Undigested by design -- see above.
	 */
	void ready(bbq_wu_product product, const QJsonDocument &response);

	/*
	 * A product could not be fetched. Bands fail independently: a dead
	 * station must not take the forecast bands down with it, which is
	 * sec 2.6.6's promise and sec 2.6.7.2's reason for caching the
	 * derived geocode.
	 */
	void failed(bbq_wu_product product, const QString &reason);

private:
	void send(bbq_wu_product product, const QString &path,
	          const QString &query, bool may_retry);

	QNetworkAccessManager *m_net;
	bbq_wu_key_source *m_keys;
};

#endif
