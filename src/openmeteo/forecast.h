#ifndef BBQ_OPENMETEO_FORECAST_H
#define BBQ_OPENMETEO_FORECAST_H

#include <QJsonDocument>
#include <QObject>

#include "model/series.h"

class QNetworkAccessManager;

/*
 * Open-Meteo's quarter-hourly forecast (project.md sec 2.10).
 *
 * It exists to cover the stretch neither other provider reaches at any
 * useful resolution. MET's radar stops under two hours and WU's
 * fifteen-minute band stops at seven; this one runs quarter-hourly for
 * SEVEN DAYS, so the day after tomorrow stops being hourly.
 *
 * Complete, which was checked before it was designed around. The
 * fifteen-minute series carries temperature, precipitation, wind AND
 * probability -- so a band that outranks the hourly one does not
 * quietly drop the fields sec 7 scores on.
 */
class bbq_openmeteo_client : public QObject {
	Q_OBJECT

public:
	explicit bbq_openmeteo_client(QNetworkAccessManager *net,
	                              QObject *parent = nullptr);

	void fetch(double latitude, double longitude);

signals:
	void ready(const QJsonDocument &response);
	void failed(const QString &reason);

private:
	QNetworkAccessManager *m_net;
};

/*
 * Translate a response into this project's own series.
 *
 * Two things to know about its time. The stamps are LOCAL and carry no
 * offset -- "2026-08-07T00:00" and nothing more -- so they cannot be
 * read without the zone, which the response names separately. And they
 * are converted through that IANA zone rather than through the single
 * `utc_offset_seconds` the response also offers: a fixed offset is
 * wrong on the far side of a daylight-saving change, and this series
 * runs seven days, which is long enough to cross one.
 *
 * Precipitation is millimetres per quarter hour and becomes a rate.
 * Wind is already km/h, which is what the model stores.
 */
bbq_series bbq_openmeteo_read(const QJsonDocument &response);

#endif
