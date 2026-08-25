#include <QJsonDocument>
#include <QTest>

#include "wu/reader.h"

/*
 * The readers, against fixtures shaped like the real responses
 * (project.md sec 2.6.2, sec 2.6.3).
 *
 * Small hand-written fixtures rather than captured ones, deliberately.
 * A saved capture would carry an API key, would be tens of kilobytes of
 * mostly-irrelevant fields, and would need re-capturing whenever WU
 * changed something unrelated. What these assert is the SHAPE and the
 * traps, and both are stated in sec 2.6 precisely enough to write down.
 */
class test_reader : public QObject {
	Q_OBJECT

private slots:
	void hourly_converts_accumulation_to_a_rate();
	void nowcast_reads_local_time_with_an_offset();
	void observed_sorts_rows_that_arrive_newest_first();
	void observed_takes_the_station_zone_and_wind();
	void a_malformed_band_comes_back_empty();
	void a_bad_utc_timestamp_does_not_become_1970();

private:
	static QJsonDocument parse(const char *json);
};

QJsonDocument test_reader::parse(const char *json) {
	return QJsonDocument::fromJson(QByteArray(json));
}

void test_reader::hourly_converts_accumulation_to_a_rate() {
	/*
	 * Sec 3.2: qpf is millimetres over the step, and the model stores
	 * millimetres per hour. The step here is deliberately TWO hours, so
	 * a reader that forgot to divide would pass on one-hour data and
	 * fail here -- which is the shortcut the division was written out
	 * to guard against.
	 */
	const QJsonDocument document = parse(R"({
		"validTimeUtc": [1786100000, 1786107200, 1786114400],
		"temperature": [20, 22, 21],
		"qpf": [1.0, 0.0, 2.0],
		"precipChance": [40, 5, 60],
		"windSpeed": [10, 12, 9]
	})");

	const bbq_series series = bbq_wu_read_hourly(document);
	QCOMPARE(series.size(), std::size_t(3));
	QCOMPARE(series.samples()[0].duration_s, 7200);

	/* 1.0 mm across two hours is half a millimetre an hour. */
	QVERIFY(series.samples()[0].precip_rate.has_value());
	QVERIFY(std::fabs(*series.samples()[0].precip_rate - 0.5) < 1e-9);

	QCOMPARE(*series.samples()[2].precip_chance, 60.0);
	QCOMPARE(*series.samples()[1].wind_kph, 12.0);
}

void test_reader::nowcast_reads_local_time_with_an_offset() {
	/*
	 * Sec 2.6.2's awkward case: the finest forecast band carries no
	 * validTimeUtc at all, only a local string, and its offset is what
	 * makes that usable without the station's zone.
	 */
	const QJsonDocument document = parse(R"({
		"validTimeLocal": ["2026-08-07T14:30:00+0200",
		                   "2026-08-07T14:45:00+0200"],
		"temperature": [19, 19],
		"precipRate": [0.0, 0.4],
		"precipChance": [10, 20],
		"windSpeed": [5, 6]
	})");

	const bbq_series series = bbq_wu_read_nowcast(document);
	QCOMPARE(series.size(), std::size_t(2));

	/* 14:30+0200 is 12:30 UTC. */
	const QDateTime expected(QDate(2026, 8, 7), QTime(12, 30), QTimeZone::UTC);
	QCOMPARE(series.samples()[0].start_utc, expected.toSecsSinceEpoch());

	/* precipRate is already mm/h and must not be divided by anything. */
	QVERIFY(std::fabs(*series.samples()[1].precip_rate - 0.4) < 1e-9);
	QVERIFY(series.zone().isValid());
}

