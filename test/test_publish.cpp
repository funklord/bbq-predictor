#include <QTest>

#include <cmath>
#include <cstring>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <sys/stat.h>

#include "model/composite.h"
#include "publish/encode.h"
#include "publish/publish.h"
#include "publish/signing_key.h"
#include "store/history.h"

#include "wire/vendor/bbq.hpp"

/*
 * The published format's encoder (project.md sec 15.12.3), read back
 * through the parser situ generates from src/wire/bbq.situ.
 *
 * That is the point of the arrangement: the encoder is my reading of the
 * schema and the generated parser is situ's, so a file both agree on is
 * evidence about the format rather than about one implementation. The
 * generated parser is safe here only because these are files the case
 * built itself (src/wire/vendor/README.md).
 */
class test_publish : public QObject {
	Q_OBJECT

private slots:
	void init();

	void a_month_reads_back_value_for_value();
	void a_head_carries_every_band_and_its_fetch_time();
	void an_index_and_a_manifest_read_back();
	void the_signature_covers_the_file_and_nothing_else();
	void the_same_input_signs_to_the_same_bytes();
	void what_the_format_cannot_carry_is_refused();
	void band_codes_are_a_contract();
	void month_numbers_and_names_agree();

	void publishing_twice_rewrites_nothing();
	void a_new_observation_rewrites_only_its_month();
	void a_round_with_no_bands_keeps_the_last_head();
	void a_station_that_cannot_be_a_path_is_refused();
	void a_key_others_can_read_is_refused();

private:
	bbq_signing_key m_key;
};

