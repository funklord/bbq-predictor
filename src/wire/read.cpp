#include "wire/read.h"

#include <cmath>

#include "wire/vendor/bbq.hpp"

/*
 * Only at(), validate(), check() and the field accessors of the generated
 * code are used here. framed() and required() still read out of bounds at
 * situ 56dcad1 (src/wire/vendor/README.md); a call to either in this file
 * is a defect, whatever it is for.
 */

namespace {

using situ::rt::err;

/* A message over a private copy: the generated reader wants mutable bytes. */
struct buffer {
	explicit buffer(const QByteArray &bytes)
	        : data(bytes), message(reinterpret_cast<std::uint8_t *>(data.data()),
	                               std::uint32_t(data.size())) {}
	QByteArray data;
	situ::rt::message message;
};

QByteArray bytes_of(const situ::rt::bytes &span) {
	return QByteArray(reinterpret_cast<const char *>(span.data()), int(span.size()));
}

bool fail(QString *why, const QString &reason) {
	*why = reason;
	return false;
}

/* [A-Z0-9]{1,32}, and the station asked for: a file about another is refused. */
bool station_is(const situ::station_id &id, const QString &expected, QString *why) {
	const QString got = QString::fromLatin1(bytes_of(id.id()));
	if (!bbq_wire_station_valid(got)) {
		return fail(why, QStringLiteral("a station id that cannot be one"));
	}
	if (!expected.isEmpty() && got != expected) {
		return fail(why, QStringLiteral("a file about %1, asked for %2").arg(got, expected));
	}
	return true;
}

uint32_t station_size(const situ::station_id &id) {
	return uint32_t(id.id().size());
}

bool printable(const QByteArray &text) {
	if (text.isEmpty() || text.size() > 32) {
		return false;
	}
	for (char c : text) {
		if (uint8_t(c) < 0x20 || uint8_t(c) > 0x7e) {
			return false;
		}
	}
	return true;
}

/* Bytes a minimal LEB128 varint of `value` takes. */
uint32_t varint_size(uint64_t value) {
	uint32_t size = 1;
	while (value >= 0x80) {
		value >>= 7;
		++size;
	}
	return size;
}

uint32_t zvarint_size(int64_t value) {
	return varint_size((uint64_t(value) << 1) ^ uint64_t(value >> 63));
}

/* Overflow is undefined for signed integers, so every running sum is checked. */
bool add(int64_t a, int64_t b, int64_t *out) {
	return !__builtin_add_overflow(a, b, out);
}

/*
 * One quantity: a bitmap of which rows have it, then one delta per set
 * bit. The count must equal the bitmap's population, padding bits past
 * the last row must be zero, and the deltas must not overflow.
 */
template <typename bits_of, typename values_of>
bool quantity(bool present, uint32_t rows, bits_of bits_fn, values_of values_fn,
              double scale, std::optional<double> bbq_sample::*member,
              std::vector<bbq_sample> *samples, uint64_t *size, QString *why) {
	if (!present) {
		return true;
	}

	situ::rt::bytes bits;
	situ::optional_column values;
	if (bits_fn(bits) != err::ok || values_fn(values) != err::ok) {
		return fail(why, QStringLiteral("an optional column did not read"));
	}
	if (bits.size() != (rows + 7) / 8 || values.validate() != err::ok) {
		return fail(why, QStringLiteral("an optional column has the wrong shape"));
	}

	uint32_t set = 0;
	for (uint32_t i = 0; i < bits.size() * 8; ++i) {
		const bool on = bits.data()[i / 8] & (1u << (i % 8));
		if (on && i >= rows) {
			return fail(why, QStringLiteral("a padding bit is set"));
		}
		set += on ? 1 : 0;
	}
	if (values.count_value() != set || values.delta_count() != set) {
		return fail(why, QStringLiteral("a column's count is not its bitmap's"));
	}
	*size += bits.size() + varint_size(set);

	uint32_t next = 0;
	int64_t value = 0;
	for (uint32_t i = 0; i < rows; ++i) {
		if (!(bits.data()[i / 8] & (1u << (i % 8)))) {
			continue;
		}
		situ::zv1 delta;
		if (values.delta_at(next++, delta) != err::ok || delta.validate() != err::ok ||
		    !add(value, delta.v_value(), &value)) {
			return fail(why, QStringLiteral("a value did not read"));
		}
		*size += zvarint_size(delta.v_value());
		(*samples)[i].*member = double(value) / scale;
	}
	return true;
}

/*
 * A band's or a month's columns: the packed length, qCompress's own
 * raw length checked BEFORE qUncompress allocates it, the inflated
 * stream validated, then every column walked.
 */
bool columns(const situ::packed_columns &packed_columns, uint32_t rows_max,
             std::vector<bbq_sample> *samples, QString *why) {
	if (packed_columns.validate() != err::ok) {
		return fail(why, QStringLiteral("the packed columns do not frame"));
	}
	const QByteArray packed = bytes_of(packed_columns.packed());

	buffer qin(packed);
	situ::qcompressed q;
	if (situ::qcompressed::at(qin.message, 0, std::uint32_t(packed.size()), q) != err::ok ||
	    q.validate() != err::ok || q.raw_len() > uint32_t(bbq_wire_inflated_max)) {
		return fail(why, QStringLiteral("the columns claim too much"));
	}

	const QByteArray raw = qUncompress(packed);
	if (raw.size() != int(q.raw_len())) {
		return fail(why, QStringLiteral("the columns do not inflate to their size"));
	}

	buffer cin(raw);
	situ::columns c;
	std::uint32_t which = 0;
	if (situ::columns::at(cin.message, 0, std::uint32_t(raw.size()), c) != err::ok ||
	    c.validate() != err::ok || c.check(&which) != err::ok) {
		return fail(why, QStringLiteral("the columns do not parse (check %1)").arg(which));
	}

	/*
	 * NOT c.trailing(): situ does not enforce `trailing[remaining] max 0`
	 * after a dynamic member at 56dcad1, and its accessor reported no
	 * trailing bytes with one there -- test_wire's case found it. So the
	 * stream's exact length is computed below from what was decoded, and
	 * must equal what was inflated: that refuses trailing bytes and
	 * padded varints alike, so every file has one encoding.
	 */
	uint64_t size = 1;

	const uint64_t rows = c.rows_value();
	if (rows > rows_max || c.start_utc_count() != rows || c.duration_s_count() != rows) {
		return fail(why, QStringLiteral("%1 rows, or columns of the wrong length").arg(rows));
	}
	samples->assign(size_t(rows), bbq_sample());
	size += varint_size(rows);

	int64_t start = 0;
	for (uint32_t i = 0; i < rows; ++i) {
		situ::zv1 delta;
		situ::v1 duration;
		if (c.start_utc_at(i, delta) != err::ok || delta.validate() != err::ok ||
		    c.duration_s_at(i, duration) != err::ok || duration.validate() != err::ok ||
		    !add(start, delta.v_value(), &start)) {
			return fail(why, QStringLiteral("row %1 did not read").arg(i));
		}
		if (i > 0 && start <= (*samples)[i - 1].start_utc) {
			return fail(why, QStringLiteral("samples do not ascend at row %1").arg(i));
		}
		if (duration.v_value() > uint64_t(366) * 86400) {
			return fail(why, QStringLiteral("a duration of %1 s").arg(duration.v_value()));
		}
		(*samples)[i].start_utc = start;
		(*samples)[i].duration_s = int(duration.v_value());
		size += zvarint_size(delta.v_value()) + varint_size(duration.v_value());
	}

	const uint32_t n = uint32_t(rows);
	const bool read = quantity(c.has_temperature(), n,
	                [&](situ::rt::bytes &b) { return c.temperature_map_temperature_map_bits(b); },
	                [&](situ::optional_column &o) { return c.temperature_temperature_values(o); },
	                10.0, &bbq_sample::temperature, samples, &size, why) &&
	       quantity(c.has_precip_rate(), n,
	                [&](situ::rt::bytes &b) { return c.precip_rate_map_precip_rate_map_bits(b); },
	                [&](situ::optional_column &o) { return c.precip_rate_precip_rate_values(o); },
	                100.0, &bbq_sample::precip_rate, samples, &size, why) &&
	       quantity(c.has_precip_chance(), n,
	                [&](situ::rt::bytes &b) { return c.precip_chance_map_precip_chance_map_bits(b); },
	                [&](situ::optional_column &o) { return c.precip_chance_precip_chance_values(o); },
	                1.0, &bbq_sample::precip_chance, samples, &size, why) &&
	       quantity(c.has_wind_kph(), n,
	                [&](situ::rt::bytes &b) { return c.wind_kph_map_wind_kph_map_bits(b); },
	                [&](situ::optional_column &o) { return c.wind_kph_wind_kph_values(o); },
	                10.0, &bbq_sample::wind_kph, samples, &size, why);
	if (!read) {
		return false;
	}

	if (size != uint64_t(raw.size())) {
		return fail(why, QStringLiteral("the columns are %1 bytes where they encode %2")
		                         .arg(raw.size())
		                         .arg(size));
	}
	return true;
}

uint32_t file_cap(bbq_wire_kind kind) {
	return kind == bbq_wire_kind::month ? 4u * 1024 * 1024 : 1024u * 1024;
}

} // namespace

