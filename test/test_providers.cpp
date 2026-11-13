#include <QJsonDocument>
#include <QTest>

#include "met/nowcast.h"
#include "openmeteo/forecast.h"

/*
 * The second and third providers (project.md sec 2.9, sec 2.10).
 *
 * Weighted entirely towards conversions, because that is what a second
 * provider actually is: the same quantities in somebody else's units,
 * spelling and clock. Every one of these fails silently -- a wind speed
 * out by 3.6, a rain rate out by 4, a whole series shifted by hours --
 * and none of them produces an error, a warning, or a curve that looks
 * obviously wrong.
 */
class test_providers : public QObject {
	Q_OBJECT

private slots:
	void met_converts_wind_from_metres_per_second();
	void met_discards_times_that_do_not_ascend();
	void an_unreadable_timestamp_discards_the_band();
	void met_keeps_its_rain_rate();
	void met_reads_zulu_time_as_utc();
	void openmeteo_reads_local_time_through_the_named_zone();
	void openmeteo_without_a_zone_is_discarded();
	void openmeteo_converts_hourly_rain_to_a_rate();
	void openmeteo_keeps_wind_that_is_already_kph();
	void a_regional_model_is_read_quarter_hourly_to_its_horizon();
	void the_regional_band_ranks_between_hourly_and_the_nowcasts();
	void a_place_no_regional_model_reaches_is_asked_about_once();

private:
	static QJsonDocument parse(const char *json);
	static QJsonDocument met_fixture();
	static QJsonDocument openmeteo_fixture(const char *zone);
};

QJsonDocument test_providers::parse(const char *json) {
	return QJsonDocument::fromJson(QByteArray(json));
}

QJsonDocument test_providers::met_fixture() {
	return parse(R"({"properties": {"timeseries": [
		{"time": "2026-08-07T12:00:00Z", "data": {"instant": {"details": {
			"air_temperature": 21.7, "precipitation_rate": 0.4,
			"wind_speed": 2.5}}}},
		{"time": "2026-08-07T12:05:00Z", "data": {"instant": {"details": {
			"air_temperature": 21.5, "precipitation_rate": 0.0,
			"wind_speed": 3.0}}}}
	]}})");
}

QJsonDocument test_providers::openmeteo_fixture(const char *zone) {
	QByteArray json = R"({
		"timezone": "ZONE",
		"hourly": {
			"time": ["2026-08-07T00:00", "2026-08-07T01:00"],
			"temperature_2m": [18.1, 17.9],
			"precipitation": [1.0, 0.0],
			"wind_speed_10m": [23.4, 23.0],
			"precipitation_probability": [7, 6]
		}
	})";
	json.replace("ZONE", zone);
	return QJsonDocument::fromJson(json);
}

/*
 * Each span is the distance to the next start, so a repeated time gave a
 * span of nought and one out of order a negative span (sec 16.132). The
 * band goes, as for an unreadable timestamp. And a wind speed that
 * overflows the conversion to km/h is no reading, not an infinite one.
 */
void test_providers::met_discards_times_that_do_not_ascend() {
	const QJsonDocument repeated = QJsonDocument::fromJson(R"({"properties": {"timeseries": [
		{"time": "2026-08-07T12:00:00Z", "data": {"instant": {"details": {"air_temperature": 21.7}}}},
		{"time": "2026-08-07T12:00:00Z", "data": {"instant": {"details": {"air_temperature": 21.5}}}}
	]}})");
	QVERIFY(bbq_met_read_nowcast(repeated).is_empty());

	const QJsonDocument gale = QJsonDocument::fromJson(R"({"properties": {"timeseries": [
		{"time": "2026-08-07T12:00:00Z", "data": {"instant": {"details": {"wind_speed": 1e308}}}},
		{"time": "2026-08-07T12:05:00Z", "data": {"instant": {"details": {"wind_speed": 3.0}}}}
	]}})");
	const bbq_series series = bbq_met_read_nowcast(gale);
	QCOMPARE(series.size(), std::size_t(2));
	QVERIFY(!series.samples()[0].wind_kph.has_value());
	QVERIFY(series.samples()[1].wind_kph.has_value());
}

