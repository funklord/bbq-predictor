#include <QTemporaryDir>
#include <QTest>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "serve/serve_core.h"

/*
 * The daemon's pure half (project.md sec 15.12.2). Its request parser is
 * the one piece of this project an anonymous stranger can drive, so the
 * cases are the ways a request could carry something the parser did not
 * check, and the ways a file on disk could be served when it should not.
 */
class test_serve : public QObject {
	Q_OBJECT

private slots:
	void a_well_formed_request_parses();
	void only_exactly_forty_eight_bytes_are_a_request();
	void magic_version_and_kind_are_checked();
	void month_is_only_for_month_requests();
	void station_ids_have_one_spelling();
	void month_names_round_trip_and_nothing_else_parses();

	void the_table_serves_what_the_format_names();
	void the_table_refuses_what_it_should_not_serve();
	void a_client_holding_the_file_is_told_unchanged();
	void an_empty_table_answers_nothing_served();

private:
	static QByteArray request(uint8_t kind, uint16_t month,
	                          const char *station, const uint8_t *have = nullptr);

	/* A file of `size` bytes whose last 64 are a recognisable "signature". */
	static QByteArray file(size_t size, uint8_t signature_byte);
	static void put(const QString &path, const QByteArray &bytes);
};

QByteArray test_serve::request(uint8_t kind, uint16_t month,
                               const char *station, const uint8_t *have) {
	QByteArray buffer(BBQ_SERVE_REQUEST_SIZE, '\0');
	std::memcpy(buffer.data(), "BBQR", 4);
	buffer[4] = 1;
	buffer[5] = char(kind);
	buffer[6] = char(month & 0xff);
	buffer[7] = char(month >> 8);
	std::memcpy(buffer.data() + 8, station,
	            std::min(std::strlen(station), size_t(BBQ_SERVE_STATION_MAX)));
	if (have) {
		std::memcpy(buffer.data() + 40, have, BBQ_SERVE_HAVE_SIZE);
	}
	return buffer;
}

QByteArray test_serve::file(size_t size, uint8_t signature_byte) {
	QByteArray bytes(int(size), 'x');
	std::memset(bytes.data() + size - BBQ_SERVE_SIGNATURE_SIZE, signature_byte,
	            BBQ_SERVE_SIGNATURE_SIZE);
	return bytes;
}

void test_serve::put(const QString &path, const QByteArray &bytes) {
	QFile out(path);
	QVERIFY(out.open(QIODevice::WriteOnly));
	QCOMPARE(out.write(bytes), qint64(bytes.size()));
}

static int parse(const QByteArray &buffer, bbq_serve_request *out) {
	return bbq_serve_parse_request(
	        reinterpret_cast<const uint8_t *>(buffer.constData()),
	        size_t(buffer.size()), out);
}

static uint16_t month_of(int year, int number) {
	return uint16_t(year * 12 + number - 1);
}

void test_serve::a_well_formed_request_parses() {
	bbq_serve_request parsed;

	QCOMPARE(parse(request(BBQ_SERVE_STATIONS, 0, ""), &parsed), 0);
	QCOMPARE(parsed.kind, uint8_t(BBQ_SERVE_STATIONS));
	QCOMPARE(parsed.station[0], '\0');

	QCOMPARE(parse(request(BBQ_SERVE_HEAD, 0, "ISTOCK877"), &parsed), 0);
	QCOMPARE(QByteArray(parsed.station), QByteArray("ISTOCK877"));

	QCOMPARE(parse(request(BBQ_SERVE_MONTH, month_of(2026, 9), "ISTOCK877"),
	               &parsed),
	         0);
	QCOMPARE(parsed.month, month_of(2026, 9));

	/* The full 32 characters, with no room left for a NUL. */
	const char *longest = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
	QCOMPARE(parse(request(BBQ_SERVE_INDEX, 0, longest), &parsed), 0);
	QCOMPARE(QByteArray(parsed.station), QByteArray(longest));
}

void test_serve::only_exactly_forty_eight_bytes_are_a_request() {
	bbq_serve_request parsed;
	const QByteArray good = request(BBQ_SERVE_HEAD, 0, "ISTOCK877");

	QCOMPARE(parse(good.left(47), &parsed), -1);
	QCOMPARE(parse(good + '\0', &parsed), -1);
	QCOMPARE(bbq_serve_parse_request(nullptr, 0, &parsed), -1);
}