bool bbq_wire_open(const QByteArray &file, bbq_wire_kind kind,
                   const QByteArray &public_key, QByteArray *body, QString *why) {
	if (uint32_t(file.size()) > file_cap(kind) || file.size() < 10 + 64) {
		return fail(why, QStringLiteral("a file of %1 bytes").arg(file.size()));
	}

	/* The signature FIRST: nothing below runs on bytes the publisher did not sign. */
	if (!bbq_signature_valid(file.right(64), public_key, file.left(file.size() - 64))) {
		return fail(why, QStringLiteral("the signature does not verify"));
	}

	buffer in(file);
	situ::bbq_file parsed;
	std::uint32_t which = 0;
	if (situ::bbq_file::at(in.message, 0, std::uint32_t(file.size()), parsed) != err::ok ||
	    parsed.validate() != err::ok || parsed.check(&which) != err::ok) {
		return fail(why, QStringLiteral("the container does not parse (check %1)").arg(which));
	}
	if (uint8_t(parsed.kind()) != uint8_t(kind)) {
		return fail(why, QStringLiteral("a file of kind %1, asked for %2")
		                         .arg(int(parsed.kind()))
		                         .arg(int(kind)));
	}

	*body = bytes_of(parsed.body());
	return true;
}

bool bbq_wire_parse_stations(const QByteArray &body, QStringList *stations, QString *why) {
	buffer in(body);
	situ::stations_body parsed;
	std::uint32_t which = 0;
	if (situ::stations_body::at(in.message, 0, std::uint32_t(body.size()), parsed) != err::ok ||
	    parsed.validate() != err::ok || parsed.check(&which) != err::ok ||
	    parsed.trailing().size() != 0 || parsed.ids_count() != parsed.count()) {
		return fail(why, QStringLiteral("the manifest does not parse (check %1)").arg(which));
	}

	stations->clear();
	for (std::uint32_t i = 0; i < parsed.count(); ++i) {
		situ::station_id id;
		if (parsed.ids_at(i, id) != err::ok || !station_is(id, QString(), why)) {
			return fail(why, QStringLiteral("manifest entry %1: %2").arg(i).arg(*why));
		}
		stations->append(QString::fromLatin1(bytes_of(id.id())));
	}

	/* The exact length, since situ's trailing guard does not hold (see columns()). */
	uint64_t size = 8 + 2;
	for (const QString &id : *stations) {
		size += 1 + uint64_t(id.size());
	}
	if (size != uint64_t(body.size())) {
		return fail(why, QStringLiteral("bytes after the manifest"));
	}
	return true;
}