void test_providers::met_converts_wind_from_metres_per_second() {
	/*
	 * MET reports m/s and the model stores km/h. Out by 3.6 is a wind
	 * that never looks alarming and quietly stops sec 7 from ever
	 * penalising a gale.
	 */
	const bbq_series series = bbq_met_read_nowcast(met_fixture());
	QCOMPARE(series.size(), std::size_t(2));

	QVERIFY(series.samples()[0].wind_kph.has_value());
	QVERIFY(std::fabs(*series.samples()[0].wind_kph - 9.0) < 1e-9);
	QVERIFY(std::fabs(*series.samples()[1].wind_kph - 10.8) < 1e-9);
}

void test_providers::met_keeps_its_rain_rate() {
	/*
	 * Already mm/h. The trap here is the opposite of WU's hourly band:
	 * dividing something that was never an accumulation.
	 */
	const bbq_series series = bbq_met_read_nowcast(met_fixture());
	QVERIFY(std::fabs(*series.samples()[0].precip_rate - 0.4) < 1e-9);

	/* And it is the radar band, not the ordinary nowcast (sec 2.9.1). */
	QCOMPARE(series.band(), bbq_band::nowcast_fine);
	QCOMPARE(series.samples()[0].duration_s, 300);
}

void test_providers::met_reads_zulu_time_as_utc() {
	const bbq_series series = bbq_met_read_nowcast(met_fixture());

	const QDateTime expected(QDate(2026, 8, 7), QTime(12, 0), bbq_utc());
	QCOMPARE(series.samples()[0].start_utc, expected.toSecsSinceEpoch());
}

/*
 * A native quarter-hourly model (sec 2.10.5). Fifteen-minute samples,
 * rain as a rate over the quarter hour, and nothing past the first row
 * the model leaves without a temperature: beyond its horizon the block
 * is nulls, or columns the provider filled from elsewhere.
 */
void test_providers::a_regional_model_is_read_quarter_hourly_to_its_horizon() {
	const QJsonDocument response = QJsonDocument::fromJson(R"({
		"timezone": "Europe/Berlin",
		"minutely_15": {
			"time": ["2026-08-07T00:00", "2026-08-07T00:15", "2026-08-07T00:30", "2026-08-07T00:45"],
			"temperature_2m": [18.1, 17.9, null, 17.5],
			"precipitation": [0.5, 0.0, 0.2, 0.1],
			"wind_speed_10m": [23.4, 23.0, 22.0, 21.0],
			"precipitation_probability": [7, 6, 5, 5]
		}
	})");

	const bbq_series series =
	        bbq_openmeteo_read_regional(response, QStringLiteral("icon_d2"));
	QCOMPARE(series.band(), bbq_band::regional);
	QCOMPARE(series.size(), std::size_t(2));
	QCOMPARE(series.samples()[0].duration_s, 900);
	QVERIFY(std::fabs(*series.samples()[0].precip_rate - 2.0) < 1e-9);
	QVERIFY(series.provider().contains(QStringLiteral("icon_d2")));

	const QDateTime expected(QDate(2026, 8, 6), QTime(22, 15), bbq_utc());
	QCOMPARE(series.samples()[1].start_utc, expected.toSecsSinceEpoch());

	/* The hourly reader is untouched by it: an hourly block, hourly spans. */
	QCOMPARE(bbq_openmeteo_read(openmeteo_fixture("Europe/Berlin")).samples()[0].duration_s,
	         3600);
}

/*
 * Declared, not computed (sec 3.3), so the declaration is what is held:
 * a model's own quarter hours beat WU's hourly over the same hours, and
 * lose to both nowcasts where those reach.
 */