void test_serve::magic_version_and_kind_are_checked() {
	bbq_serve_request parsed;

	QByteArray bad = request(BBQ_SERVE_HEAD, 0, "ISTOCK877");
	bad[0] = 'X';
	QCOMPARE(parse(bad, &parsed), -1);

	bad = request(BBQ_SERVE_HEAD, 0, "ISTOCK877");
	bad[4] = 2;
	QCOMPARE(parse(bad, &parsed), -1);

	QCOMPARE(parse(request(0, 0, "ISTOCK877"), &parsed), -1);
	QCOMPARE(parse(request(5, 0, "ISTOCK877"), &parsed), -1);
	QCOMPARE(parse(request(255, 0, "ISTOCK877"), &parsed), -1);
}

void test_serve::month_is_only_for_month_requests() {
	bbq_serve_request parsed;

	QCOMPARE(parse(request(BBQ_SERVE_HEAD, month_of(2026, 9), "ISTOCK877"),
	               &parsed),
	         -1);
	QCOMPARE(parse(request(BBQ_SERVE_MONTH, 0, "ISTOCK877"), &parsed), -1);
	QCOMPARE(parse(request(BBQ_SERVE_MONTH, month_of(1969, 12), "ISTOCK877"),
	               &parsed),
	         -1);
	QCOMPARE(parse(request(BBQ_SERVE_MONTH, month_of(2200, 1), "ISTOCK877"),
	               &parsed),
	         -1);
	QCOMPARE(parse(request(BBQ_SERVE_MONTH, month_of(1970, 1), "ISTOCK877"),
	               &parsed),
	         0);
}

void test_serve::station_ids_have_one_spelling() {
	bbq_serve_request parsed;

	/* The path an attacker would want, and its neighbours. */
	QCOMPARE(parse(request(BBQ_SERVE_HEAD, 0, "../ETC"), &parsed), -1);
	QCOMPARE(parse(request(BBQ_SERVE_HEAD, 0, "A/B"), &parsed), -1);
	QCOMPARE(parse(request(BBQ_SERVE_HEAD, 0, "istock877"), &parsed), -1);
	QCOMPARE(parse(request(BBQ_SERVE_HEAD, 0, "A.B"), &parsed), -1);
	QCOMPARE(parse(request(BBQ_SERVE_HEAD, 0, ""), &parsed), -1);

	/* A station where none belongs. */
	QCOMPARE(parse(request(BBQ_SERVE_STATIONS, 0, "ISTOCK877"), &parsed), -1);

	/* Bytes after the terminating NUL. */
	QByteArray smuggled = request(BBQ_SERVE_HEAD, 0, "ISTOCK877");
	smuggled[8 + 20] = 'Z';
	QCOMPARE(parse(smuggled, &parsed), -1);

	/* A high byte, which a signed char comparison could let through. */
	QByteArray high = request(BBQ_SERVE_HEAD, 0, "ISTOCK877");
	high[8] = char(0xc1);
	QCOMPARE(parse(high, &parsed), -1);
}

void test_serve::month_names_round_trip_and_nothing_else_parses() {
	char name[8];
	uint16_t month = 0;

	QCOMPARE(bbq_serve_month_name(month_of(2026, 9), name), 0);
	QCOMPARE(QByteArray(name), QByteArray("2026-09"));
	QCOMPARE(bbq_serve_parse_month_name(name, &month), 0);
	QCOMPARE(month, month_of(2026, 9));

	QCOMPARE(bbq_serve_month_name(month_of(1969, 12), name), -1);

	const char *refused[] = {"2026-13", "2026-00", "2026-1x", "2026_09",
	                         "1969-12", "2200-01", "202-091", "2026-9\0"};
	for (const char *candidate : refused) {
		char padded[8] = {};
		std::memcpy(padded, candidate, 7);
		QVERIFY2(bbq_serve_parse_month_name(padded, &month) != 0, candidate);
	}

	/* Exactly seven characters: an eighth is not a month. */
	QCOMPARE(bbq_serve_parse_month_name("2026-091", &month), -1);
}