bool bbq_wire_parse_index(const QByteArray &body, const QString &station,
                          std::vector<bbq_wire_index_entry> *entries, QString *why) {
	buffer in(body);
	situ::index_body parsed;
	std::uint32_t which = 0;
	if (situ::index_body::at(in.message, 0, std::uint32_t(body.size()), parsed) != err::ok ||
	    parsed.validate() != err::ok || parsed.check(&which) != err::ok ||
	    parsed.trailing().size() != 0 || parsed.months_count() != parsed.count()) {
		return fail(why, QStringLiteral("the index does not parse (check %1)").arg(which));
	}

	situ::station_id id;
	if (parsed.station(id) != err::ok || !station_is(id, station, why)) {
		return false;
	}

	entries->clear();
	for (std::uint32_t i = 0; i < parsed.count(); ++i) {
		situ::index_entry entry;
		if (parsed.months_at(i, entry) != err::ok || entry.validate() != err::ok) {
			return fail(why, QStringLiteral("index entry %1 did not read").arg(i));
		}
		bbq_wire_index_entry out;
		out.month = entry.month();
		out.rows = entry.rows();
		out.bytes = entry.bytes();
		out.digest = bytes_of(entry.digest());
		out.first_utc = entry.first_utc();
		out.last_utc = entry.last_utc();

		/* Sorted without repeats: a relation situ cannot state. */
		if (!entries->empty() && out.month <= entries->back().month) {
			return fail(why, QStringLiteral("index months do not ascend"));
		}
		if (out.month / 12 < 1970 || out.month / 12 > 2199 || out.digest.size() != 16 ||
		    out.first_utc > out.last_utc || out.first_utc < bbq_wire_month_begin(out.month) ||
		    out.last_utc >= bbq_wire_month_end(out.month)) {
			return fail(why, QStringLiteral("index entry %1 is not a month").arg(i));
		}
		entries->push_back(out);
	}

	if (uint64_t(body.size()) != 1 + uint64_t(station_size(id)) + 8 + 2 + 42 * entries->size()) {
		return fail(why, QStringLiteral("bytes after the index"));
	}
	return true;
}

