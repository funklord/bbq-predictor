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
	void met_keeps_its_rain_rate();
	void met_reads_zulu_time_as_utc();
	void openmeteo_reads_local_time_through_the_named_zone();
	void openmeteo_without_a_zone_is_discarded();
	void openmeteo_converts_quarter_hour_rain_to_a_rate();
	void openmeteo_keeps_wind_that_is_already_kph();

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
		"minutely_15": {
			"time": ["2026-08-07T00:00", "2026-08-07T00:15"],
			"temperature_2m": [18.1, 17.9],
			"precipitation": [0.25, 0.0],
			"wind_speed_10m": [23.4, 23.0],
			"precipitation_probability": [7, 6]
		}
	})";
	json.replace("ZONE", zone);
	return QJsonDocument::fromJson(json);
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

	const QDateTime expected(QDate(2026, 8, 7), QTime(12, 0), QTimeZone::UTC);
	QCOMPARE(series.samples()[0].start_utc, expected.toSecsSinceEpoch());
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

	const QDateTime expected(QDate(2026, 8, 6), QTime(22, 0), QTimeZone::UTC);
	QCOMPARE(series.samples()[0].start_utc, expected.toSecsSinceEpoch());

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

void test_providers::openmeteo_converts_quarter_hour_rain_to_a_rate() {
	/*
	 * Millimetres per quarter hour becomes millimetres per hour, so
	 * 0.25 over fifteen minutes is 1.0 an hour. Forgetting this reports
	 * a cloudburst as a drizzle.
	 */
	const bbq_series series = bbq_openmeteo_read(
	        openmeteo_fixture("Europe/Stockholm"));

	QVERIFY(series.samples()[0].precip_rate.has_value());
	QVERIFY(std::fabs(*series.samples()[0].precip_rate - 1.0) < 1e-9);
	QCOMPARE(series.samples()[0].duration_s, 900);
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
