/*
 * libFuzzer target for the providers' response readers: Weather
 * Underground's, Open-Meteo's and MET's (project.md sec 2). They parse
 * what arrives from the internet, and the publisher runs them for
 * stations strangers named through the demand list (sec 15.14) -- in the
 * process that holds the signing key. Built and run by
 * `make fuzz-providers`, under ASan and UBSan.
 *
 * The first byte picks the reader; the rest is the response body, parsed
 * as JSON the way the client parses it. Beyond the sanitizers, two things
 * every reader's caller relies on are held: every number it produces is
 * finite, since one infinite temperature takes the graph's scale with it,
 * and no sample covers a negative span.
 */
#include <QByteArray>
#include <QJsonDocument>
#include <QtGlobal>

#include <cmath>
#include <optional>

#include "met/nowcast.h"
#include "openmeteo/forecast.h"
#include "wu/reader.h"

static void quiet(QtMsgType, const QMessageLogContext &, const QString &) {}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
	qInstallMessageHandler(quiet);
	return 0;
}

static void must_be_finite(double value) {
	if (!std::isfinite(value)) {
		__builtin_trap();
	}
}

static void must_be_finite(const std::optional<double> &value) {
	if (value.has_value()) {
		must_be_finite(*value);
	}
}

static void hold(const bbq_series &series) {
	for (const bbq_sample &sample : series.samples()) {
		if (sample.duration_s < 0) {
			__builtin_trap();
		}
		must_be_finite(sample.temperature);
		must_be_finite(sample.precip_rate);
		must_be_finite(sample.precip_chance);
		must_be_finite(sample.wind_kph);
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 1) {
		return 0;
	}
	const QJsonDocument response = QJsonDocument::fromJson(
	        QByteArray(reinterpret_cast<const char *>(data + 1), int(size - 1)));

	switch (data[0] % 9) {
	case 0: hold(bbq_wu_read_observed(response)); break;
	case 1: hold(bbq_wu_read_current_station(response)); break;
	case 2: hold(bbq_wu_read_current_point(response)); break;
	case 3: hold(bbq_wu_read_nowcast(response)); break;
	case 4: hold(bbq_wu_read_hourly(response)); break;
	case 5:
		for (const bbq_wu_nearby &one : bbq_wu_read_nearby(response)) {
			must_be_finite(one.latitude);
			must_be_finite(one.longitude);
			must_be_finite(one.distance_km);
		}
		break;
	case 6:
		for (const bbq_wu_place &one : bbq_wu_read_places(response)) {
			must_be_finite(one.latitude);
			must_be_finite(one.longitude);
		}
		break;
	case 7: hold(bbq_openmeteo_read(response)); break;
	case 8: hold(bbq_met_read_nowcast(response)); break;
	}
	return 0;
}
