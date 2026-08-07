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

const char *const agent = "bbqpredictor/0.1 (funklord@vibes.se)";

/*
 * timezone=auto is asked for deliberately, and it costs a conversion.
 *
 * The alternative is timezone=UTC, which returns unambiguous stamps and
 * needs no zone at all. But `auto` also names the location's IANA zone
 * in the response, and that is worth having: without a pinned station
 * this is the only provider here that supplies a real zone name rather
 * than the bare offset a WU forecast implies (sec 3.12.1).
 */
const char *const endpoint =
	"https://api.open-meteo.com/v1/forecast"
	"?minutely_15=temperature_2m,precipitation,wind_speed_10m,"
	"precipitation_probability"
	"&timezone=auto&forecast_days=7";

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

bbq_series bbq_openmeteo_read(const QJsonDocument &response) {
	bbq_series series(bbq_band::extended, QString::fromLatin1(provider_name));

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
	const QTimeZone zone(zone_name.toUtf8());
	if (!zone.isValid()) {
		return series;
	}

	const QJsonObject block =
	        root.value(QStringLiteral("minutely_15")).toObject();
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
			return bbq_series(bbq_band::extended,
			                  QString::fromLatin1(provider_name));
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
		sample.duration_s = 900;
		sample.temperature = number_at(temperatures, i);
		sample.wind_kph = number_at(wind, i);
		sample.precip_chance = number_at(chance, i);

		/* Millimetres per quarter hour, which the model stores as a rate. */
		const std::optional<double> fell = number_at(rain, i);
		if (fell.has_value()) {
			sample.precip_rate = bbq_rate_from_accumulation(*fell, 900);
		}

		samples.push_back(sample);
	}

	series.set_zone(zone);
	series.set_samples(std::move(samples));
	return series;
}
