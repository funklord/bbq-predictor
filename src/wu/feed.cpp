#include "wu/feed.h"

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>

#include "wu/client.h"
#include "wu/key_source.h"
#include "wu/reader.h"

namespace {

bbq_series read_for(bbq_wu_product product, const QJsonDocument &document) {
	switch (product) {
	case bbq_wu_product::observed:
		return bbq_wu_read_observed(document);
	case bbq_wu_product::current_station:
		return bbq_wu_read_current_station(document);
	case bbq_wu_product::current_point:
		return bbq_wu_read_current_point(document);
	case bbq_wu_product::nowcast:
		return bbq_wu_read_nowcast(document);
	case bbq_wu_product::hourly:
		return bbq_wu_read_hourly(document);
	}

	return bbq_series();
}

} // namespace

bbq_wu_feed::bbq_wu_feed(QObject *parent) : QObject(parent) {
	m_net = new QNetworkAccessManager(this);
	m_keys = new bbq_wu_key_source(m_net, this);
	m_client = new bbq_wu_client(m_net, m_keys, this);

	connect(m_client, &bbq_wu_client::ready, this,
	        [this](bbq_wu_product product, const QJsonDocument &document) {
		bbq_series series = read_for(product, document);
		series.set_fetched_utc(QDateTime::currentSecsSinceEpoch());

		const bool was_observed = product == bbq_wu_product::observed;
		const bool need_geocode = !m_have_geocode;

		m_composite.set_series(std::move(series));

		/*
		 * The station supplies the coordinate the forecast bands need
		 * (sec 2.6.7.1), so they wait for it rather than for a lookup.
		 */
		if (was_observed && need_geocode) {
			const QJsonArray rows = document.object()
			                                .value(QStringLiteral("observations"))
			                                .toArray();
			if (!rows.isEmpty()) {
				const QJsonObject first = rows.first().toObject();
				if (first.contains(QStringLiteral("lat"))) {
					m_latitude = first.value(QStringLiteral("lat")).toDouble();
					m_longitude = first.value(QStringLiteral("lon")).toDouble();
					m_have_geocode = true;
					start_forecast_bands();
				}
			}
		}

		emit updated();
		finish_one();
	});

	connect(m_client, &bbq_wu_client::failed, this,
	        [this](bbq_wu_product product, const QString &reason) {
		const QString name = QString::fromLatin1(bbq_wu_product_name(product));
		emit band_failed(name, reason);
		finish_one();
	});
}

void bbq_wu_feed::set_station(const QString &station_id) {
	m_station_id = station_id;
}

void bbq_wu_feed::set_geocode(double latitude, double longitude) {
	m_latitude = latitude;
	m_longitude = longitude;
	m_have_geocode = true;
}

void bbq_wu_feed::finish_one() {
	--m_outstanding;
	if (m_outstanding <= 0) {
		m_outstanding = 0;
		emit settled();
	}
}

void bbq_wu_feed::start_forecast_bands() {
	m_outstanding += 2;
	m_client->fetch_nowcast(m_latitude, m_longitude);
	m_client->fetch_hourly(m_latitude, m_longitude);
}

void bbq_wu_feed::refresh() {
	if (m_outstanding > 0) {
		return;
	}

	if (m_station_id.isEmpty() && !m_have_geocode) {
		emit band_failed(QStringLiteral("all"),
		                 tr("no station pinned and no geocode set"));
		emit settled();
		return;
	}

	if (!m_station_id.isEmpty()) {
		m_outstanding += 2;
		const QString today =
		        QDate::currentDate().toString(QStringLiteral("yyyyMMdd"));
		m_client->fetch_observed(m_station_id, today);
		m_client->fetch_current_station(m_station_id);
	}

	if (m_have_geocode) {
		start_forecast_bands();

		/*
		 * Only without a station: the station's current endpoint
		 * carries a real rain rate where this one carries none
		 * (sec 3.9.2).
		 */
		if (m_station_id.isEmpty()) {
			m_outstanding += 1;
			m_client->fetch_current_point(m_latitude, m_longitude);
		}
	}
}
