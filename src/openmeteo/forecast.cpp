#include "openmeteo/forecast.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimeZone>
#include <QUrl>

#include <vector>

namespace {

const char *const provider_name = "open-meteo";

const char *const agent = "bbq-predictor/0.1 (funklord@vibes.se)";

/*
 * timezone=auto is asked for deliberately, and it costs a conversion.
 *
 * The alternative is timezone=UTC, which returns unambiguous stamps and
 * needs no zone at all. But `auto` also names the location's IANA zone
 * in the response, and that is worth having: without a pinned station
 * this is the only provider here that supplies a real zone name rather
 * than the bare offset a WU forecast implies (sec 3.12.1).
 */
/*
 * The HOURLY block, not the fifteen-minute one, and that is a
 * correction rather than a preference (project.md sec 2.10.4).
 *
 * Open-Meteo will serve `minutely_15` anywhere, but it is genuine only
 * where a native fifteen-minute model reaches -- HRRR over North
 * America, ICON-D2 and AROME over central Europe -- and is interpolated
 * from hourly everywhere else. Their own API says so plainly: asking
 * for models=icon_d2 at this project's station answers "No data is
 * available for this location", while Berlin returns a full series.
 *
 * Presenting that interpolation as quarter-hourly samples would put
 * marks on the graph at points nobody measured, which is precisely what
 * sec 3.11.3 says the marks exist to rule out. Sixteen days of honest
 * hourly beats seven days of hourly wearing a finer spacing.
 */
const char *const endpoint =
    "https://api.open-meteo.com/v1/forecast"
    "?hourly=temperature_2m,precipitation,wind_speed_10m,"
    "precipitation_probability"
    "&timezone=auto&forecast_days=16";

/*
 * One model's quarter-hourly block, three days of it: the native models
 * reach about two and a half, and the rest of the request would be
 * nulls.
 */
const char *const regional_endpoint =
    "https://api.open-meteo.com/v1/forecast"
    "?minutely_15=temperature_2m,precipitation,wind_speed_10m,"
    "precipitation_probability"
    "&timezone=auto&forecast_days=3";

std::optional<double> number_at(const QJsonArray &array, int index) {
	if (index < 0 || index >= array.size()) {
		return std::nullopt;
	}

	const QJsonValue value = array.at(index);
	if (!value.isDouble()) {
		return std::nullopt;
	}

	return value.toDouble();
}

} // namespace

bbq_openmeteo_client::bbq_openmeteo_client(QNetworkAccessManager *net,
                                           QObject *parent)
        : QObject(parent), m_net(net) {
}

void bbq_openmeteo_client::fetch(double latitude, double longitude) {
	QString target = QString::fromLatin1(endpoint);
	target += QStringLiteral("&latitude=");
	target += QString::number(latitude, 'f', 4);
	target += QStringLiteral("&longitude=");
	target += QString::number(longitude, 'f', 4);

	QNetworkRequest request((QUrl(target)));
	request.setHeader(QNetworkRequest::UserAgentHeader,
	                  QString::fromLatin1(agent));

	QNetworkReply *reply = m_net->get(request);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			emit failed(reply->errorString());
			return;
		}

		QJsonParseError parse_error;
		const QJsonDocument document =
		        QJsonDocument::fromJson(reply->readAll(), &parse_error);

		if (document.isNull()) {
			emit failed(tr("malformed response: %1")
			                    .arg(parse_error.errorString()));
			return;
		}

		emit ready(document);
	});
}

const char *const bbq_openmeteo_regional_models[3] = {
	"ncep_hrrr_conus",
	"meteofrance_arome_france_hd",
	"icon_d2",
};

void bbq_regional_probe::at(double latitude, double longitude) {
	const QString place = QString::number(latitude, 'f', 2) + QLatin1Char(',') +
	                      QString::number(longitude, 'f', 2);
	if (place != m_place) {
		m_place = place;
		m_model.clear();
		m_next = 0;
	}
}

QString bbq_regional_probe::to_ask() const {
	if (!m_model.isEmpty()) {
		return m_model;
	}
	if (m_next >= 3) {
		return QString();
	}
	return QString::fromLatin1(bbq_openmeteo_regional_models[m_next]);
}

void bbq_regional_probe::answered(const QString &model) {
	m_model = model;
}

bool bbq_regional_probe::outside(const QString &model) {
	int asked = 0;
	while (asked < 3 && model != QLatin1String(bbq_openmeteo_regional_models[asked])) {
		++asked;
	}

	/*
	 * A model that answered before and is outside now -- the point moved
	 * less than the place key notices, at a domain's edge -- starts the
	 * asking again after it rather than being kept.
	 */
	m_model.clear();
	m_next = asked + 1;
	return m_next < 3;
}

