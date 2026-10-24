#include <QTest>

#include <cstring>

#include "publish/encode.h"
#include "publish/signing_key.h"
#include "wire/read.h"

/*
 * The applet's reader (project.md sec 15.12). The round trips say it
 * reads what the publisher writes; the rest say what it refuses, one
 * case per rule situ cannot state, because those are exactly the rules
 * that are nobody's but this file's.
 *
 * The refusals go through the parse_* functions with hand-built bodies,
 * since through read_* a malformed body would be refused for its
 * signature first and the rule under test would never be reached.
 */
class test_wire : public QObject {
	Q_OBJECT

private slots:
	void init();

	void every_kind_round_trips();
	void a_head_band_this_build_does_not_know_is_skipped();
	void nothing_unsigned_is_parsed();
	void a_file_about_something_else_is_refused();
	void a_file_over_its_cap_is_refused_unread();
	void bytes_after_the_last_column_are_refused();
	void a_count_that_is_not_its_bitmap_is_refused();
	void a_set_padding_bit_is_refused();
	void samples_that_do_not_ascend_are_refused();
	void a_sample_outside_its_month_is_refused();
	void provider_runs_must_cover_the_rows_exactly();
	void a_raw_length_past_the_cap_is_refused_before_inflating();
	void index_months_must_ascend();

private:
	bbq_signing_key m_key;
};

namespace {

const QString station = QStringLiteral("ISTOCK877");
const uint16_t september = uint16_t(2026 * 12 + 8);

void put_varint(QByteArray &out, uint64_t value) {
	do {
		uint8_t byte = value & 0x7f;
		value >>= 7;
		out.append(char(value ? byte | 0x80 : byte));
	} while (value);
}

void put_zvarint(QByteArray &out, int64_t value) {
	put_varint(out, (uint64_t(value) << 1) ^ uint64_t(value >> 63));
}

/* An inflated column stream with only a temperature column, by hand. */
QByteArray columns_by_hand(const std::vector<int64_t> &starts, uint8_t bitmap,
                           int count, const std::vector<int64_t> &temperatures,
                           const QByteArray &tail = QByteArray()) {
	QByteArray out;
	out.append(char(0x01));                          /* temperature present */
	put_varint(out, starts.size());
	out.append(char(bitmap));
	int64_t previous = 0;
	for (int64_t start : starts) {
		put_zvarint(out, start - previous);
		previous = start;
	}
	for (size_t i = 0; i < starts.size(); ++i) {
		put_varint(out, 300);
	}
	put_varint(out, uint64_t(count));
	int64_t last = 0;
	for (int64_t t : temperatures) {
		put_zvarint(out, t - last);
		last = t;
	}
	return out + tail;
}

/* A month body around any column stream, with one provider run. */
QByteArray month_around(const QByteArray &inflated, uint64_t run_length,
                        uint8_t run_provider = 0) {
	QByteArray out;
	out.append(char(station.size()));
	out.append(station.toLatin1());
	out.append(char(september & 0xff));
	out.append(char(september >> 8));
	out.append(char(1));
	out.append(char(12));
	out.append("wunderground");
	put_varint(out, 1);
	put_varint(out, run_length);
	out.append(char(run_provider));
	out.append(bbq_wire_packed(inflated));
	return out;
}

qint64 in_september(int minute) {
	return bbq_wire_month_begin(september) + minute * 60;
}

} // namespace

void test_wire::init() {
	uint8_t seed[32];
	for (int i = 0; i < 32; ++i) {
		seed[i] = uint8_t(255 - i);
	}
	m_key.set_seed(seed);
}

