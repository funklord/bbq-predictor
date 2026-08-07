#include "wu/reader.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <vector>

namespace {

const char *const provider_name = "wunderground";

/*
 * Give every sample a duration, taken from the distance to the next one.
 *
 * Measured rather than assumed, because the observed band arrives at 288
 * and 306 second intervals against a nominal 300. A model that wrote
 * 300 into every duration would be quietly wrong about where each span
 * ends, and the error would show up as bands that do not quite line up
 * for reasons nobody could find.
 *
 * The final sample has no successor, so it takes the median of the
 * others -- the median rather than the mean so one gap cannot stretch
 * it. A lone sample gets the fallback, which is the only place a
 * nominal cadence is used at all.
 */
void fill_durations(std::vector<bbq_sample> &samples, int fallback_s) {
	if (samples.empty()) {
		return;
	}

	std::vector<qint64> strides;
	strides.reserve(samples.size());

	for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
		const qint64 stride = samples[i + 1].start_utc - samples[i].start_utc;
		samples[i].duration_s = static_cast<int>(stride);
		strides.push_back(stride);
	}

	if (strides.empty()) {
		samples.back().duration_s = fallback_s;
		return;
	}

	const std::size_t middle = strides.size() / 2;
	std::nth_element(strides.begin(), strides.begin() + middle, strides.end());
	samples.back().duration_s = static_cast<int>(strides[middle]);
}

/* A JSON number, or nothing where the field is absent or null. */
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

/*
 * The two column-oriented forecast bands differ only in how they spell
 * time and rain, so they share this.
 *
 * `rain_is_rate` is the whole of sec 3.2 in one flag: the nowcast
 * already reports mm/h, while the hourly band reports an accumulation
 * over its step and has to be divided by that step.
 */
struct column_spec {
	bbq_band band = bbq_band::hourly;
	QString rain_field;
	bool rain_is_rate = true;
	int fallback_step_s = 3600;
};

bbq_series read_columns(const QJsonDocument &response, const column_spec &spec) {
	bbq_series series(spec.band, QString::fromLatin1(provider_name));

	if (!response.isObject()) {
		return series;
	}

	const QJsonObject root = response.object();
	const QString temperature_key = QStringLiteral("temperature");
	const QJsonArray temperatures = root.value(temperature_key).toArray();
	const QJsonArray rain = root.value(spec.rain_field).toArray();

	/*
	 * Both forecast bands spell it the same way, so unlike temperature
	 * and rain this one needs no per-band spelling in column_spec.
	 */
	const QString chance_key = QStringLiteral("precipChance");
	const QJsonArray chance = root.value(chance_key).toArray();

	/*
	 * Time. The hourly band offers epoch seconds; the nowcast offers
	 * only a local string, and its offset is what makes that usable
	 * without the station's zone (sec 2.6.2).
	 */
	std::vector<qint64> starts;

	const QJsonArray utc = root.value(QStringLiteral("validTimeUtc")).toArray();
	if (!utc.isEmpty()) {
		starts.reserve(utc.size());
		for (const QJsonValue &value : utc) {
			starts.push_back(static_cast<qint64>(value.toDouble()));
		}
	} else {
		const QString local_key = QStringLiteral("validTimeLocal");
		const QJsonArray local = root.value(local_key).toArray();
		starts.reserve(local.size());
		for (const QJsonValue &value : local) {
			const QString text = value.toString();
			const QDateTime when = QDateTime::fromString(text, Qt::ISODate);
			if (!when.isValid()) {
				/*
				 * One unparseable timestamp discards the band. A
				 * series missing an arbitrary sample from its middle
				 * draws a gap that means nothing, and sec 2.6.6 can
				 * report an absent band but not a quietly short one.
				 */
				return series;
			}
			starts.push_back(when.toSecsSinceEpoch());
		}
	}

	if (starts.empty()) {
		return series;
	}

	std::vector<bbq_sample> samples;
	samples.reserve(starts.size());

	for (std::size_t i = 0; i < starts.size(); ++i) {
		bbq_sample sample;
		sample.start_utc = starts[i];
		sample.temperature = number_at(temperatures, static_cast<int>(i));
		samples.push_back(sample);
	}

	fill_durations(samples, spec.fallback_step_s);

	for (std::size_t i = 0; i < samples.size(); ++i) {
		samples[i].precip_chance = number_at(chance, static_cast<int>(i));
	}

	/* Rain second, because converting an accumulation needs the span. */
	for (std::size_t i = 0; i < samples.size(); ++i) {
		const std::optional<double> value = number_at(rain, static_cast<int>(i));
		if (!value.has_value()) {
			continue;
		}

		if (spec.rain_is_rate) {
			samples[i].precip_rate = value;
		} else {
			const int span = samples[i].duration_s;
			samples[i].precip_rate = bbq_rate_from_accumulation(*value, span);
		}
	}

	series.set_samples(std::move(samples));
	return series;
}

} // namespace