void test_providers::the_regional_band_ranks_between_hourly_and_the_nowcasts() {
	QVERIFY(bbq_band_priority(bbq_band::regional) > bbq_band_priority(bbq_band::hourly));
	QVERIFY(bbq_band_priority(bbq_band::regional) > bbq_band_priority(bbq_band::extended));
	QVERIFY(bbq_band_priority(bbq_band::regional) < bbq_band_priority(bbq_band::nowcast));
	QVERIFY(bbq_band_priority(bbq_band::regional) < bbq_band_priority(bbq_band::nowcast_fine));
}

/*
 * The asking (sec 2.10.5). Stockholm is outside all three: three
 * questions, then none for as long as the place stays put. Berlin is
 * outside HRRR and inside AROME, which is then the only one asked. And a
 * new place starts the asking again.
 */
void test_providers::a_place_no_regional_model_reaches_is_asked_about_once() {
	bbq_regional_probe probe;
	probe.at(59.34, 18.05);

	QStringList asked;
	while (!probe.to_ask().isEmpty() && asked.size() < 10) {
		const QString model = probe.to_ask();
		asked << model;
		probe.outside(model);
	}
	QCOMPARE(asked, (QStringList{QStringLiteral("ncep_hrrr_conus"),
	                             QStringLiteral("meteofrance_arome_france_hd"),
	                             QStringLiteral("icon_d2")}));

	probe.at(59.34, 18.05);
	QVERIFY(probe.to_ask().isEmpty());

	probe.at(52.52, 13.40);
	QCOMPARE(probe.to_ask(), QStringLiteral("ncep_hrrr_conus"));
	QVERIFY(probe.outside(QStringLiteral("ncep_hrrr_conus")));
	QCOMPARE(probe.to_ask(), QStringLiteral("meteofrance_arome_france_hd"));
	probe.answered(QStringLiteral("meteofrance_arome_france_hd"));
	probe.at(52.52, 13.40);
	QCOMPARE(probe.to_ask(), QStringLiteral("meteofrance_arome_france_hd"));
}

void test_providers::openmeteo_reads_local_time_through_the_named_zone() {
	/*
	 * The sharpest trap in either provider. Open-Meteo's stamps are
	 * local and carry NO offset, so a reader that treated them as UTC
	 * would shift the entire series by hours and produce a graph that
	 * looks completely valid.
	 *
	 * Midnight in Stockholm on 7 August is 22:00 UTC on the 6th.
	 */
	const bbq_series series = bbq_openmeteo_read(
	        openmeteo_fixture("Europe/Stockholm"));
	QCOMPARE(series.size(), std::size_t(2));

	const QDateTime expected(QDate(2026, 8, 6), QTime(22, 0), bbq_utc());
	QCOMPARE(series.samples()[0].start_utc, expected.toSecsSinceEpoch());
	QCOMPARE(series.band(), bbq_band::extended);

	/* And the band carries that zone onwards, for sec 3.12.1. */
	QVERIFY(series.zone().isValid());
	QCOMPARE(series.zone().id(), QByteArray("Europe/Stockholm"));
}

void test_providers::openmeteo_without_a_zone_is_discarded() {
	/*
	 * Discarded rather than guessed. Sec 2.10.1: with no offset in the
	 * stamps there is nothing to fall back TO, and reading them in the
	 * wrong zone is the failure this whole test file exists for.
	 */
	QVERIFY(bbq_openmeteo_read(openmeteo_fixture("Not/AZone")).is_empty());
	QVERIFY(bbq_openmeteo_read(openmeteo_fixture("")).is_empty());
}