void test_wire::every_kind_round_trips() {
	QString why;

	/* A month, with two providers and gaps in every quantity. */
	bbq_wire_month_rows rows;
	rows.providers = {QStringLiteral("wunderground"), QStringLiteral("met.no")};
	for (int i = 0; i < 2000; ++i) {
		bbq_sample sample;
		sample.start_utc = in_september(i * 5);
		sample.duration_s = 300;
		if (i % 9 != 4) {
			sample.temperature = -5.0 + (i % 300) * 0.1;
		}
		if (i % 4 == 0) {
			sample.precip_rate = (i % 13) * 0.05;
		}
		sample.wind_kph = (i % 11) * 1.5;
		rows.samples.push_back(sample);
		rows.provider.push_back(i < 1500 ? 0 : 1);
	}
	const QByteArray month = bbq_wire_file(
	        bbq_wire_kind::month, bbq_wire_month_body(station, september, rows, &why), m_key);

	bbq_wire_month_rows back;
	QVERIFY2(bbq_wire_read_month(month, m_key.public_key(), station, september, &back, &why),
	         qPrintable(why));
	QCOMPARE(back.providers, rows.providers);
	QCOMPARE(back.provider, rows.provider);
	QCOMPARE(back.samples.size(), rows.samples.size());
	for (size_t i = 0; i < back.samples.size(); ++i) {
		QCOMPARE(back.samples[i].start_utc, rows.samples[i].start_utc);
		QCOMPARE(back.samples[i].temperature.has_value(), rows.samples[i].temperature.has_value());
		if (rows.samples[i].temperature) {
			QCOMPARE(qRound(*back.samples[i].temperature * 10),
			         qRound(*rows.samples[i].temperature * 10));
		}
		QCOMPARE(back.samples[i].precip_rate.has_value(), rows.samples[i].precip_rate.has_value());
		QCOMPARE(back.samples[i].wind_kph, rows.samples[i].wind_kph);
	}

	/* The index, with a digest of that month. */
	bbq_wire_index_entry entry;
	entry.month = september;
	entry.rows = 2000;
	entry.bytes = uint32_t(month.size());
	entry.digest = bbq_wire_digest(month);
	entry.first_utc = rows.samples.front().start_utc;
	entry.last_utc = rows.samples.back().start_utc;
	const QByteArray index = bbq_wire_file(
	        bbq_wire_kind::index, bbq_wire_index_body(station, 1790000000, {entry}, &why), m_key);
	std::vector<bbq_wire_index_entry> entries;
	QVERIFY2(bbq_wire_read_index(index, m_key.public_key(), station, &entries, &why),
	         qPrintable(why));
	QCOMPARE(entries.size(), size_t(1));
	QCOMPARE(entries[0].digest, bbq_wire_digest(month));

	/* The manifest. */
	const QByteArray manifest = bbq_wire_file(
	        bbq_wire_kind::stations,
	        bbq_wire_stations_body(1790000000, {QStringLiteral("A1"), station}, &why), m_key);
	QStringList stations;
	QVERIFY2(bbq_wire_read_stations(manifest, m_key.public_key(), &stations, &why),
	         qPrintable(why));
	QCOMPARE(stations, QStringList({QStringLiteral("A1"), station}));

	/* The head, each band keeping its own fetch time. */
	bbq_series hourly(bbq_band::hourly, QStringLiteral("wunderground"));
	std::vector<bbq_sample> hours(48);
	for (int i = 0; i < 48; ++i) {
		hours[size_t(i)].start_utc = 1790000000 + i * 3600;
		hours[size_t(i)].duration_s = 3600;
		hours[size_t(i)].precip_chance = double(i);
	}
	hourly.set_samples(hours);
	hourly.set_fetched_utc(1789999000);
	const QByteArray head_file = bbq_wire_file(
	        bbq_wire_kind::head, bbq_wire_head_body(station, 1789999000, {&hourly}, &why), m_key);
	bbq_wire_head head;
	QVERIFY2(bbq_wire_read_head(head_file, m_key.public_key(), station, &head, &why),
	         qPrintable(why));
	QCOMPARE(head.bands.size(), size_t(1));
	QCOMPARE(head.bands[0].band(), bbq_band::hourly);
	QCOMPARE(head.bands[0].fetched_utc(), qint64(1789999000));
	QCOMPARE(head.bands[0].samples()[47].precip_chance, std::optional<double>(47.0));
}