bbq_series bbq_wu_read_observed(const QJsonDocument &response) {
	bbq_series series(bbq_band::observed, QString::fromLatin1(provider_name));

	if (!response.isObject()) {
		return series;
	}

	const QJsonObject root = response.object();
	const QJsonArray rows =
	        root.value(QStringLiteral("observations")).toArray();
	if (rows.isEmpty()) {
		return series;
	}

	std::vector<bbq_sample> samples;
	samples.reserve(rows.size());

	for (const QJsonValue &row_value : rows) {
		const QJsonObject row = row_value.toObject();

		const QJsonValue epoch = row.value(QStringLiteral("epoch"));
		if (!epoch.isDouble()) {
			return bbq_series(bbq_band::observed,
			                  QString::fromLatin1(provider_name));
		}

		bbq_sample sample;
		sample.start_utc = static_cast<qint64>(epoch.toDouble());

		/*
		 * The values live under a unit-system key rather than beside
		 * the timestamp -- the row-oriented shape from sec 2.6.3. The
		 * metric block is requested via units=m, and asking for the
		 * block by name means a response that ignored the request
		 * yields no samples instead of numbers in the wrong unit.
		 */
		const QJsonObject metric =
		        row.value(QStringLiteral("metric")).toObject();

		const QJsonValue temperature = metric.value(QStringLiteral("tempAvg"));
		if (temperature.isDouble()) {
			sample.temperature = temperature.toDouble();
		}

		/* Already mm/h, which is what the model stores. */
		const QJsonValue rain = metric.value(QStringLiteral("precipRate"));
		if (rain.isDouble()) {
			sample.precip_rate = rain.toDouble();
		}

		samples.push_back(sample);
	}

	/*
	 * Sorted before durations are measured, because a duration taken
	 * from the distance to the next row is meaningless if the rows are
	 * not in order -- and sec 2.6.2 records one WU endpoint that
	 * answers newest-first. bbq_series sorts too, but by then the
	 * durations would already be wrong.
	 */
	const auto by_start = [](const bbq_sample &left, const bbq_sample &right) {
		return left.start_utc < right.start_utc;
	};
	std::sort(samples.begin(), samples.end(), by_start);

	fill_durations(samples, 300);

	series.set_samples(std::move(samples));
	return series;
}

bbq_series bbq_wu_read_current_station(const QJsonDocument &response) {
	bbq_series series(bbq_band::current, QString::fromLatin1(provider_name));

	if (!response.isObject()) {
		return series;
	}

	const QJsonObject root = response.object();
	const QJsonArray rows = root.value(QStringLiteral("observations")).toArray();
	if (rows.isEmpty()) {
		return series;
	}

	const QJsonObject row = rows.first().toObject();
	const QJsonValue epoch = row.value(QStringLiteral("epoch"));
	if (!epoch.isDouble()) {
		return series;
	}

	bbq_sample sample;
	sample.start_utc = static_cast<qint64>(epoch.toDouble());
	sample.duration_s = bbq_current_validity_s;

	const QJsonObject metric = row.value(QStringLiteral("metric")).toObject();

	/*
	 * `temp` here, `tempAvg` in the history endpoint. Same API, same
	 * quantity, two spellings -- and reusing the history reader's name
	 * would have yielded a band with no temperature and nothing to say
	 * why.
	 */
	const QJsonValue temperature = metric.value(QStringLiteral("temp"));
	if (temperature.isDouble()) {
		sample.temperature = temperature.toDouble();
	}

	/* Already mm/h. This is why the station path beats the geocode one. */
	const QJsonValue rain = metric.value(QStringLiteral("precipRate"));
	if (rain.isDouble()) {
		sample.precip_rate = rain.toDouble();
	}

	std::vector<bbq_sample> samples;
	samples.push_back(sample);
	series.set_samples(std::move(samples));
	return series;
}

bbq_series bbq_wu_read_current_point(const QJsonDocument &response) {
	bbq_series series(bbq_band::current, QString::fromLatin1(provider_name));

	if (!response.isObject()) {
		return series;
	}

	const QJsonObject root = response.object();
	const QJsonValue when = root.value(QStringLiteral("validTimeUtc"));
	if (!when.isDouble()) {
		return series;
	}

	bbq_sample sample;
	sample.start_utc = static_cast<qint64>(when.toDouble());
	sample.duration_s = bbq_current_validity_s;

	const QJsonValue temperature = root.value(QStringLiteral("temperature"));
	if (temperature.isDouble()) {
		sample.temperature = temperature.toDouble();
	}

	/*
	 * Deliberately no rain. This endpoint offers precip1Hour and its
	 * longer siblings, which are trailing accumulations -- dividing one
	 * by its window would give the mean rate over the hour just gone
	 * and label it as now, which is a plausible wrong number rather
	 * than a missing one. Absent is the honest value.
	 */

	std::vector<bbq_sample> samples;
	samples.push_back(sample);
	series.set_samples(std::move(samples));
	return series;
}

bbq_series bbq_wu_read_nowcast(const QJsonDocument &response) {
	column_spec spec;
	spec.band = bbq_band::nowcast;
	spec.rain_field = QStringLiteral("precipRate");
	spec.rain_is_rate = true;
	spec.fallback_step_s = 900;
	return read_columns(response, spec);
}

bbq_series bbq_wu_read_hourly(const QJsonDocument &response) {
	column_spec spec;
	spec.band = bbq_band::hourly;
	spec.rain_field = QStringLiteral("qpf");
	spec.rain_is_rate = false;
	spec.fallback_step_s = 3600;
	return read_columns(response, spec);
}