void test_serve::the_table_serves_what_the_format_names() {
	QTemporaryDir root;
	QVERIFY(root.isValid());
	const QString dir = root.path();

	put(dir + "/stations", file(100, 0x11));
	QVERIFY(QDir(dir).mkpath("ISTOCK877"));
	put(dir + "/ISTOCK877/index", file(120, 0x22));
	put(dir + "/ISTOCK877/head", file(130, 0x33));
	put(dir + "/ISTOCK877/2026-09", file(140, 0x44));
	put(dir + "/ISTOCK877/2026-08", file(150, 0x55));

	const int fd = ::open(QFile::encodeName(dir).constData(),
	                      O_RDONLY | O_DIRECTORY);
	QVERIFY(fd >= 0);
	bbq_serve_table *table = bbq_serve_table_load(fd);
	::close(fd);
	QVERIFY(table);

	QCOMPARE(table->station_count, size_t(1));
	QCOMPARE(table->station_list[0].month_count, size_t(2));

	/* Sorted, whatever order the directory listed them in. */
	QCOMPARE(table->station_list[0].months[0], month_of(2026, 8));
	QCOMPARE(table->station_list[0].months[1], month_of(2026, 9));

	const struct {
		uint8_t kind;
		uint16_t month;
		const char *station;
		uint32_t size;
	} cases[] = {
		{BBQ_SERVE_STATIONS, 0, "", 100},
		{BBQ_SERVE_INDEX, 0, "ISTOCK877", 120},
		{BBQ_SERVE_HEAD, 0, "ISTOCK877", 130},
		{BBQ_SERVE_MONTH, month_of(2026, 9), "ISTOCK877", 140},
		{BBQ_SERVE_MONTH, month_of(2026, 8), "ISTOCK877", 150},
	};
	for (const auto &one : cases) {
		bbq_serve_request parsed;
		QCOMPARE(parse(request(one.kind, one.month, one.station), &parsed), 0);

		bbq_serve_blob *blob = nullptr;
		QCOMPARE(bbq_serve_answer(table, &parsed, &blob), BBQ_SERVE_OK);
		QVERIFY(blob);
		QCOMPARE(blob->size, one.size);
		bbq_serve_blob_unref(blob);
	}

	bbq_serve_table_free(table);
}

void test_serve::the_table_refuses_what_it_should_not_serve() {
	QTemporaryDir root;
	QVERIFY(root.isValid());
	const QString dir = root.path();

	/* A secret beside the tree, which a symlink inside it points at. */
	QTemporaryDir outside;
	QVERIFY(outside.isValid());
	put(outside.path() + "/secret", file(200, 0x66));

	QVERIFY(QDir(dir).mkpath("ISTOCK877"));
	put(dir + "/ISTOCK877/2026-09", file(140, 0x44));
	QVERIFY(QFile::link(outside.path() + "/secret", dir + "/ISTOCK877/head"));
	QVERIFY(QFile::link(outside.path(), dir + "/LINKED"));

	/* Too small to hold a header and a signature, and over the cap. */
	put(dir + "/ISTOCK877/index", file(BBQ_SERVE_FILE_MIN - 1 + 64, 0x22).left(
	                                      BBQ_SERVE_FILE_MIN - 1));
	put(dir + "/ISTOCK877/2026-08", file(BBQ_SERVE_MONTH_FILE_MAX + 1, 0x55));

	/* Names the format does not define. */
	QVERIFY(QDir(dir).mkpath("lowercase"));
	put(dir + "/lowercase/head", file(130, 0x33));
	put(dir + "/ISTOCK877/2026-9", file(140, 0x77));
	put(dir + "/ISTOCK877/notes", file(140, 0x77));
	QVERIFY(QDir(dir).mkpath("ISTOCK877/2026-10"));

	const int fd = ::open(QFile::encodeName(dir).constData(),
	                      O_RDONLY | O_DIRECTORY);
	QVERIFY(fd >= 0);
	bbq_serve_table *table = bbq_serve_table_load(fd);
	::close(fd);
	QVERIFY(table);

	QVERIFY(!table->stations);
	QCOMPARE(table->station_count, size_t(1));

	const bbq_serve_station &station = table->station_list[0];
	QCOMPARE(QByteArray(station.id), QByteArray("ISTOCK877"));
	QVERIFY(!station.head);
	QVERIFY(!station.index);
	QCOMPARE(station.month_count, size_t(1));
	QCOMPARE(station.months[0], month_of(2026, 9));

	bbq_serve_request parsed;
	bbq_serve_blob *blob = nullptr;
	QCOMPARE(parse(request(BBQ_SERVE_HEAD, 0, "ISTOCK877"), &parsed), 0);
	QCOMPARE(bbq_serve_answer(table, &parsed, &blob), BBQ_SERVE_NOT_SERVED);
	QVERIFY(!blob);

	QCOMPARE(parse(request(BBQ_SERVE_HEAD, 0, "LINKED"), &parsed), 0);
	QCOMPARE(bbq_serve_answer(table, &parsed, &blob), BBQ_SERVE_NOT_SERVED);

	QCOMPARE(parse(request(BBQ_SERVE_HEAD, 0, "ISTOCK000"), &parsed), 0);
	QCOMPARE(bbq_serve_answer(table, &parsed, &blob), BBQ_SERVE_NOT_SERVED);

	bbq_serve_table_free(table);
}

