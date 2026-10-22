#include "publish/encode.h"

#include "publish/signing_key.h"

#include <QDateTime>
#include <QTimeZone>

#include <cmath>

extern "C" {
#include "monocypher.h"
}

namespace {

void put_u8(QByteArray &out, uint8_t value) {
	out.append(char(value));
}

void put_u16(QByteArray &out, uint16_t value) {
	out.append(char(value & 0xff));
	out.append(char(value >> 8));
}

void put_u32(QByteArray &out, uint32_t value) {
	for (int shift = 0; shift < 32; shift += 8) {
		out.append(char((value >> shift) & 0xff));
	}
}

void put_i64(QByteArray &out, qint64 value) {
	const uint64_t bits = uint64_t(value);
	for (int shift = 0; shift < 64; shift += 8) {
		out.append(char((bits >> shift) & 0xff));
	}
}

void put_varint(QByteArray &out, uint64_t value) {
	do {
		uint8_t byte = value & 0x7f;
		value >>= 7;
		if (value != 0) {
			byte |= 0x80;
		}
		out.append(char(byte));
	} while (value != 0);
}

void put_zvarint(QByteArray &out, int64_t value) {
	/* Zigzag: 0, -1, 1, -2 ... become 0, 1, 2, 3 ... */
	put_varint(out, (uint64_t(value) << 1) ^ uint64_t(value >> 63));
}

/* len u8 then the bytes; the caller has checked it is 1..32 ASCII. */
void put_short_text(QByteArray &out, const QString &text) {
	const QByteArray bytes = text.toLatin1();
	put_u8(out, uint8_t(bytes.size()));
	out.append(bytes);
}

bool short_ascii(const QString &text) {
	if (text.isEmpty() || text.size() > 32) {
		return false;
	}
	for (QChar c : text) {
		if (c.unicode() < 0x20 || c.unicode() > 0x7e) {
			return false;
		}
	}
	return true;
}

/*
 * One optional quantity, scaled to the integer the wire carries. A value
 * that is not finite, or would not fit, travels as absent rather than as
 * a number nobody measured.
 */
struct quantity {
	std::optional<double> bbq_sample::*member;
	double scale;
};

const quantity quantities[] = {
	{&bbq_sample::temperature, 10.0},     /* 0.1 degC */
	{&bbq_sample::precip_rate, 100.0},    /* 0.01 mm/h */
	{&bbq_sample::precip_chance, 1.0},    /* 1 percent */
	{&bbq_sample::wind_kph, 10.0},        /* 0.1 km/h */
};

std::optional<int64_t> scaled(const std::optional<double> &value, double scale) {
	if (!value || !std::isfinite(*value)) {
		return std::nullopt;
	}

	const double x = std::round(*value * scale);
	if (std::fabs(x) > 1e15) {
		return std::nullopt;
	}
	return int64_t(x);
}

} // namespace

uint8_t bbq_wire_band_code(bbq_band band) {
	switch (band) {
	case bbq_band::observed:
		return 1;
	case bbq_band::current:
		return 2;
	case bbq_band::nowcast_fine:
		return 3;
	case bbq_band::nowcast:
		return 4;
	case bbq_band::extended:
		return 5;
	case bbq_band::hourly:
		return 6;
	case bbq_band::corrected:
		/* This program's opinion, not a provider's report (sec 12.5). */
		return 0;
	}
	return 0;
}