void test_providers::openmeteo_converts_hourly_rain_to_a_rate() {
	/*
	 * Millimetres over the hour becomes millimetres per hour, which is
	 * a division by one and is written out anyway -- the day the block
	 * changes cadence again, the shortcut is the bug (sec 3.2).
	 *
	 * The block is `hourly` rather than `minutely_15` on purpose: the
	 * finer one is interpolated outside a few model domains, and
	 * marking interpolated points as samples is what sec 3.11.3
	 * forbids (sec 2.10.4).
	 */
	const bbq_series series = bbq_openmeteo_read(
	        openmeteo_fixture("Europe/Stockholm"));

	QVERIFY(series.samples()[0].precip_rate.has_value());
	QVERIFY(std::fabs(*series.samples()[0].precip_rate - 1.0) < 1e-9);
	QCOMPARE(series.samples()[0].duration_s, 3600);
}

void test_providers::openmeteo_keeps_wind_that_is_already_kph() {
	/*
	 * The counterpart to the MET test, and the reason both exist: two
	 * providers, two units, and the wrong assumption in either
	 * direction is invisible. Open-Meteo is asked for km/h and gives
	 * it, so multiplying by 3.6 here would be as wrong as not
	 * multiplying there.
	 */
	const bbq_series series = bbq_openmeteo_read(
	        openmeteo_fixture("Europe/Stockholm"));

	QVERIFY(std::fabs(*series.samples()[0].wind_kph - 23.4) < 1e-9);
	QCOMPARE(*series.samples()[0].precip_chance, 7.0);
}

QTEST_APPLESS_MAIN(test_providers)
#include "test_providers.moc"

/*
 * One bad timestamp discards the band, in the two parsers where nothing
 * had said so.
 *
 * The WU readers have had this asserted since a null became a sample at
 * the epoch (sec 12.13). met.no and Open-Meteo both guard it too, and
 * both cite the WU readers' reason in a comment -- a series short by an
 * arbitrary sample from its middle draws a gap that means nothing. What
 * neither had was a test, so the guards were correct and unprotected:
 * exactly the shape that let a call be deleted from main this same day
 * with everything still green (sec 16.71.2).
 *
 * ONE BAD STAMP AMONG GOOD ONES, not a wholly broken document. A parser
 * that rejects nonsense outright would pass a test made of nonsense
 * while still admitting the case that matters.
 */
void test_providers::an_unreadable_timestamp_discards_the_band() {
	const QJsonDocument met = QJsonDocument::fromJson(R"({
		"properties": {
			"timeseries": [
				{
					"time": "2026-08-07T12:00:00Z",
					"data": {"instant": {"details": {"air_temperature": 19}}}
				},
				{
					"time": "not a timestamp",
					"data": {"instant": {"details": {"air_temperature": 20}}}
				}
			]
		}
	})");

	QVERIFY2(bbq_met_read_nowcast(met).is_empty(),
	         "met.no kept a band whose second timestamp is unreadable, so "
	         "the series is short by a sample nobody can see");

	const QJsonDocument open = QJsonDocument::fromJson(R"({
		"timezone": "Europe/Stockholm",
		"hourly": {
			"time": ["2026-08-07T12:00", "not a timestamp"],
			"temperature_2m": [19, 20],
			"precipitation": [0, 0],
			"wind_speed_10m": [5, 5]
		}
	})");

	QVERIFY2(bbq_openmeteo_read(open).is_empty(),
	         "Open-Meteo kept a band whose second timestamp is unreadable");

	/*
	 * And the control: the same documents with both stamps good must
	 * NOT come back empty, or the assertions above would pass against a
	 * parser that rejects everything.
	 */
	const QJsonDocument met_good = QJsonDocument::fromJson(R"({
		"properties": {
			"timeseries": [
				{
					"time": "2026-08-07T12:00:00Z",
					"data": {"instant": {"details": {"air_temperature": 19}}}
				},
				{
					"time": "2026-08-07T12:05:00Z",
					"data": {"instant": {"details": {"air_temperature": 20}}}
				}
			]
		}
	})");

	QVERIFY2(!bbq_met_read_nowcast(met_good).is_empty(),
	         "the met.no control came back empty, so the assertion above "
	         "proves nothing about the timestamp");
}
