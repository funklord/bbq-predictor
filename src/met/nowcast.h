#ifndef BBQ_MET_NOWCAST_H
#define BBQ_MET_NOWCAST_H

#include <QJsonDocument>
#include <QObject>

#include "model/series.h"

class QNetworkAccessManager;

/*
 * MET Norway's radar nowcast, as a second provider for the sub-hourly
 * band (project.md sec 2.8, sec 2.9).
 *
 * This is the band that most needed one. Sec 2.6.4 records that WU's
 * fifteen-minute endpoint answers a scraped key but is called by none
 * of their own pages, so the single most important band in the project
 * rests on the endpoint least likely to survive -- and there was no
 * second source for it.
 *
 * Measured on 2026-08-07 and better on every axis that matters here:
 * five-minute steps against WU's fifteen, precipitation already in
 * mm/h, no key, and no terms being violated to read it.
 *
 * Its span is short -- under two hours against WU's seven -- which is
 * what a radar extrapolation honestly reaches. That is a reason to keep
 * both rather than to prefer one: they cover different distances into
 * the future.
 */
class bbq_met_client : public QObject {
	Q_OBJECT

public:
	explicit bbq_met_client(QNetworkAccessManager *net, QObject *parent = nullptr);

	void fetch_nowcast(double latitude, double longitude);

signals:
	void ready(const QJsonDocument &response);
	void failed(const QString &reason);

private:
	QNetworkAccessManager *m_net;
};

/*
 * Translate a nowcast response into this project's own series.
 *
 * Two conversions, both stated because both are the kind that fail
 * silently. Wind arrives in metres per second and the model stores
 * kilometres per hour. Time arrives as ISO 8601 with a Z, which is UTC
 * and needs no zone to read -- unlike WU's finest band, which carries
 * only local time (sec 2.6.2).
 *
 * No precipitation probability: the nowcast reports a rate and does not
 * claim a chance, so the field is left absent rather than invented.
 */
bbq_series bbq_met_read_nowcast(const QJsonDocument &response);

#endif