void bbq_openmeteo_client::fetch_regional(double latitude, double longitude,
                                          const QString &model) {
	QString target = QString::fromLatin1(regional_endpoint);
	target += QStringLiteral("&models=");
	target += model;
	target += QStringLiteral("&latitude=");
	target += QString::number(latitude, 'f', 4);
	target += QStringLiteral("&longitude=");
	target += QString::number(longitude, 'f', 4);

	QNetworkRequest request((QUrl(target)));
	request.setHeader(QNetworkRequest::UserAgentHeader,
	                  QString::fromLatin1(agent));

	QNetworkReply *reply = m_net->get(request);

	connect(reply, &QNetworkReply::finished, this, [this, reply, model]() {
		reply->deleteLater();

		const int status =
		        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray body = reply->readAll();

		/*
		 * Out of the model's domain is a 400 with a reason, measured
		 * 2026-11-13 at Stockholm for all three. Any other failure says
		 * nothing about the place, so it is not remembered as one.
		 */
		if (status == 400) {
			const QString reason = QJsonDocument::fromJson(body)
			                               .object()
			                               .value(QStringLiteral("reason"))
			                               .toString();
			emit regional_failed(model, true, reason);
			return;
		}

		if (reply->error() != QNetworkReply::NoError) {
			emit regional_failed(model, false, reply->errorString());
			return;
		}

		QJsonParseError parse_error;
		const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
		if (document.isNull()) {
			emit regional_failed(model, false,
			                     tr("malformed response: %1")
			                             .arg(parse_error.errorString()));
			return;
		}

		emit regional_ready(model, document);
	});
}

namespace {

/*
 * A block of rows in the response's own zone, shared by both bands.
 * `while_measured` stops at the first row with no temperature, which is
 * where a regional model's horizon ends.
 */
bbq_series read_block(const QJsonDocument &response, const QString &block_name,
                      int step_s, bbq_band band, const QString &provider,
                      bool while_measured) {
	bbq_series series(band, provider);

	if (!response.isObject()) {
		return series;
	}

	const QJsonObject root = response.object();

	/*
	 * The zone first, because nothing else can be read without it. An
	 * unknown zone is a discarded band rather than a guessed one: the
	 * stamps carry no offset, so reading them in the wrong zone would
	 * shift the whole series by hours while looking perfectly valid.
	 */
	const QString zone_name = root.value(QStringLiteral("timezone")).toString();

	/*
	 * The emptiness is checked BEFORE the zone is constructed, and that
	 * order is the whole of it.
	 *
	 * QTimeZone(QByteArray("")) does not produce an invalid zone. It
	 * produces the LOCAL one, and reports itself valid -- so a response
	 * with a missing or empty timezone would have been read in the
	 * viewer's clock, silently shifting the entire series and putting
	 * back the exact defect sec 3.12.1 removed. In the one provider
	 * whose stamps carry no offset to contradict it.
	 *
	 * A wrong name is caught by isValid; an absent one is not caught by
	 * anything, which is why it is caught here.
	 */
	if (zone_name.isEmpty()) {
		return series;
	}

	const QTimeZone zone(zone_name.toUtf8());
	if (!zone.isValid()) {
		return series;
	}

	const QJsonObject block = root.value(block_name).toObject();
	const QJsonArray times = block.value(QStringLiteral("time")).toArray();
	if (times.isEmpty()) {
		return series;
	}

	const QJsonArray temperatures =
	        block.value(QStringLiteral("temperature_2m")).toArray();
	const QJsonArray rain =
	        block.value(QStringLiteral("precipitation")).toArray();
	const QJsonArray wind =
	        block.value(QStringLiteral("wind_speed_10m")).toArray();
	const QJsonArray chance =
	        block.value(QStringLiteral("precipitation_probability")).toArray();

	std::vector<bbq_sample> samples;
	samples.reserve(times.size());

	for (int i = 0; i < times.size(); ++i) {
		QDateTime when = QDateTime::fromString(times.at(i).toString(),
		                                       Qt::ISODate);
		if (!when.isValid()) {
			return bbq_series(band, provider);
		}

		/*
		 * Parsed as naive and then placed in the zone, which is what
		 * makes daylight saving come out right. Adding a fixed offset
		 * would be correct until the transition and silently an hour
		 * out afterwards, in a series long enough to contain one.
		 */
		when.setTimeZone(zone);

		bbq_sample sample;
		sample.start_utc = when.toSecsSinceEpoch();
		sample.duration_s = step_s;
		sample.temperature = number_at(temperatures, i);
		if (while_measured && !sample.temperature.has_value()) {
			break;
		}
		sample.wind_kph = number_at(wind, i);
		sample.precip_chance = number_at(chance, i);

		/* Millimetres over the step, which the model stores as a rate. */
		const std::optional<double> fell = number_at(rain, i);
		if (fell.has_value()) {
			sample.precip_rate = bbq_rate_from_accumulation(*fell, step_s);
		}

		samples.push_back(sample);
	}

	series.set_zone(zone);
	series.set_samples(std::move(samples));
	return series;
}

} // namespace

bbq_series bbq_openmeteo_read(const QJsonDocument &response) {
	return read_block(response, QStringLiteral("hourly"), 3600, bbq_band::extended,
	                  QString::fromLatin1(provider_name), false);
}

bbq_series bbq_openmeteo_read_regional(const QJsonDocument &response,
                                       const QString &model) {
	return read_block(response, QStringLiteral("minutely_15"), 900, bbq_band::regional,
	                  QString::fromLatin1(provider_name) + QLatin1Char(' ') + model,
	                  true);
}