namespace {

using situ::rt::err;

/* A message over a copy, since the generated reader wants mutable bytes. */
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

QString text_of(const situ::rt::bytes &span) {
	return QString::fromLatin1(bytes_of(span));
}

/* The body of a signed file, after checking the container and the signature. */
bool open_file(const QByteArray &file, const QByteArray &public_key,
               situ::file_kind kind, QByteArray *body) {
	buffer in(file);
	situ::bbq_file parsed;
	if (situ::bbq_file::at(in.message, 0, std::uint32_t(file.size()), parsed) != err::ok ||
	    parsed.validate() != err::ok || parsed.kind() != kind) {
		return false;
	}

	std::uint32_t offset = 0;
	std::uint32_t length = 0;
	if (parsed.signature_covered(offset, length) != err::ok || offset != 0 ||
	    length + 64 != std::uint32_t(file.size())) {
		return false;
	}
	if (!bbq_signature_valid(bytes_of(parsed.signature()), public_key,
	                         file.left(int(length)))) {
		return false;
	}

	*body = bytes_of(parsed.body());
	return true;
}

/*
 * The inflated stream back into samples, through the generated reader:
 * inflate, then walk every column with its deltas and bitmaps.
 */
bool decode_columns(const situ::packed_columns &packed_columns,
                    std::vector<bbq_sample> *samples) {
	const QByteArray packed = bytes_of(packed_columns.packed());
	buffer qin(packed);
	situ::qcompressed q;
	if (situ::qcompressed::at(qin.message, 0, std::uint32_t(packed.size()), q) != err::ok ||
	    q.validate() != err::ok) {
		return false;
	}

	const QByteArray raw = qUncompress(packed);
	if (raw.size() != int(q.raw_len())) {
		return false;
	}

	buffer cin(raw);
	situ::columns c;
	std::uint32_t which = 0;
	if (situ::columns::at(cin.message, 0, std::uint32_t(raw.size()), c) != err::ok ||
	    c.check(&which) != err::ok) {
		return false;
	}

	const std::uint32_t rows = std::uint32_t(c.rows_value());
	samples->assign(rows, bbq_sample());

	std::int64_t start = 0;
	for (std::uint32_t i = 0; i < rows; ++i) {
		situ::zv1 delta;
		situ::v1 duration;
		if (c.start_utc_at(i, delta) != err::ok || c.duration_s_at(i, duration) != err::ok) {
			return false;
		}
		start += delta.v_value();
		(*samples)[i].start_utc = start;
		(*samples)[i].duration_s = int(duration.v_value());
	}

	/* One quantity: its bitmap, then one delta per set bit. */
	auto quantity = [&](bool has, auto bits_of, auto values_of, double scale,
	                    std::optional<double> bbq_sample::*member) {
		if (!has) {
			return true;
		}
		situ::rt::bytes bits;
		situ::optional_column values;
		if (bits_of(bits) != err::ok || values_of(values) != err::ok ||
		    bits.size() != (rows + 7) / 8) {
			return false;
		}

		std::uint32_t next = 0;
		std::int64_t value = 0;
		for (std::uint32_t i = 0; i < rows; ++i) {
			if (!(bits.data()[i / 8] & (1u << (i % 8)))) {
				continue;
			}
			situ::zv1 delta;
			if (values.delta_at(next++, delta) != err::ok) {
				return false;
			}
			value += delta.v_value();
			(*samples)[i].*member = double(value) / scale;
		}

		/* The count must equal the bitmap's population: the caller's check. */
		return next == values.count_value() && next == values.delta_count();
	};

	return quantity(c.has_temperature(),
	                [&](situ::rt::bytes &b) { return c.temperature_map_temperature_map_bits(b); },
	                [&](situ::optional_column &o) { return c.temperature_temperature_values(o); },
	                10.0, &bbq_sample::temperature) &&
	       quantity(c.has_precip_rate(),
	                [&](situ::rt::bytes &b) { return c.precip_rate_map_precip_rate_map_bits(b); },
	                [&](situ::optional_column &o) { return c.precip_rate_precip_rate_values(o); },
	                100.0, &bbq_sample::precip_rate) &&
	       quantity(c.has_precip_chance(),
	                [&](situ::rt::bytes &b) { return c.precip_chance_map_precip_chance_map_bits(b); },
	                [&](situ::optional_column &o) { return c.precip_chance_precip_chance_values(o); },
	                1.0, &bbq_sample::precip_chance) &&
	       quantity(c.has_wind_kph(),
	                [&](situ::rt::bytes &b) { return c.wind_kph_map_wind_kph_map_bits(b); },
	                [&](situ::optional_column &o) { return c.wind_kph_wind_kph_values(o); },
	                10.0, &bbq_sample::wind_kph);
}

/* A month of five-minute observations with every awkward case in it. */
std::vector<bbq_sample> observations(uint16_t month, int count) {
	std::vector<bbq_sample> samples;
	const qint64 begin = bbq_wire_month_begin(month);
	for (int i = 0; i < count; ++i) {
		bbq_sample sample;
		sample.start_utc = begin + i * 300 + (i % 7 == 3 ? 17 : 0);
		sample.duration_s = 300;
		sample.temperature = (i % 11 == 5) ? std::nullopt
		                                    : std::optional<double>(-12.3 + 0.1 * (i % 400));
		sample.precip_rate = (i % 13 == 0) ? std::optional<double>(0.25 * (i % 9)) : 0.0;
		sample.wind_kph = (i % 17 == 2) ? std::nullopt : std::optional<double>(3.4);
		samples.push_back(sample);
	}
	return samples;
}

bool same_value(const std::optional<double> &a, const std::optional<double> &b,
                double scale) {
	if (a.has_value() != b.has_value()) {
		return false;
	}
	return !a || std::llround(*a * scale) == std::llround(*b * scale);
}

} // namespace

void test_publish::init() {
	uint8_t seed[32];
	for (int i = 0; i < 32; ++i) {
		seed[i] = uint8_t(i * 7 + 1);
	}
	m_key.set_seed(seed);
}