void test_wire::a_head_band_this_build_does_not_know_is_skipped() {
	QString why;
	bbq_series hourly(bbq_band::hourly, QStringLiteral("wunderground"));
	bbq_sample sample;
	sample.start_utc = 1790000000;
	sample.duration_s = 3600;
	sample.temperature = 12.0;
	hourly.set_samples({sample});
	hourly.set_fetched_utc(1789999000);

	/* Two bands, then the first's code rewritten to one no build knows. */
	bbq_series extended(bbq_band::extended, QStringLiteral("open-meteo"));
	extended.set_samples({sample});
	QByteArray body = bbq_wire_head_body(station, 1789999000, {&hourly, &extended}, &why);
	const int code_at = 1 + station.size() + 8 + 1;
	QCOMPARE(uint8_t(body[code_at]), bbq_wire_band_code(bbq_band::hourly));
	body[code_at] = char(200);

	bbq_wire_head head;
	QVERIFY2(bbq_wire_parse_head(body, station, &head, &why), qPrintable(why));
	QCOMPARE(head.bands.size(), size_t(1));
	QCOMPARE(head.bands[0].band(), bbq_band::extended);
}

void test_wire::nothing_unsigned_is_parsed() {
	QString why;
	const QByteArray body = bbq_wire_stations_body(1790000000, {station}, &why);
	const QByteArray file = bbq_wire_file(bbq_wire_kind::stations, body, m_key);
	QStringList stations;

	/* Another key. */
	bbq_signing_key other;
	uint8_t seed[32] = {1};
	other.set_seed(seed);
	QVERIFY(!bbq_wire_read_stations(file, other.public_key(), &stations, &why));
	QVERIFY(why.contains("signature"));

	/* Every single flipped bit, anywhere in the file. */
	for (int i = 0; i < file.size(); ++i) {
		for (int bit = 0; bit < 8; ++bit) {
			QByteArray bad = file;
			bad[i] = char(bad[i] ^ (1 << bit));
			QVERIFY2(!bbq_wire_read_stations(bad, m_key.public_key(), &stations, &why),
			         qPrintable(QStringLiteral("byte %1 bit %2").arg(i).arg(bit)));
		}
	}

	/* Truncated at every length. */
	for (int n = 0; n < file.size(); ++n) {
		QVERIFY(!bbq_wire_read_stations(file.left(n), m_key.public_key(), &stations, &why));
	}
}

void test_wire::a_file_about_something_else_is_refused() {
	QString why;
	bbq_wire_month_rows rows;
	rows.providers = {QStringLiteral("wunderground")};
	bbq_sample sample;
	sample.start_utc = in_september(10);
	sample.duration_s = 300;
	sample.temperature = 3.0;
	rows.samples = {sample};
	rows.provider = {0};
	const QByteArray month = bbq_wire_file(
	        bbq_wire_kind::month, bbq_wire_month_body(station, september, rows, &why), m_key);
	bbq_wire_month_rows back;

	/* Another station's, another month's, and a month read as an index. */
	QVERIFY(!bbq_wire_read_month(month, m_key.public_key(), QStringLiteral("ISTOCK822"),
	                             september, &back, &why));
	QVERIFY(!bbq_wire_read_month(month, m_key.public_key(), station, uint16_t(september + 1),
	                             &back, &why));
	std::vector<bbq_wire_index_entry> entries;
	QVERIFY(!bbq_wire_read_index(month, m_key.public_key(), station, &entries, &why));
	QVERIFY(why.contains("kind"));
}

void test_wire::a_file_over_its_cap_is_refused_unread() {
	QString why;
	QStringList stations;
	const QByteArray huge(1024 * 1024 + 1, 'x');
	QVERIFY(!bbq_wire_read_stations(huge, m_key.public_key(), &stations, &why));
	QVERIFY(why.contains("bytes"));
}