bool bbq_wire_station_valid(const QString &station) {
	if (station.isEmpty() || station.size() > 32) {
		return false;
	}
	for (QChar c : station) {
		const ushort u = c.unicode();
		if (!((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9'))) {
			return false;
		}
	}
	return true;
}

uint16_t bbq_wire_month_of(qint64 utc) {
	const QDate date = QDateTime::fromSecsSinceEpoch(utc, QTimeZone::UTC).date();
	return uint16_t(date.year() * 12 + date.month() - 1);
}

qint64 bbq_wire_month_begin(uint16_t month) {
	const QDate first(month / 12, month % 12 + 1, 1);
	return QDateTime(first, QTime(0, 0), QTimeZone::UTC).toSecsSinceEpoch();
}

qint64 bbq_wire_month_end(uint16_t month) {
	return bbq_wire_month_begin(uint16_t(month + 1));
}

QString bbq_wire_month_name(uint16_t month) {
	return QStringLiteral("%1-%2")
	        .arg(month / 12, 4, 10, QLatin1Char('0'))
	        .arg(month % 12 + 1, 2, 10, QLatin1Char('0'));
}

QByteArray bbq_wire_columns(const std::vector<bbq_sample> &samples, int rows_max,
                            QString *error) {
	const int rows = int(samples.size());
	if (rows > rows_max) {
		*error = QStringLiteral("%1 rows, over the cap of %2").arg(rows).arg(rows_max);
		return {};
	}

	for (int i = 1; i < rows; ++i) {
		if (samples[i].start_utc <= samples[i - 1].start_utc) {
			*error = QStringLiteral("samples do not ascend at row %1").arg(i);
			return {};
		}
	}
	for (const bbq_sample &sample : samples) {
		if (sample.duration_s < 0) {
			*error = QStringLiteral("a negative duration");
			return {};
		}
	}

	/* Which quantities appear at all; an absent one costs nothing. */
	uint8_t present = 0;
	std::vector<std::optional<int64_t>> values[4];
	for (int q = 0; q < 4; ++q) {
		values[q].reserve(samples.size());
		for (const bbq_sample &sample : samples) {
			values[q].push_back(scaled(sample.*quantities[q].member, quantities[q].scale));
			if (values[q].back()) {
				present |= uint8_t(1u << q);
			}
		}
	}

	QByteArray out;
	put_u8(out, present);
	put_varint(out, uint64_t(rows));

	for (int q = 0; q < 4; ++q) {
		if (!(present & (1u << q))) {
			continue;
		}
		QByteArray bitmap((rows + 7) / 8, '\0');
		for (int i = 0; i < rows; ++i) {
			if (values[q][size_t(i)]) {
				bitmap[i / 8] = char(uint8_t(bitmap[i / 8]) | (1u << (i % 8)));
			}
		}
		out.append(bitmap);
	}

	int64_t previous = 0;
	for (const bbq_sample &sample : samples) {
		put_zvarint(out, sample.start_utc - previous);
		previous = sample.start_utc;
	}
	for (const bbq_sample &sample : samples) {
		put_varint(out, uint64_t(sample.duration_s));
	}

	for (int q = 0; q < 4; ++q) {
		if (!(present & (1u << q))) {
			continue;
		}
		int count = 0;
		for (const auto &value : values[q]) {
			count += value ? 1 : 0;
		}
		put_varint(out, uint64_t(count));

		int64_t last = 0;
		for (const auto &value : values[q]) {
			if (value) {
				put_zvarint(out, *value - last);
				last = *value;
			}
		}
	}

	if (out.size() > bbq_wire_inflated_max) {
		*error = QStringLiteral("the columns inflate past 8 MiB");
		return {};
	}
	return out;
}

QByteArray bbq_wire_packed(const QByteArray &inflated) {
	/* qCompress writes the inflated size big-endian, then a zlib stream. */
	const QByteArray compressed = qCompress(inflated, 9);
	QByteArray out;
	put_u32(out, uint32_t(compressed.size()));
	out.append(compressed);
	return out;
}

QByteArray bbq_wire_month_body(const QString &station, uint16_t month,
                               const bbq_wire_month_rows &rows, QString *error) {
	if (!bbq_wire_station_valid(station)) {
		*error = QStringLiteral("station id %1 cannot be published").arg(station);
		return {};
	}
	if (rows.samples.size() != rows.provider.size()) {
		*error = QStringLiteral("a provider for every row");
		return {};
	}
	if (rows.providers.isEmpty() || rows.providers.size() > bbq_wire_providers_max) {
		*error = QStringLiteral("%1 providers").arg(rows.providers.size());
		return {};
	}
	for (const QString &provider : rows.providers) {
		if (!short_ascii(provider)) {
			*error = QStringLiteral("provider name %1").arg(provider);
			return {};
		}
	}

	const qint64 begin = bbq_wire_month_begin(month);
	const qint64 end = bbq_wire_month_end(month);
	for (const bbq_sample &sample : rows.samples) {
		if (sample.start_utc < begin || sample.start_utc >= end) {
			*error = QStringLiteral("a sample outside %1").arg(bbq_wire_month_name(month));
			return {};
		}
	}
	for (uint8_t provider : rows.provider) {
		if (provider >= rows.providers.size()) {
			*error = QStringLiteral("a row names provider %1 of %2")
			                 .arg(provider)
			                 .arg(rows.providers.size());
			return {};
		}
	}

	const QByteArray columns =
	        bbq_wire_columns(rows.samples, bbq_wire_month_rows_max, error);
	if (columns.isEmpty()) {
		return {};
	}

	QByteArray out;
	put_short_text(out, station);
	put_u16(out, month);
	put_u8(out, uint8_t(rows.providers.size()));
	for (const QString &provider : rows.providers) {
		put_short_text(out, provider);
	}

	/* Runs of one provider, which is nearly always the whole month. */
	std::vector<std::pair<uint32_t, uint8_t>> runs;
	for (uint8_t provider : rows.provider) {
		if (!runs.empty() && runs.back().second == provider) {
			++runs.back().first;
		} else {
			runs.emplace_back(1, provider);
		}
	}
	put_varint(out, runs.size());
	for (const auto &run : runs) {
		put_varint(out, run.first);
		put_u8(out, run.second);
	}

	out.append(bbq_wire_packed(columns));
	return out;
}

QByteArray bbq_wire_head_body(const QString &station, qint64 generated_utc,
                              const std::vector<const bbq_series *> &bands,
                              QString *error) {
	if (!bbq_wire_station_valid(station)) {
		*error = QStringLiteral("station id %1 cannot be published").arg(station);
		return {};
	}

	QByteArray encoded;
	int count = 0;
	for (const bbq_series *series : bands) {
		const uint8_t code = series ? bbq_wire_band_code(series->band()) : 0;
		if (code == 0 || series->is_empty()) {
			continue;
		}
		if (count == bbq_wire_head_bands_max) {
			*error = QStringLiteral("more than %1 bands").arg(bbq_wire_head_bands_max);
			return {};
		}
		if (!short_ascii(series->provider())) {
			*error = QStringLiteral("provider name %1").arg(series->provider());
			return {};
		}

		const QByteArray columns =
		        bbq_wire_columns(series->samples(), bbq_wire_head_rows_max, error);
		if (columns.isEmpty()) {
			error->prepend(QStringLiteral("band %1: ")
			                       .arg(QString::fromLatin1(bbq_band_name(series->band()))));
			return {};
		}

		qint64 max_duration = 0;
		for (const bbq_sample &sample : series->samples()) {
			max_duration = std::max<qint64>(max_duration, sample.duration_s);
		}

		put_u8(encoded, code);
		put_short_text(encoded, series->provider());
		/* When it was FETCHED, never when it is published (sec 15.3). */
		put_i64(encoded, series->fetched_utc());
		put_u32(encoded, uint32_t(std::max(0, series->nominal_step_s())));
		put_u32(encoded, uint32_t(max_duration));
		encoded.append(bbq_wire_packed(columns));
		++count;
	}

	QByteArray out;
	put_short_text(out, station);
	put_i64(out, generated_utc);
	put_u8(out, uint8_t(count));
	out.append(encoded);
	return out;
}

QByteArray bbq_wire_index_body(const QString &station, qint64 generated_utc,
                               const std::vector<bbq_wire_index_entry> &entries,
                               QString *error) {
	if (!bbq_wire_station_valid(station)) {
		*error = QStringLiteral("station id %1 cannot be published").arg(station);
		return {};
	}
	if (entries.size() > 2400) {
		*error = QStringLiteral("%1 months").arg(entries.size());
		return {};
	}

	QByteArray out;
	put_short_text(out, station);
	put_i64(out, generated_utc);
	put_u16(out, uint16_t(entries.size()));

	for (size_t i = 0; i < entries.size(); ++i) {
		const bbq_wire_index_entry &entry = entries[i];
		if (i > 0 && entry.month <= entries[i - 1].month) {
			*error = QStringLiteral("index months do not ascend");
			return {};
		}
		if (entry.digest.size() != 16 || entry.first_utc > entry.last_utc) {
			*error = QStringLiteral("index entry for %1").arg(bbq_wire_month_name(entry.month));
			return {};
		}
		put_u16(out, entry.month);
		put_u32(out, entry.rows);
		put_u32(out, entry.bytes);
		out.append(entry.digest);
		put_i64(out, entry.first_utc);
		put_i64(out, entry.last_utc);
	}
	return out;
}

QByteArray bbq_wire_stations_body(qint64 generated_utc, const QStringList &stations,
                                  QString *error) {
	if (stations.size() > 4096) {
		*error = QStringLiteral("%1 stations").arg(stations.size());
		return {};
	}

	QByteArray out;
	put_i64(out, generated_utc);
	put_u16(out, uint16_t(stations.size()));
	for (const QString &station : stations) {
		if (!bbq_wire_station_valid(station)) {
			*error = QStringLiteral("station id %1 cannot be published").arg(station);
			return {};
		}
		put_short_text(out, station);
	}
	return out;
}

QByteArray bbq_wire_digest(const QByteArray &file) {
	QByteArray digest(16, '\0');
	crypto_blake2b(reinterpret_cast<uint8_t *>(digest.data()), 16,
	               reinterpret_cast<const uint8_t *>(file.constData()),
	               size_t(file.size()));
	return digest;
}

QByteArray bbq_wire_file(bbq_wire_kind kind, const QByteArray &body,
                         const bbq_signing_key &key) {
	QByteArray out("BBQ1");
	put_u8(out, uint8_t(kind));
	put_u8(out, 0);
	put_u32(out, uint32_t(body.size()));
	out.append(body);
	out.append(key.sign(out));
	return out;
}