void test_publish::a_month_reads_back_value_for_value() {
	const uint16_t month = uint16_t(2026 * 12 + 8);
	bbq_wire_month_rows rows;
	rows.samples = observations(month, 8000);
	rows.providers = {QStringLiteral("wunderground"), QStringLiteral("open-meteo")};
	for (size_t i = 0; i < rows.samples.size(); ++i) {
		rows.provider.push_back(i < 5000 ? 0 : 1);
	}

	QString error;
	const QByteArray body = bbq_wire_month_body(QStringLiteral("ISTOCK877"), month, rows, &error);
	QVERIFY2(!body.isEmpty(), qPrintable(error));
	const QByteArray file = bbq_wire_file(bbq_wire_kind::month, body, m_key);

	QByteArray read_body;
	QVERIFY(open_file(file, m_key.public_key(), situ::file_kind::month, &read_body));

	buffer in(read_body);
	situ::month_body parsed;
	QCOMPARE(situ::month_body::at(in.message, 0, std::uint32_t(read_body.size()), parsed), err::ok);
	QCOMPARE(parsed.validate(), err::ok);
	QCOMPARE(parsed.month(), month);

	situ::station_id station;
	QCOMPARE(parsed.station(station), err::ok);
	QCOMPARE(text_of(station.id()), QStringLiteral("ISTOCK877"));

	QCOMPARE(int(parsed.provider_count()), 2);
	situ::provider_name second;
	QCOMPARE(parsed.providers_at(1, second), err::ok);
	QCOMPARE(text_of(second.name()), QStringLiteral("open-meteo"));

	/* Two runs: 5000 of the first provider, 3000 of the second. */
	QCOMPARE(parsed.run_count_value(), uint64_t(2));
	situ::provider_run run;
	QCOMPARE(parsed.runs_at(1, run), err::ok);
	QCOMPARE(run.provider(), uint8_t(1));

	situ::packed_columns packed;
	QCOMPARE(parsed.columns(packed), err::ok);
	std::vector<bbq_sample> back;
	QVERIFY(decode_columns(packed, &back));

	QCOMPARE(back.size(), rows.samples.size());
	for (size_t i = 0; i < back.size(); ++i) {
		const bbq_sample &a = rows.samples[i];
		const bbq_sample &b = back[i];
		QCOMPARE(b.start_utc, a.start_utc);
		QCOMPARE(b.duration_s, a.duration_s);
		QVERIFY2(same_value(a.temperature, b.temperature, 10), qPrintable(QString::number(i)));
		QVERIFY2(same_value(a.precip_rate, b.precip_rate, 100), qPrintable(QString::number(i)));
		QVERIFY2(same_value(a.wind_kph, b.wind_kph, 10), qPrintable(QString::number(i)));
		QVERIFY(!b.precip_chance);
	}

	/* And it is small: the point of packing. */
	QVERIFY2(file.size() < 8000 * 4, qPrintable(QString::number(file.size())));
}

void test_publish::a_head_carries_every_band_and_its_fetch_time() {
	bbq_series hourly(bbq_band::hourly, QStringLiteral("wunderground"));
	bbq_series extended(bbq_band::extended, QStringLiteral("open-meteo"));
	bbq_series corrected(bbq_band::corrected, QStringLiteral("bbq-predictor"));

	std::vector<bbq_sample> hours;
	for (int i = 0; i < 360; ++i) {
		bbq_sample sample;
		sample.start_utc = 1790000000 + i * 3600;
		sample.duration_s = 3600;
		sample.temperature = 15.0 + (i % 24) * 0.5;
		sample.precip_chance = double(i % 101);
		hours.push_back(sample);
	}
	hourly.set_samples(hours);
	hourly.set_fetched_utc(1789990000);
	extended.set_samples(std::vector<bbq_sample>(hours.begin(), hours.begin() + 100));
	extended.set_fetched_utc(1789980000);
	corrected.set_samples(hours);

	QString error;
	const QByteArray body = bbq_wire_head_body(QStringLiteral("ISTOCK877"), 1790000500,
	                                           {&hourly, &corrected, &extended}, &error);
	QVERIFY2(!body.isEmpty(), qPrintable(error));
	const QByteArray file = bbq_wire_file(bbq_wire_kind::head, body, m_key);

	QByteArray read_body;
	QVERIFY(open_file(file, m_key.public_key(), situ::file_kind::head, &read_body));
	buffer in(read_body);
	situ::head_body head;
	QCOMPARE(situ::head_body::at(in.message, 0, std::uint32_t(read_body.size()), head), err::ok);
	QCOMPARE(head.validate(), err::ok);
	QCOMPARE(head.generated_utc(), qint64(1790000500));

	/* The corrected band is this program's opinion and is not published. */
	QCOMPARE(int(head.band_count()), 2);

	/*
	 * The SECOND band is the one the packed length exists for: without
	 * it, nothing after the first band's compressed columns could be
	 * found without inflating them.
	 */
	situ::band second;
	QCOMPARE(head.bands_at(1, second), err::ok);
	QCOMPARE(second.band(), bbq_wire_band_code(bbq_band::extended));
	QCOMPARE(second.fetched_utc(), qint64(1789980000));
	situ::provider_name provider;
	QCOMPARE(second.provider(provider), err::ok);
	QCOMPARE(text_of(provider.name()), QStringLiteral("open-meteo"));
	QCOMPARE(second.max_duration_s(), uint32_t(3600));

	situ::packed_columns packed;
	QCOMPARE(second.columns(packed), err::ok);
	std::vector<bbq_sample> back;
	QVERIFY(decode_columns(packed, &back));
	QCOMPARE(back.size(), size_t(100));
	QCOMPARE(back[57].precip_chance, std::optional<double>(57.0));
	QCOMPARE(back[57].temperature, std::optional<double>(15.0 + 9 * 0.5));
}