void test_wire::bytes_after_the_last_column_are_refused() {
	/*
	 * Refused twice over since situ 217ba65: its check() enforces
	 * `trailing[remaining] max 0`, and the reader checks exact lengths.
	 * With the reader's check disabled, the columns case failed at
	 * b186326 and passes at 217ba65.
	 */
	QString why;
	bbq_wire_month_rows rows;
	const QByteArray good = month_around(
	        columns_by_hand({in_september(0), in_september(5)}, 0x03, 2, {100, 101}), 2);
	QVERIFY2(bbq_wire_parse_month(good, station, september, &rows, &why), qPrintable(why));

	const QByteArray tail = month_around(
	        columns_by_hand({in_september(0), in_september(5)}, 0x03, 2, {100, 101},
	                        QByteArray(1, '\0')),
	        2);
	QVERIFY(!bbq_wire_parse_month(tail, station, september, &rows, &why));

	/* And after the body itself, for every kind. */
	QVERIFY(!bbq_wire_parse_month(good + QByteArray(1, '\0'), station, september, &rows, &why));

	const QByteArray manifest = bbq_wire_stations_body(1, {station}, &why);
	QStringList stations;
	QVERIFY(bbq_wire_parse_stations(manifest, &stations, &why));
	QVERIFY(!bbq_wire_parse_stations(manifest + QByteArray(1, '\0'), &stations, &why));

	bbq_wire_index_entry entry;
	entry.month = september;
	entry.rows = 1;
	entry.bytes = 100;
	entry.digest = QByteArray(16, 'd');
	entry.first_utc = entry.last_utc = in_september(0);
	const QByteArray index = bbq_wire_index_body(station, 1, {entry}, &why);
	std::vector<bbq_wire_index_entry> entries;
	QVERIFY(bbq_wire_parse_index(index, station, &entries, &why));
	QVERIFY(!bbq_wire_parse_index(index + QByteArray(1, '\0'), station, &entries, &why));

	bbq_series hourly(bbq_band::hourly, QStringLiteral("wunderground"));
	bbq_sample sample;
	sample.start_utc = in_september(0);
	sample.duration_s = 3600;
	sample.temperature = 1.0;
	hourly.set_samples({sample});
	const QByteArray head_body = bbq_wire_head_body(station, 1, {&hourly}, &why);
	bbq_wire_head head;
	QVERIFY(bbq_wire_parse_head(head_body, station, &head, &why));
	QVERIFY(!bbq_wire_parse_head(head_body + QByteArray(1, '\0'), station, &head, &why));
}

void test_wire::a_count_that_is_not_its_bitmap_is_refused() {
	QString why;
	bbq_wire_month_rows rows;

	/* Two bits set, one value: the second sample's value would be invented. */
	const QByteArray short_count = month_around(
	        columns_by_hand({in_september(0), in_september(5)}, 0x03, 1, {100}), 2);
	QVERIFY(!bbq_wire_parse_month(short_count, station, september, &rows, &why));

	/* One bit set, two values: the second would be dropped silently. */
	const QByteArray long_count = month_around(
	        columns_by_hand({in_september(0), in_september(5)}, 0x01, 2, {100, 101}), 2);
	QVERIFY(!bbq_wire_parse_month(long_count, station, september, &rows, &why));
}

void test_wire::a_set_padding_bit_is_refused() {
	QString why;
	bbq_wire_month_rows rows;
	/*
	 * Two rows, three bits: the third names a row that does not exist.
	 *
	 * Held three ways, and this case cannot tell them apart: the padding
	 * check, the count (which counts every bit, padding included), and
	 * the exact length (a value nobody reads is left over). Disabling
	 * the padding check alone left this passing, which is the other two
	 * doing their job rather than a gap -- recorded so that nobody reads
	 * it as one.
	 */
	const QByteArray padded = month_around(
	        columns_by_hand({in_september(0), in_september(5)}, 0x07, 3, {100, 101, 102}), 2);
	QVERIFY(!bbq_wire_parse_month(padded, station, september, &rows, &why));
}

