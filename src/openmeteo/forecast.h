#ifndef BBQ_OPENMETEO_FORECAST_H
#define BBQ_OPENMETEO_FORECAST_H

#include <QJsonDocument>
#include <QObject>
#include <QString>

#include "model/series.h"

class QNetworkAccessManager;

/*
 * Open-Meteo (project.md sec 2.10): the extended band, hourly for
 * sixteen days, and the regional band, a native fine-scale model's own
 * quarter hours where one reaches the point (sec 2.10.5).
 *
 * Both carry temperature, precipitation, wind AND probability, so a
 * band that outranks the hourly one does not quietly drop the fields
 * sec 7 scores on.
 */
class bbq_openmeteo_client : public QObject {
	Q_OBJECT

public:
	explicit bbq_openmeteo_client(QNetworkAccessManager *net,
	                              QObject *parent = nullptr);

	void fetch(double latitude, double longitude);

	/*
	 * One native quarter-hourly model's own forecast (sec 2.10.5), named
	 * from bbq_openmeteo_regional_models. Asked one model at a time:
	 * several at once came back suffixed for one place, unsuffixed for
	 * another, and refused outright where any was out of reach.
	 */
	void fetch_regional(double latitude, double longitude, const QString &model);

signals:
	void ready(const QJsonDocument &response);
	void failed(const QString &reason);

	void regional_ready(const QString &model, const QJsonDocument &response);

	/*
	 * `outside` is the provider saying the point is beyond the model's
	 * domain -- HTTP 400, "No data is available for this location" --
	 * which is an answer about the place, not a fault to retry.
	 */
	void regional_failed(const QString &model, bool outside, const QString &reason);

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

/*
 * The models whose quarter-hourly output is their own rather than
 * interpolated from hourly (project.md sec 2.10.4), finest first where
 * two overlap: HRRR over North America, AROME's 1.3 km run over France
 * and its neighbours, ICON-D2 over central Europe.
 */
extern const char *const bbq_openmeteo_regional_models[3];

/*
 * Which of those models reaches a place, learned by asking (sec 2.10.5).
 *
 * The provider is asked one model at a time, in the order above, until
 * one answers with data or all three say the place is outside them. The
 * answer is kept for the place it was learned at and forgotten when the
 * place moves. "None" is kept too: it is the answer at this project's
 * own station, and without it every refresh would ask three times.
 */
class bbq_regional_probe {
public:
	/* The place the next request is for; a new one forgets the answer. */
	void at(double latitude, double longitude);

	/* The model to ask now, or empty when none reaches this place. */
	QString to_ask() const;

	/* It answered with data: ask it, and only it, from now on. */
	void answered(const QString &model);

	/* It said the place is outside it. True when another is left to ask. */
	bool outside(const QString &model);

private:
	QString m_place;
	QString m_model;
	int m_next = 0;
};

/*
 * A regional model's `minutely_15` block, as the regional band.
 *
 * Rows are taken while the model has a temperature for them and no
 * further: past its horizon (about 54 hours for HRRR, 62 for the
 * European two) the block carries nulls, or a column the provider has
 * filled from elsewhere -- HRRR's rain chance runs the full three days
 * though HRRR forecasts no chance at all. Precipitation is millimetres
 * per quarter hour and becomes a rate.
 */
bbq_series bbq_openmeteo_read_regional(const QJsonDocument &response,
                                       const QString &model);

#endif