void test_publish::an_index_and_a_manifest_read_back() {
	std::vector<bbq_wire_index_entry> entries(2);
	entries[0].month = uint16_t(2026 * 12 + 7);
	entries[0].rows = 8000;
	entries[0].bytes = 20000;
	entries[0].digest = QByteArray(16, 'a');
	entries[0].first_utc = 1785000000;
	entries[0].last_utc = 1787000000;
	entries[1] = entries[0];
	entries[1].month = uint16_t(2026 * 12 + 8);
	entries[1].digest = QByteArray(16, 'b');

	QString error;
	QByteArray body = bbq_wire_index_body(QStringLiteral("ISTOCK877"), 1790000000, entries, &error);
	QVERIFY2(!body.isEmpty(), qPrintable(error));
	QByteArray read_body;
	QVERIFY(open_file(bbq_wire_file(bbq_wire_kind::index, body, m_key), m_key.public_key(),
	                  situ::file_kind::index, &read_body));
	{
		buffer in(read_body);
		situ::index_body index;
		QCOMPARE(situ::index_body::at(in.message, 0, std::uint32_t(read_body.size()), index), err::ok);
		QCOMPARE(index.validate(), err::ok);
		QCOMPARE(int(index.count()), 2);
		situ::index_entry entry;
		QCOMPARE(index.months_at(1, entry), err::ok);
		QCOMPARE(entry.month(), entries[1].month);
		QCOMPARE(entry.rows(), uint32_t(8000));
		QCOMPARE(bytes_of(entry.digest()), QByteArray(16, 'b'));
		QCOMPARE(entry.last_utc(), qint64(1787000000));
	}

	body = bbq_wire_stations_body(1790000000, {QStringLiteral("ISTOCK822"), QStringLiteral("ISTOCK877")},
	                              &error);
	QVERIFY2(!body.isEmpty(), qPrintable(error));
	QVERIFY(open_file(bbq_wire_file(bbq_wire_kind::stations, body, m_key), m_key.public_key(),
	                  situ::file_kind::stations, &read_body));
	buffer in(read_body);
	situ::stations_body stations;
	QCOMPARE(situ::stations_body::at(in.message, 0, std::uint32_t(read_body.size()), stations), err::ok);
	QCOMPARE(stations.validate(), err::ok);
	situ::station_id id;
	QCOMPARE(stations.ids_at(1, id), err::ok);
	QCOMPARE(text_of(id.id()), QStringLiteral("ISTOCK877"));
}

void test_publish::the_signature_covers_the_file_and_nothing_else() {
	QString error;
	const QByteArray body = bbq_wire_stations_body(1790000000, {QStringLiteral("A1")}, &error);
	const QByteArray file = bbq_wire_file(bbq_wire_kind::stations, body, m_key);
	QByteArray read_body;
	QVERIFY(open_file(file, m_key.public_key(), situ::file_kind::stations, &read_body));

	/* Any flipped bit anywhere, header and signature included, is refused. */
	for (int i = 0; i < file.size(); ++i) {
		QByteArray bad = file;
		bad[i] = char(bad[i] ^ 0x01);
		QVERIFY2(!open_file(bad, m_key.public_key(), situ::file_kind::stations, &read_body),
		         qPrintable(QStringLiteral("byte %1").arg(i)));
	}

	/* And another key's public half does not verify it. */
	bbq_signing_key other;
	uint8_t seed[32] = {9};
	other.set_seed(seed);
	QVERIFY(!open_file(file, other.public_key(), situ::file_kind::stations, &read_body));
}