bool bbq_wire_parse_month(const QByteArray &body, const QString &station, uint16_t month,
                          bbq_wire_month_rows *rows, QString *why) {
	buffer in(body);
	situ::month_body parsed;
	std::uint32_t which = 0;
	if (situ::month_body::at(in.message, 0, std::uint32_t(body.size()), parsed) != err::ok ||
	    parsed.validate() != err::ok || parsed.check(&which) != err::ok ||
	    parsed.trailing().size() != 0) {
		return fail(why, QStringLiteral("the month does not parse (check %1)").arg(which));
	}

	situ::station_id id;
	if (parsed.station(id) != err::ok || !station_is(id, station, why)) {
		return false;
	}
	if (parsed.month() != month) {
		return fail(why, QStringLiteral("a file for another month"));
	}

	/* Counted as it is read; compared with the body at the end. */
	uint64_t size = 1 + uint64_t(station_size(id)) + 2 + 1;

	rows->providers.clear();
	if (parsed.providers_count() != parsed.provider_count()) {
		return fail(why, QStringLiteral("the provider table does not frame"));
	}
	for (std::uint32_t i = 0; i < parsed.provider_count(); ++i) {
		situ::provider_name name;
		if (parsed.providers_at(i, name) != err::ok || !printable(bytes_of(name.name()))) {
			return fail(why, QStringLiteral("provider %1 is not a name").arg(i));
		}
		rows->providers.append(QString::fromLatin1(bytes_of(name.name())));
		size += 1 + uint64_t(rows->providers.back().size());
	}

	situ::packed_columns packed;
	if (parsed.columns(packed) != err::ok ||
	    !columns(packed, uint32_t(bbq_wire_month_rows_max), &rows->samples, why)) {
		return false;
	}
	size += 4 + packed.packed().size();

	/* Every sample inside its month. */
	const qint64 begin = bbq_wire_month_begin(month);
	const qint64 end = bbq_wire_month_end(month);
	for (const bbq_sample &sample : rows->samples) {
		if (sample.start_utc < begin || sample.start_utc >= end) {
			return fail(why, QStringLiteral("a sample outside the month"));
		}
	}

	/* The provider runs sum to exactly the rows, each naming a provider that exists. */
	rows->provider.clear();
	if (parsed.runs_count() != parsed.run_count_value()) {
		return fail(why, QStringLiteral("the provider runs do not frame"));
	}
	size += varint_size(parsed.run_count_value());
	for (std::uint32_t i = 0; i < parsed.runs_count(); ++i) {
		situ::provider_run run;
		if (parsed.runs_at(i, run) != err::ok || run.validate() != err::ok ||
		    run.provider() >= rows->providers.size() ||
		    run.run_len_value() > rows->samples.size() - rows->provider.size()) {
			return fail(why, QStringLiteral("provider run %1 does not fit").arg(i));
		}
		rows->provider.insert(rows->provider.end(), size_t(run.run_len_value()),
		                      run.provider());
		size += varint_size(run.run_len_value()) + 1;
	}
	if (rows->provider.size() != rows->samples.size()) {
		return fail(why, QStringLiteral("the provider runs do not cover the rows"));
	}

	if (uint64_t(body.size()) != size) {
		return fail(why, QStringLiteral("bytes after the month"));
	}
	return true;
}