void test_wire::samples_that_do_not_ascend_are_refused() {
	QString why;
	bbq_wire_month_rows rows;
	const QByteArray backwards = month_around(
	        columns_by_hand({in_september(5), in_september(0)}, 0x03, 2, {100, 101}), 2);
	QVERIFY(!bbq_wire_parse_month(backwards, station, september, &rows, &why));
	const QByteArray repeated = month_around(
	        columns_by_hand({in_september(5), in_september(5)}, 0x03, 2, {100, 101}), 2);
	QVERIFY(!bbq_wire_parse_month(repeated, station, september, &rows, &why));
}

void test_wire::a_sample_outside_its_month_is_refused() {
	QString why;
	bbq_wire_month_rows rows;
	const qint64 october = bbq_wire_month_end(september);
	const QByteArray outside = month_around(
	        columns_by_hand({in_september(0), october}, 0x03, 2, {100, 101}), 2);
	QVERIFY(!bbq_wire_parse_month(outside, station, september, &rows, &why));
}

void test_wire::provider_runs_must_cover_the_rows_exactly() {
	QString why;
	bbq_wire_month_rows rows;
	const QByteArray inflated =
	        columns_by_hand({in_september(0), in_september(5)}, 0x03, 2, {100, 101});

	QVERIFY(!bbq_wire_parse_month(month_around(inflated, 1), station, september, &rows, &why));
	QVERIFY(!bbq_wire_parse_month(month_around(inflated, 3), station, september, &rows, &why));
	QVERIFY(!bbq_wire_parse_month(month_around(inflated, 2, 1), station, september, &rows,
	                              &why));
	QVERIFY2(bbq_wire_parse_month(month_around(inflated, 2), station, september, &rows, &why),
	         qPrintable(why));
}

void test_wire::a_raw_length_past_the_cap_is_refused_before_inflating() {
	/*
	 * qUncompress allocates whatever the first four bytes claim, so a
	 * claim of 2 GB must be refused from the claim, not discovered by
	 * attempting it.
	 */
	QString why;
	bbq_wire_month_rows rows;
	QByteArray packed = bbq_wire_packed(
	        columns_by_hand({in_september(0)}, 0x01, 1, {100}));
	/* After the u32 packed length: qCompress's own big-endian raw length. */
	packed[4] = char(0x7f);
	packed[5] = char(0xff);
	packed[6] = char(0xff);
	packed[7] = char(0xff);

	QByteArray body = month_around(QByteArray(), 1);
	body.chop(int(bbq_wire_packed(QByteArray()).size()));
	body.append(packed);
	QVERIFY(!bbq_wire_parse_month(body, station, september, &rows, &why));
}

void test_wire::index_months_must_ascend() {
	QString why;
	auto entry = [](uint16_t month) {
		bbq_wire_index_entry e;
		e.month = month;
		e.rows = 1;
		e.bytes = 100;
		e.digest = QByteArray(16, 'd');
		e.first_utc = e.last_utc = bbq_wire_month_begin(month);
		return e;
	};

	/* The encoder refuses to write it, so the body is spliced by hand. */
	QByteArray good = bbq_wire_index_body(station, 1, {entry(september), entry(uint16_t(september + 1))}, &why);
	std::vector<bbq_wire_index_entry> entries;
	QVERIFY2(bbq_wire_parse_index(good, station, &entries, &why), qPrintable(why));

	const int first = 1 + station.size() + 8 + 2;
	QByteArray swapped = good;
	swapped.replace(first, 42, good.mid(first + 42, 42));
	swapped.replace(first + 42, 42, good.mid(first, 42));
	QVERIFY(!bbq_wire_parse_index(swapped, station, &entries, &why));

	QByteArray repeated = good;
	repeated.replace(first + 42, 42, good.mid(first, 42));
	QVERIFY(!bbq_wire_parse_index(repeated, station, &entries, &why));
}

QTEST_GUILESS_MAIN(test_wire)
#include "test_wire.moc"