void test_publish::the_same_input_signs_to_the_same_bytes() {
	/*
	 * The daemon tells a client "unchanged" by the first bytes of the
	 * signature, which only works because the encoding and Ed25519 are
	 * both deterministic. A file re-published with nothing new in it
	 * must be byte-identical, or every client re-downloads every month.
	 */
	const uint16_t month = uint16_t(2026 * 12 + 8);
	bbq_wire_month_rows rows;
	rows.samples = observations(month, 500);
	rows.providers = {QStringLiteral("wunderground")};
	rows.provider.assign(rows.samples.size(), 0);

	QString error;
	const QByteArray a = bbq_wire_file(
	        bbq_wire_kind::month,
	        bbq_wire_month_body(QStringLiteral("ISTOCK877"), month, rows, &error), m_key);
	const QByteArray b = bbq_wire_file(
	        bbq_wire_kind::month,
	        bbq_wire_month_body(QStringLiteral("ISTOCK877"), month, rows, &error), m_key);
	QCOMPARE(a, b);
	QCOMPARE(bbq_wire_digest(a), bbq_wire_digest(b));
	QCOMPARE(bbq_wire_digest(a).size(), 16);
}

void test_publish::what_the_format_cannot_carry_is_refused() {
	const uint16_t month = uint16_t(2026 * 12 + 8);
	QString error;

	bbq_wire_month_rows rows;
	rows.samples = observations(month, 10);
	rows.providers = {QStringLiteral("wunderground")};
	rows.provider.assign(rows.samples.size(), 0);

	/* A station id that could become a path. */
	for (const char *station : {"../ETC", "istock877", "", "A B",
	                            "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456"}) {
		QVERIFY2(bbq_wire_month_body(QString::fromLatin1(station), month, rows, &error).isEmpty(),
		         station);
	}

	/* A sample outside the month. */
	bbq_wire_month_rows outside = rows;
	outside.samples.back().start_utc = bbq_wire_month_end(month);
	QVERIFY(bbq_wire_month_body(QStringLiteral("A1"), month, outside, &error).isEmpty());

	/* Out of order, and repeated. */
	bbq_wire_month_rows unsorted = rows;
	std::swap(unsorted.samples[2], unsorted.samples[3]);
	QVERIFY(bbq_wire_month_body(QStringLiteral("A1"), month, unsorted, &error).isEmpty());
	bbq_wire_month_rows repeated = rows;
	repeated.samples[3].start_utc = repeated.samples[2].start_utc;
	QVERIFY(bbq_wire_month_body(QStringLiteral("A1"), month, repeated, &error).isEmpty());

	/* A row naming a provider that is not in the table. */
	bbq_wire_month_rows unnamed = rows;
	unnamed.provider[4] = 1;
	QVERIFY(bbq_wire_month_body(QStringLiteral("A1"), month, unnamed, &error).isEmpty());

	/* More rows than the format allows. */
	std::vector<bbq_sample> many(size_t(bbq_wire_head_rows_max) + 1);
	for (size_t i = 0; i < many.size(); ++i) {
		many[i].start_utc = qint64(i) * 60;
	}
	QVERIFY(bbq_wire_columns(many, bbq_wire_head_rows_max, &error).isEmpty());

	/* A value no instrument reports travels as absent, not as a number. */
	std::vector<bbq_sample> odd(1);
	odd[0].temperature = std::nan("");
	odd[0].wind_kph = 1e300;
	const QByteArray columns = bbq_wire_columns(odd, 10, &error);
	QVERIFY(!columns.isEmpty());
	QCOMPARE(uint8_t(columns[0]), uint8_t(0));
}