void test_reader::observed_sorts_rows_that_arrive_newest_first() {
	/*
	 * Sorting happens in the reader as well as in bbq_series, and this
	 * is why: a duration measured as the distance to the next row is
	 * meaningless if the rows are not in order.
	 */
	const QJsonDocument document = parse(R"({"observations": [
		{"epoch": 1786100600, "tz": "Europe/Stockholm",
		 "metric": {"tempAvg": 18, "precipRate": 0.0, "windspeedAvg": 4}},
		{"epoch": 1786100300, "tz": "Europe/Stockholm",
		 "metric": {"tempAvg": 19, "precipRate": 0.0, "windspeedAvg": 4}},
		{"epoch": 1786100000, "tz": "Europe/Stockholm",
		 "metric": {"tempAvg": 20, "precipRate": 0.0, "windspeedAvg": 4}}
	]})");

	const bbq_series series = bbq_wu_read_observed(document);
	QCOMPARE(series.size(), std::size_t(3));

	QCOMPARE(series.samples()[0].start_utc, qint64(1786100000));
	QCOMPARE(*series.samples()[0].temperature, 20.0);
	QCOMPARE(series.samples()[0].duration_s, 300);
}

void test_reader::observed_takes_the_station_zone_and_wind() {
	/*
	 * Sec 7.4: the same quantity is windSpeed in three places and
	 * windspeedAvg here, and getting it wrong fails silently as a field
	 * that is simply never populated. Which is exactly what a test can
	 * catch and a compiler cannot.
	 */
	const QJsonDocument document = parse(R"({"observations": [
		{"epoch": 1786100000, "tz": "Europe/Stockholm",
		 "metric": {"tempAvg": 20, "precipRate": 0.2, "windspeedAvg": 11}}
	]})");

	const bbq_series series = bbq_wu_read_observed(document);
	QCOMPARE(series.size(), std::size_t(1));

	QVERIFY2(series.samples()[0].wind_kph.has_value(),
	         "wind is unset: the reader is probably looking for windSpeed "
	         "where the history endpoint says windspeedAvg");
	QCOMPARE(*series.samples()[0].wind_kph, 11.0);

	QVERIFY(series.zone().isValid());
	QCOMPARE(series.zone().id(), QByteArray("Europe/Stockholm"));
}

void test_reader::a_malformed_band_comes_back_empty() {
	/*
	 * A series missing an arbitrary sample from its middle draws a gap
	 * that means nothing, and sec 2.6.6 can report an absent band
	 * honestly but not a quietly short one.
	 */
	const QJsonDocument broken = parse(R"({
		"validTimeLocal": ["2026-08-07T14:30:00+0200", "not a timestamp"],
		"temperature": [19, 19]
	})");

	QVERIFY(bbq_wu_read_nowcast(broken).is_empty());
	QVERIFY(bbq_wu_read_hourly(parse("[]")).is_empty());
	QVERIFY(bbq_wu_read_observed(parse(R"({"observations": []})")).is_empty());
}

void test_reader::a_bad_utc_timestamp_does_not_become_1970() {
	/*
	 * The two time paths must be equally strict.
	 *
	 * validTimeLocal already discards the whole band on one unparseable
	 * timestamp, and says why: a series missing an arbitrary sample from
	 * its middle draws a gap that means nothing. validTimeUtc had no
	 * such guard, and QJsonValue::toDouble() answers 0 for anything that
	 * is not a number -- so a single null became a sample at the epoch.
	 *
	 * That is not a small wrong number. The series then claims to begin
	 * on 1 January 1970: begin_utc reports it, the composite's coverage
	 * reports it, and the graph sees a gap of fifty-odd years next to
	 * an hour of weather.
	 */
	const QJsonDocument document = parse(R"({
		"validTimeUtc": [1786100000, null, 1786107200],
		"temperature": [11, 12, 13],
		"qpf": [0, 0, 0]
	})");

	const bbq_series series = bbq_wu_read_hourly(document);

	for (const bbq_sample &sample : series.samples()) {
		QVERIFY2(sample.start_utc > 1000000000,
		         "a sample landed at or near the epoch");
	}

	/* Discarded outright, which is what the local path does. */
	QVERIFY2(series.is_empty(),
	         "a band with an unreadable timestamp was kept anyway");
}

QTEST_APPLESS_MAIN(test_reader)
#include "test_reader.moc"