void test_serve::an_empty_table_answers_nothing_served() {
	/*
	 * Found by fuzz_serve.c: a month asked of a station holding no
	 * months reached bsearch with a NULL array, which is undefined
	 * even with a count of zero. Only a sanitizer sees it, so this
	 * case pins the answer and the fuzzer pins the behaviour.
	 */
	bbq_serve_station bare = {};
	std::strcpy(bare.id, "A1");
	bbq_serve_table one = {};
	one.station_list = &bare;
	one.station_count = 1;
	bbq_serve_table none = {};

	bbq_serve_request parsed;
	bbq_serve_blob *blob = nullptr;
	QCOMPARE(parse(request(BBQ_SERVE_MONTH, month_of(2026, 9), "A1"), &parsed), 0);
	QCOMPARE(bbq_serve_answer(&one, &parsed, &blob), BBQ_SERVE_NOT_SERVED);
	QCOMPARE(bbq_serve_answer(&none, &parsed, &blob), BBQ_SERVE_NOT_SERVED);
	QCOMPARE(parse(request(BBQ_SERVE_STATIONS, 0, ""), &parsed), 0);
	QCOMPARE(bbq_serve_answer(&none, &parsed, &blob), BBQ_SERVE_NOT_SERVED);
	QVERIFY(!blob);
}

void test_serve::a_client_holding_the_file_is_told_unchanged() {
	QTemporaryDir root;
	QVERIFY(root.isValid());
	QVERIFY(QDir(root.path()).mkpath("ISTOCK877"));
	put(root.path() + "/ISTOCK877/head", file(130, 0x33));

	const int fd = ::open(QFile::encodeName(root.path()).constData(),
	                      O_RDONLY | O_DIRECTORY);
	QVERIFY(fd >= 0);
	bbq_serve_table *table = bbq_serve_table_load(fd);
	::close(fd);
	QVERIFY(table);

	uint8_t have[BBQ_SERVE_HAVE_SIZE];
	std::memset(have, 0x33, sizeof have);

	bbq_serve_request parsed;
	bbq_serve_blob *blob = nullptr;
	QCOMPARE(parse(request(BBQ_SERVE_HEAD, 0, "ISTOCK877", have), &parsed), 0);
	QCOMPARE(bbq_serve_answer(table, &parsed, &blob), BBQ_SERVE_UNCHANGED);
	QVERIFY(!blob);

	/* A different copy is sent in full. */
	std::memset(have, 0x34, sizeof have);
	QCOMPARE(parse(request(BBQ_SERVE_HEAD, 0, "ISTOCK877", have), &parsed), 0);
	QCOMPARE(bbq_serve_answer(table, &parsed, &blob), BBQ_SERVE_OK);
	bbq_serve_blob_unref(blob);

	/* And holding nothing is never "unchanged", even against a zero signature. */
	put(root.path() + "/ISTOCK877/index", file(130, 0x00));
	bbq_serve_table_free(table);
	const int again = ::open(QFile::encodeName(root.path()).constData(),
	                         O_RDONLY | O_DIRECTORY);
	table = bbq_serve_table_load(again);
	::close(again);
	QCOMPARE(parse(request(BBQ_SERVE_INDEX, 0, "ISTOCK877"), &parsed), 0);
	QCOMPARE(bbq_serve_answer(table, &parsed, &blob), BBQ_SERVE_OK);
	bbq_serve_blob_unref(blob);

	bbq_serve_table_free(table);
}

QTEST_GUILESS_MAIN(test_serve)
#include "test_serve.moc"