void test_publish::band_codes_are_a_contract() {
	/* Written out, because a client in the field already depends on them. */
	QCOMPARE(bbq_wire_band_code(bbq_band::observed), uint8_t(1));
	QCOMPARE(bbq_wire_band_code(bbq_band::current), uint8_t(2));
	QCOMPARE(bbq_wire_band_code(bbq_band::nowcast_fine), uint8_t(3));
	QCOMPARE(bbq_wire_band_code(bbq_band::nowcast), uint8_t(4));
	QCOMPARE(bbq_wire_band_code(bbq_band::extended), uint8_t(5));
	QCOMPARE(bbq_wire_band_code(bbq_band::hourly), uint8_t(6));
	QCOMPARE(bbq_wire_band_code(bbq_band::corrected), uint8_t(0));
}

void test_publish::month_numbers_and_names_agree() {
	const uint16_t month = bbq_wire_month_of(1790000000);  /* 2026-09-21 */
	QCOMPARE(bbq_wire_month_name(month), QStringLiteral("2026-09"));
	QCOMPARE(bbq_wire_month_begin(month), qint64(1788220800));
	QCOMPARE(bbq_wire_month_end(month), qint64(1790812800));

	/* December rolls into the next year. */
	const uint16_t december = uint16_t(2026 * 12 + 11);
	QCOMPARE(bbq_wire_month_name(december), QStringLiteral("2026-12"));
	QCOMPARE(bbq_wire_month_name(uint16_t(december + 1)), QStringLiteral("2027-01"));
	QCOMPARE(bbq_wire_month_of(bbq_wire_month_end(december) - 1), december);
}

namespace {

/* A store holding two months of one station's observations. */
bool seed_history(bbq_history &history, const QString &path, const QString &station) {
	if (!history.open(path)) {
		return false;
	}
	for (uint16_t month : {uint16_t(2026 * 12 + 7), uint16_t(2026 * 12 + 8)}) {
		bbq_series series(bbq_band::observed, QStringLiteral("wunderground"));
		series.set_samples(observations(month, 300));
		if (history.record_observations(station, series) <= 0) {
			return false;
		}
	}
	return true;
}

QByteArray contents(const QString &path) {
	QFile in(path);
	return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
}

qint64 inode(const QString &path) {
	struct stat info;
	return ::stat(QFile::encodeName(path).constData(), &info) == 0 ? qint64(info.st_ino) : -1;
}

bbq_composite one_band() {
	bbq_series hourly(bbq_band::hourly, QStringLiteral("wunderground"));
	std::vector<bbq_sample> hours(24);
	for (int i = 0; i < 24; ++i) {
		hours[size_t(i)].start_utc = 1790000000 + i * 3600;
		hours[size_t(i)].duration_s = 3600;
		hours[size_t(i)].temperature = 10.0 + i;
	}
	hourly.set_samples(hours);
	hourly.set_fetched_utc(1789999000);
	bbq_composite composite;
	composite.set_series(hourly);
	return composite;
}

} // namespace

void test_publish::publishing_twice_rewrites_nothing() {
	QTemporaryDir root;
	bbq_history history;
	QVERIFY(seed_history(history, root.filePath("history.sqlite"), "ISTOCK877"));
	const QString dir = root.filePath("pub");
	QString out_text, error_text;
	QTextStream out(&out_text), error(&error_text);

	const bbq_composite composite = one_band();
	QCOMPARE(bbq_publish(dir, "ISTOCK877", &composite, history, m_key, 1790000100, out, error), 0);

	const QStringList files = {"ISTOCK877/2026-08", "ISTOCK877/2026-09", "ISTOCK877/index",
	                           "ISTOCK877/head", "stations"};
	for (const QString &file : files) {
		QVERIFY2(QFile::exists(dir + "/" + file), qPrintable(file + " " + error_text));
	}

	/*
	 * Same rows, same bands: every station file keeps its inode, which is
	 * what an untouched file looks like -- rewriting even identical bytes
	 * would rename a new file into place. The manifest is the exception
	 * by design: it carries the publish time and is the daemon's signal.
	 */
	std::map<QString, qint64> before;
	for (const QString &file : files) {
		before[file] = inode(dir + "/" + file);
	}
	QCOMPARE(bbq_publish(dir, "ISTOCK877", &composite, history, m_key, 1790000200, out, error), 0);
	for (const QString &file : files) {
		if (file != "stations") {
			QVERIFY2(inode(dir + "/" + file) == before[file], qPrintable(file));
		}
	}
	QVERIFY(inode(dir + "/stations") != before["stations"]);
}

