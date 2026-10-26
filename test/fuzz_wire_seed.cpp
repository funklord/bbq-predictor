/*
 * Writes fuzz_wire.cpp's starting corpus: one real body of each kind,
 * from the encoder, each prefixed with the byte fuzz_wire reads its kind
 * from. A fuzzer starting from noise would spend its run being refused
 * by the first length; from these it starts inside every structure.
 */
#include <QFile>
#include <QString>

#include "publish/encode.h"

static void put(const QString &dir, const char *name, char kind, const QByteArray &body) {
	QFile out(dir + QLatin1Char('/') + QLatin1String(name));
	if (out.open(QIODevice::WriteOnly)) {
		out.write(QByteArray(1, kind) + body);
	}
}

int main(int argc, char **argv) {
	if (argc != 2) {
		return 2;
	}
	const QString dir = QString::fromLocal8Bit(argv[1]);
	const QString station = QStringLiteral("ISTOCK877");
	const uint16_t month = uint16_t(2026 * 12 + 8);
	QString why;

	bbq_wire_month_rows rows;
	rows.providers = {QStringLiteral("wunderground"), QStringLiteral("met.no")};
	for (int i = 0; i < 40; ++i) {
		bbq_sample sample;
		sample.start_utc = bbq_wire_month_begin(month) + i * 300;
		sample.duration_s = 300;
		if (i % 3) {
			sample.temperature = -3.0 + i * 0.1;
		}
		sample.precip_rate = (i % 5) * 0.1;
		sample.wind_kph = i * 0.5;
		rows.samples.push_back(sample);
		rows.provider.push_back(i < 30 ? 0 : 1);
	}
	const QByteArray month_body = bbq_wire_month_body(station, month, rows, &why);
	put(dir, "month", 2, month_body);
	put(dir, "framed-month", 4, QByteArray(1, 3) + month_body);

	bbq_series hourly(bbq_band::hourly, QStringLiteral("wunderground"));
	std::vector<bbq_sample> hours(24);
	for (int i = 0; i < 24; ++i) {
		hours[size_t(i)].start_utc = 1790000000 + i * 3600;
		hours[size_t(i)].duration_s = 3600;
		hours[size_t(i)].precip_chance = double(i);
		hours[size_t(i)].temperature = 10.0 + i;
	}
	hourly.set_samples(hours);
	bbq_series extended(bbq_band::extended, QStringLiteral("open-meteo"));
	extended.set_samples(std::vector<bbq_sample>(hours.begin(), hours.begin() + 6));
	const QByteArray head_body =
	        bbq_wire_head_body(station, 1790000000, {&hourly, &extended}, &why);
	put(dir, "head", 3, head_body);
	put(dir, "framed-head", 4, QByteArray(1, 1) + head_body);

	bbq_wire_index_entry entry;
	entry.month = month;
	entry.rows = 40;
	entry.bytes = 300;
	entry.digest = QByteArray(16, 'd');
	entry.first_utc = rows.samples.front().start_utc;
	entry.last_utc = rows.samples.back().start_utc;
	const QByteArray index_body = bbq_wire_index_body(station, 1790000000, {entry}, &why);
	put(dir, "index", 1, index_body);
	put(dir, "framed-index", 4, QByteArray(1, 2) + index_body);

	const QByteArray stations_body =
	        bbq_wire_stations_body(1790000000, {station, QStringLiteral("A1")}, &why);
	put(dir, "stations", 0, stations_body);

	/*
	 * The framing case: kind 4, then the byte that picks the generated
	 * struct (fuzz_wire.cpp), then a real body of that struct.
	 */
	put(dir, "framed-stations", 4, QByteArray(1, 4) + stations_body);
	return 0;
}