bool bbq_wire_parse_head(const QByteArray &body, const QString &station,
                         bbq_wire_head *head, QString *why) {
	buffer in(body);
	situ::head_body parsed;
	std::uint32_t which = 0;
	if (situ::head_body::at(in.message, 0, std::uint32_t(body.size()), parsed) != err::ok ||
	    parsed.validate() != err::ok || parsed.check(&which) != err::ok ||
	    parsed.trailing().size() != 0 || parsed.bands_count() != parsed.band_count() ||
	    parsed.band_count() > bbq_wire_head_bands_max) {
		return fail(why, QStringLiteral("the head does not parse (check %1)").arg(which));
	}

	situ::station_id id;
	if (parsed.station(id) != err::ok || !station_is(id, station, why)) {
		return false;
	}

	head->generated_utc = parsed.generated_utc();
	head->bands.clear();
	uint64_t size = 1 + uint64_t(station_size(id)) + 8 + 1;
	for (std::uint32_t i = 0; i < parsed.band_count(); ++i) {
		situ::band band;
		situ::provider_name provider;
		situ::packed_columns packed;
		if (parsed.bands_at(i, band) != err::ok || band.validate() != err::ok ||
		    band.provider(provider) != err::ok || !printable(bytes_of(provider.name())) ||
		    band.columns(packed) != err::ok) {
			return fail(why, QStringLiteral("band %1 did not read").arg(i));
		}
		size += 1 + 1 + uint64_t(provider.name().size()) + 8 + 4 + 4 + 4 +
		        packed.packed().size();

		/*
		 * A band this build does not know is skipped, not refused: a
		 * newer publisher may add one, and an older client should keep
		 * reading the bands it does know.
		 */
		bbq_band which_band;
		if (!bbq_wire_band_from_code(band.band(), &which_band)) {
			continue;
		}

		std::vector<bbq_sample> samples;
		if (!columns(packed, uint32_t(bbq_wire_head_rows_max), &samples, why)) {
			return fail(why, QStringLiteral("band %1: %2").arg(i).arg(*why));
		}

		bbq_series series(which_band, QString::fromLatin1(bytes_of(provider.name())));
		series.set_samples(std::move(samples));
		series.set_fetched_utc(band.fetched_utc());
		head->bands.push_back(std::move(series));
	}

	if (uint64_t(body.size()) != size) {
		return fail(why, QStringLiteral("bytes after the head"));
	}
	return true;
}

bool bbq_wire_read_stations(const QByteArray &file, const QByteArray &public_key,
                            QStringList *stations, QString *why) {
	QByteArray body;
	return bbq_wire_open(file, bbq_wire_kind::stations, public_key, &body, why) &&
	       bbq_wire_parse_stations(body, stations, why);
}

bool bbq_wire_read_index(const QByteArray &file, const QByteArray &public_key,
                         const QString &station,
                         std::vector<bbq_wire_index_entry> *entries, QString *why) {
	QByteArray body;
	return bbq_wire_open(file, bbq_wire_kind::index, public_key, &body, why) &&
	       bbq_wire_parse_index(body, station, entries, why);
}

bool bbq_wire_read_month(const QByteArray &file, const QByteArray &public_key,
                         const QString &station, uint16_t month,
                         bbq_wire_month_rows *rows, QString *why) {
	QByteArray body;
	return bbq_wire_open(file, bbq_wire_kind::month, public_key, &body, why) &&
	       bbq_wire_parse_month(body, station, month, rows, why);
}

bool bbq_wire_read_head(const QByteArray &file, const QByteArray &public_key,
                        const QString &station, bbq_wire_head *head, QString *why) {
	QByteArray body;
	return bbq_wire_open(file, bbq_wire_kind::head, public_key, &body, why) &&
	       bbq_wire_parse_head(body, station, head, why);
}