void test_publish::a_new_observation_rewrites_only_its_month() {
	QTemporaryDir root;
	bbq_history history;
	QVERIFY(seed_history(history, root.filePath("history.sqlite"), "ISTOCK877"));
	const QString dir = root.filePath("pub");
	QString text;
	QTextStream out(&text), error(&text);
	QCOMPARE(bbq_publish(dir, "ISTOCK877", nullptr, history, m_key, 1790000100, out, error), 0);

	const QByteArray august = contents(dir + "/ISTOCK877/2026-08");
	const QByteArray september = contents(dir + "/ISTOCK877/2026-09");
	const QByteArray index = contents(dir + "/ISTOCK877/index");

	/* One more reading, late in September. */
	bbq_series late(bbq_band::observed, QStringLiteral("wunderground"));
	bbq_sample sample;
	sample.start_utc = bbq_wire_month_end(uint16_t(2026 * 12 + 8)) - 600;
	sample.duration_s = 300;
	sample.temperature = 4.5;
	late.set_samples({sample});
	QCOMPARE(history.record_observations("ISTOCK877", late), 1);

	QCOMPARE(bbq_publish(dir, "ISTOCK877", nullptr, history, m_key, 1790000200, out, error), 0);
	QCOMPARE(contents(dir + "/ISTOCK877/2026-08"), august);
	QVERIFY(contents(dir + "/ISTOCK877/2026-09") != september);
	QVERIFY(contents(dir + "/ISTOCK877/index") != index);

	/* And no head was written, since no round was given. */
	QVERIFY(!QFile::exists(dir + "/ISTOCK877/head"));
}

void test_publish::a_round_with_no_bands_keeps_the_last_head() {
	QTemporaryDir root;
	bbq_history history;
	QVERIFY(seed_history(history, root.filePath("history.sqlite"), "ISTOCK877"));
	const QString dir = root.filePath("pub");
	QString text;
	QTextStream out(&text), error(&text);

	const bbq_composite good = one_band();
	QCOMPARE(bbq_publish(dir, "ISTOCK877", &good, history, m_key, 1790000100, out, error), 0);
	const QByteArray head = contents(dir + "/ISTOCK877/head");
	QVERIFY(!head.isEmpty());

	const bbq_composite nothing;
	QCOMPARE(bbq_publish(dir, "ISTOCK877", &nothing, history, m_key, 1790000200, out, error), 0);
	QCOMPARE(contents(dir + "/ISTOCK877/head"), head);
}

void test_publish::a_station_that_cannot_be_a_path_is_refused() {
	QTemporaryDir root;
	bbq_history history;
	QVERIFY(history.open(root.filePath("history.sqlite")));
	const QString dir = root.filePath("pub");
	QString text;
	QTextStream out(&text), error(&text);

	for (const char *station : {"../ETC", "istock877", "", "A/B"}) {
		QCOMPARE(bbq_publish(dir, QString::fromLatin1(station), nullptr, history, m_key,
		                     1790000100, out, error),
		         1);
	}
	/* Refused before anything was created. */
	QVERIFY(!QDir(dir).exists());
}

void test_publish::a_key_others_can_read_is_refused() {
	QTemporaryDir root;
	const QString path = root.filePath("signing.key");

	bbq_signing_key made;
	QString why;
	QVERIFY2(made.generate(path, &why), qPrintable(why));
	QCOMPARE(QFile::permissions(path) & (QFile::ReadGroup | QFile::ReadOther),
	         QFileDevice::Permissions());

	bbq_signing_key loaded;
	QVERIFY2(loaded.load(path, &why), qPrintable(why));
	QCOMPARE(loaded.public_key(), made.public_key());

	/* Never overwritten. */
	bbq_signing_key again;
	QVERIFY(!again.generate(path, &why));

	QVERIFY(::chmod(QFile::encodeName(path).constData(), 0644) == 0);
	bbq_signing_key exposed;
	QVERIFY(!exposed.load(path, &why));
	QVERIFY2(why.contains("readable by others"), qPrintable(why));
}

QTEST_GUILESS_MAIN(test_publish)
#include "test_publish.moc"
