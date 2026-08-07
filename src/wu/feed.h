#ifndef BBQ_WU_FEED_H
#define BBQ_WU_FEED_H

#include <QObject>
#include <QString>

#include "model/composite.h"

class QNetworkAccessManager;
class bbq_wu_key_source;
class bbq_wu_client;

/*
 * Keeps a bbq_composite fed from Weather Underground.
 *
 * The orchestration the diagnostic does by hand, made reusable and
 * asynchronous: no event loop of its own, so a window can own one and
 * stay responsive while the bands arrive.
 *
 * Bands land independently and the composite is updated as each does,
 * which is what sec 2.6.6 requires -- a dead station must not delay or
 * suppress the forecast bands, and an absent band is reported by name
 * rather than left as a hole.
 */
class bbq_wu_feed : public QObject {
	Q_OBJECT

public:
	explicit bbq_wu_feed(QObject *parent = nullptr);

	/*
	 * The pinned station (sec 2.6.5). Empty is a normal state: the
	 * forecast bands are geocode-keyed and need no station at all.
	 */
	void set_station(const QString &station_id);

	/*
	 * The forecast point. Derived from the station when one is pinned
	 * and no override is set (sec 2.6.7), so calling this is the
	 * override rather than the normal path.
	 */
	void set_geocode(double latitude, double longitude);

	void refresh();

	const bbq_composite &composite() const { return m_composite; }
	bool is_busy() const { return m_outstanding > 0; }

signals:
	/* A band arrived and the composite changed. */
	void updated();

	/* A band did not arrive. Named, because absent must not read as empty. */
	void band_failed(const QString &band, const QString &reason);

	/* Nothing further is outstanding, however it went. */
	void settled();

private:
	void start_forecast_bands();
	void finish_one();

	QNetworkAccessManager *m_net;
	bbq_wu_key_source *m_keys;
	bbq_wu_client *m_client;

	bbq_composite m_composite;
	QString m_station_id;
	double m_latitude = 0.0;
	double m_longitude = 0.0;
	bool m_have_geocode = false;
	int m_outstanding = 0;
};

#endif
