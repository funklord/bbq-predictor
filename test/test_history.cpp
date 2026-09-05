#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "store/history.h"

/*
 * The permanent store and the forecast queue that is not permanent
 * (project.md sec 12).
 *
 * Every test opens its own database in a temporary directory, so none of
 * them touches the real history and none of them depends on another
 * having run first. That matters more here than elsewhere: a store is
 * stateful by definition, and a suite that shared one would pass or fail
 * depending on the order it happened to run in.
 */
class test_history : public QObject {
	Q_OBJECT

private slots:
	void lead_times_bucket_by_their_upper_bound();
	void observations_survive_being_stored_twice();
	void a_forecast_is_kept_once_per_bucket_not_once_per_fetch();
	void verifying_computes_the_standard_scores_and_empties_the_queue();
	void bias_can_be_zero_while_the_forecast_is_useless();
	void an_unverifiable_forecast_is_given_up_on();
	void the_database_file_alone_carries_the_archive();
	void rain_is_still_scored_through_a_dry_spell();
	void a_chance_forecast_is_scored_by_occurrence_not_by_error();
	void a_brier_score_is_read_against_its_baseline();
	void a_stuck_sensor_does_not_score_a_forecast();
	void rediscovery_does_not_unpin_a_station();
	void a_station_s_own_cadence_still_pairs_with_the_hour();
	void the_corrected_band_scores_like_any_other();
	void the_verdict_is_scored_and_is_not_its_ingredients();
	void a_dry_spell_still_scores_the_verdict();

private:
	static bbq_series forecast_of(bbq_band band, qint64 start, int count,
	                              double temperature);
};

bbq_series test_history::forecast_of(bbq_band band, qint64 start, int count,
                                     double temperature) {
	std::vector<bbq_sample> samples;

	for (int i = 0; i < count; ++i) {
		bbq_sample sample;
		sample.start_utc = start + i * 3600;
		sample.duration_s = 3600;
		sample.temperature = temperature;
		samples.push_back(sample);
	}

	bbq_series series(band, QStringLiteral("test"));
	series.set_samples(std::move(samples));
	return series;
}

void test_history::lead_times_bucket_by_their_upper_bound() {
	QCOMPARE(bbq_lead_bucket_for(600), bbq_lead_bucket::hour);
	QCOMPARE(bbq_lead_bucket_for(3600), bbq_lead_bucket::hour);
	QCOMPARE(bbq_lead_bucket_for(3601), bbq_lead_bucket::three_hours);
	QCOMPARE(bbq_lead_bucket_for(24 * 3600), bbq_lead_bucket::day);
	QCOMPARE(bbq_lead_bucket_for(30 * 24 * 3600), bbq_lead_bucket::beyond);
}

void test_history::observations_survive_being_stored_twice() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY2(store.open(directory.filePath(QStringLiteral("h.sqlite"))),
	         qPrintable(store.last_error()));

	std::vector<bbq_sample> samples;
	for (int i = 0; i < 10; ++i) {
		bbq_sample sample;
		sample.start_utc = 1000000 + i * 300;
		sample.duration_s = 300;
		sample.temperature = 20.0 + i;
		samples.push_back(sample);
	}

	bbq_series observed(bbq_band::observed, QStringLiteral("wunderground"));
	observed.set_samples(samples);

	store.record_observations(QStringLiteral("ITEST1"), observed);
	QCOMPARE(store.observation_count(QStringLiteral("ITEST1")), 10);

	/*
	 * The overlap case, and it is the ordinary one rather than an edge:
	 * each refresh re-fetches the whole day, so almost every row stored
	 * has been stored before. Ten rows, not twenty.
	 */
	store.record_observations(QStringLiteral("ITEST1"), observed);
	QCOMPARE(store.observation_count(QStringLiteral("ITEST1")), 10);

	QCOMPARE(store.earliest_observation(QStringLiteral("ITEST1")),
	         static_cast<qint64>(1000000));

	const bbq_series back =
	        store.observations(QStringLiteral("ITEST1"), 1000000, 1000000 + 1500);
	QCOMPARE(static_cast<int>(back.samples().size()), 5);
	QVERIFY(back.samples().front().temperature.has_value());
	QCOMPARE(*back.samples().front().temperature, 20.0);
}

void test_history::a_forecast_is_kept_once_per_bucket_not_once_per_fetch() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	const qint64 issued = 1000000;
	const bbq_series forecast =
	        forecast_of(bbq_band::hourly, issued + 3600, 6, 15.0);

	const int first = store.record_forecast(QStringLiteral("ITEST1"), forecast,
	                                        issued);
	QVERIFY(first > 0);

	const int pending = store.pending_count(QStringLiteral("ITEST1"));

	/*
	 * Re-forecasting the same hours from the same moment must add
	 * nothing. Without the bound in sec 12.6 the queue would take a
	 * fresh copy of every valid time on every refresh, which is the
	 * unbounded growth keeping forecasts was supposed to avoid.
	 */
	store.record_forecast(QStringLiteral("ITEST1"), forecast, issued);
	QCOMPARE(store.pending_count(QStringLiteral("ITEST1")), pending);

	/*
	 * Issued much later, the same valid times are now a SHORTER lead --
	 * a different claim, and one the statistics must be able to tell
	 * apart. So this does add rows.
	 */
	store.record_forecast(QStringLiteral("ITEST1"), forecast, issued + 3 * 3600);
	QVERIFY2(store.pending_count(QStringLiteral("ITEST1")) > pending,
	         "a shorter-lead forecast of the same hours was discarded");
}

void test_history::verifying_computes_the_standard_scores_and_empties_the_queue() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	const qint64 issued = 1000000;
	const qint64 valid = issued + 3600;

	/* Forecast 15 C for four consecutive hours. */
	store.record_forecast(QStringLiteral("ITEST1"),
	                      forecast_of(bbq_band::hourly, valid, 4, 15.0), issued);
	QCOMPARE(store.pending_count(QStringLiteral("ITEST1")), 4);

	/* It was 17 C every time: the forecast ran two degrees cold. */
	std::vector<bbq_sample> observed_samples;
	for (int i = 0; i < 4; ++i) {
		bbq_sample sample;
		sample.start_utc = valid + i * 3600;
		sample.duration_s = 300;
		sample.temperature = 17.0;
		observed_samples.push_back(sample);
	}

	bbq_series observed(bbq_band::observed, QStringLiteral("wunderground"));
	observed.set_samples(observed_samples);
	store.record_observations(QStringLiteral("ITEST1"), observed);

	const int verified = store.verify(QStringLiteral("ITEST1"));
	QCOMPARE(verified, 4);

	/* Verified forecasts are discarded, which is the whole retention rule. */
	QCOMPARE(store.pending_count(QStringLiteral("ITEST1")), 0);

	/* The observations are untouched: they are the permanent half. */
	QCOMPARE(store.observation_count(QStringLiteral("ITEST1")), 4);

	const bbq_verification hour = store.verification(
	        QStringLiteral("ITEST1"), bbq_band::hourly,
	        QStringLiteral("temperature"), bbq_lead_bucket::hour);

	QCOMPARE(hour.count, 1);
	QCOMPARE(hour.bias, -2.0);
	QCOMPARE(hour.mean_absolute_error, 2.0);
	QCOMPARE(hour.root_mean_square_error, 2.0);

	/*
	 * Stratified, not pooled. The four hours fall in different buckets,
	 * so the one-hour bucket must not have collected all of them.
	 */
	const bbq_verification longer = store.verification(
	        QStringLiteral("ITEST1"), bbq_band::hourly,
	        QStringLiteral("temperature"), bbq_lead_bucket::three_hours);
	QVERIFY(longer.count > 0);
}

void test_history::bias_can_be_zero_while_the_forecast_is_useless() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	const qint64 issued = 1000000;
	const qint64 valid = issued + 1800;

	/*
	 * Sec 12.3's reason for keeping MAE and RMSE beside the bias. Two
	 * predictions at the same lead, one ten degrees high and one ten
	 * degrees low. Their mean error is zero -- a forecast that looks
	 * perfect by bias alone and is worthless.
	 */
	bbq_sample warm;
	warm.start_utc = valid;
	warm.duration_s = 3600;
	warm.temperature = 30.0;

	bbq_sample cold;
	cold.start_utc = valid + 600;
	cold.duration_s = 3600;
	cold.temperature = 10.0;

	bbq_series forecast(bbq_band::hourly, QStringLiteral("test"));
	forecast.set_samples({warm, cold});
	store.record_forecast(QStringLiteral("ITEST1"), forecast, issued);

	std::vector<bbq_sample> truth;
	for (int i = 0; i < 2; ++i) {
		bbq_sample sample;
		sample.start_utc = valid + i * 600;
		sample.duration_s = 300;
		sample.temperature = 20.0;
		truth.push_back(sample);
	}

	bbq_series observed(bbq_band::observed, QStringLiteral("wunderground"));
	observed.set_samples(truth);
	store.record_observations(QStringLiteral("ITEST1"), observed);

	QCOMPARE(store.verify(QStringLiteral("ITEST1")), 2);

	const bbq_verification score = store.verification(
	        QStringLiteral("ITEST1"), bbq_band::hourly,
	        QStringLiteral("temperature"), bbq_lead_bucket::hour);

	QCOMPARE(score.count, 2);
	QCOMPARE(score.bias, 0.0);
	QCOMPARE(score.mean_absolute_error, 10.0);
	QCOMPARE(score.root_mean_square_error, 10.0);
}

void test_history::an_unverifiable_forecast_is_given_up_on() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	const qint64 issued = 1000000;
	store.record_forecast(QStringLiteral("ITEST1"),
	                      forecast_of(bbq_band::hourly, issued + 3600, 4, 15.0),
	                      issued);
	QCOMPARE(store.pending_count(QStringLiteral("ITEST1")), 4);

	/* Still recent: nothing is given up on yet. */
	QCOMPARE(store.expire(QStringLiteral("ITEST1"), issued + 3600), 0);
	QCOMPARE(store.pending_count(QStringLiteral("ITEST1")), 4);

	/*
	 * Long past, and no observation ever arrived -- the station was down.
	 * Without this the queue would hold those rows for ever, and every
	 * outage would leak a few more (sec 12.6).
	 */
	const int dropped =
	        store.expire(QStringLiteral("ITEST1"), issued + 30 * 24 * 3600);
	QCOMPARE(dropped, 4);
	QCOMPARE(store.pending_count(QStringLiteral("ITEST1")), 0);
}

void test_history::a_chance_forecast_is_scored_by_occurrence_not_by_error() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	const qint64 issued = 1000000;
	const qint64 valid = issued + 1800;

	/* "70% chance", and it rained. */
	bbq_sample forecast_sample;
	forecast_sample.start_utc = valid;
	forecast_sample.duration_s = 3600;
	forecast_sample.precip_chance = 70.0;

	bbq_series forecast(bbq_band::hourly, QStringLiteral("test"));
	forecast.set_samples({forecast_sample});
	store.record_forecast(QStringLiteral("ITEST1"), forecast, issued);

	bbq_sample observed_sample;
	observed_sample.start_utc = valid;
	observed_sample.duration_s = 300;
	observed_sample.precip_rate = 2.5;

	bbq_series observed(bbq_band::observed, QStringLiteral("wunderground"));
	observed.set_samples({observed_sample});
	store.record_observations(QStringLiteral("ITEST1"), observed);

	QCOMPARE(store.verify(QStringLiteral("ITEST1")), 1);

	/*
	 * A percentage is not scored by subtracting it from what happened.
	 * "70%" is not 70 units wrong when it rains, so nothing should have
	 * landed in the mean-error table for it.
	 */
	const bbq_verification wrong_table = store.verification(
	        QStringLiteral("ITEST1"), bbq_band::hourly,
	        QStringLiteral("precip_chance"), bbq_lead_bucket::hour);
	QCOMPARE(wrong_table.count, 0);

	const bbq_brier score = store.brier(QStringLiteral("ITEST1"),
	                                    bbq_band::hourly, bbq_lead_bucket::hour);
	QCOMPARE(score.count, 1);

	/* It rained, so the outcome is 1 and the error is 0.3. */
	QVERIFY(qAbs(score.score - 0.09) < 0.0001);
	QCOMPARE(score.base_rate, 1.0);

	const std::vector<bbq_reliability_bin> bins = store.reliability(
	        QStringLiteral("ITEST1"), bbq_band::hourly, bbq_lead_bucket::hour);
	QCOMPARE(static_cast<int>(bins.size()), 1);
	QCOMPARE(bins.front().probability_bin, 7);
	QCOMPARE(bins.front().rain_count, 1);
}

void test_history::a_brier_score_is_read_against_its_baseline() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	/*
	 * A forecaster who always says 50% in a climate that rains half the
	 * time. The Brier score is 0.25, which sounds poor and is exactly
	 * what knowing nothing earns -- so the skill against the baseline is
	 * zero, and that is the number worth printing. A raw score means
	 * nothing without it: 0.1 is excellent in a dry climate and poor in
	 * a changeable one.
	 */
	store.set_reliability(QStringLiteral("ITEST1"), bbq_band::hourly,
	                      bbq_lead_bucket::day, 5, 100, 50, 25.0);

	const bbq_brier score = store.brier(QStringLiteral("ITEST1"),
	                                    bbq_band::hourly, bbq_lead_bucket::day);

	QCOMPARE(score.count, 100);
	QVERIFY(qAbs(score.score - 0.25) < 0.0001);
	QVERIFY(qAbs(score.base_rate - 0.5) < 0.0001);
	QVERIFY(qAbs(score.baseline - 0.25) < 0.0001);
	QVERIFY2(qAbs(score.skill()) < 0.0001,
	         "always saying 50% in a coin-flip climate scored as skill");

	/* And a forecaster who is actually right earns skill. */
	store.set_reliability(QStringLiteral("ITEST2"), bbq_band::hourly,
	                      bbq_lead_bucket::day, 0, 50, 0, 0.0);
	store.set_reliability(QStringLiteral("ITEST2"), bbq_band::hourly,
	                      bbq_lead_bucket::day, 10, 50, 50, 0.0);

	const bbq_brier perfect = store.brier(
	        QStringLiteral("ITEST2"), bbq_band::hourly, bbq_lead_bucket::day);
	QVERIFY(qAbs(perfect.score) < 0.0001);
	QVERIFY(qAbs(perfect.skill() - 1.0) < 0.0001);
}


void test_history::a_stuck_sensor_does_not_score_a_forecast() {
	/*
	 * Sec 12.14. The station this project was written against reported
	 * tempAvg 22 for 288 consecutive observations -- dew point 22 and
	 * humidity 99 beside it, a soaked probe -- while its wind moved
	 * normally. Scoring a forecast against that produces a bias of
	 * several degrees that is not a forecast error at all, and the
	 * corrected band is then drawn from it.
	 *
	 * The guard is per QUANTITY: the same station's rain is still worth
	 * scoring, and refusing the lot would throw away good measurements
	 * along with the bad one.
	 */
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY2(store.open(directory.filePath(QStringLiteral("h.sqlite"))),
	         qPrintable(store.last_error()));

	const qint64 start = 1786000000;
	const int count = 96; /* eight hours at five minutes */

	std::vector<bbq_sample> measured;
	for (int i = 0; i < count; ++i) {
		bbq_sample sample;
		sample.start_utc = start + i * 300;
		sample.duration_s = 300;
		sample.temperature = 22.0;          /* stuck */
		sample.precip_rate = (i % 4) * 0.1; /* moving */
		measured.push_back(sample);
	}

	bbq_series observed(bbq_band::observed, QStringLiteral("test"));
	observed.set_samples(std::move(measured));
	store.record_observations(QStringLiteral("ITEST1"), observed);

	std::vector<bbq_sample> predicted;
	for (int i = 0; i < count; ++i) {
		bbq_sample sample;
		sample.start_utc = start + i * 300;
		sample.duration_s = 300;
		sample.temperature = 15.0; /* seven degrees from the stuck value */
		sample.precip_rate = 0.0;
		predicted.push_back(sample);
	}

	bbq_series forecast(bbq_band::hourly, QStringLiteral("test"));
	forecast.set_samples(std::move(predicted));
	store.record_forecast(QStringLiteral("ITEST1"), forecast, start - 3600);

	QVERIFY(store.verify(QStringLiteral("ITEST1")) > 0);

	const bbq_verification temperature =
	        store.verification(QStringLiteral("ITEST1"), bbq_band::hourly,
	                           QStringLiteral("temperature"),
	                           bbq_lead_bucket::hour);
	const bbq_verification rain =
	        store.verification(QStringLiteral("ITEST1"), bbq_band::hourly,
	                           QStringLiteral("precip_rate"),
	                           bbq_lead_bucket::hour);

	QCOMPARE(temperature.count, 0);
	QVERIFY2(rain.count > 0, "the rain moved and should still be scored");
}


void test_history::rediscovery_does_not_unpin_a_station() {
	/*
	 * Sec 13. Discovery runs again every time the coordinate moves, so
	 * remembering has to be an upsert that leaves the pinned flag alone
	 * -- otherwise walking somewhere would silently unpin whatever the
	 * user had chosen to keep fetching, and the loss would show up days
	 * later as a station with no history.
	 */
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY2(store.open(directory.filePath(QStringLiteral("h.sqlite"))),
	         qPrintable(store.last_error()));

	bbq_station found;
	found.id = QStringLiteral("ISTOCK877");
	found.name = QStringLiteral("Vasastan");
	found.latitude = 59.34;
	found.longitude = 18.05;
	found.distance_km = 3.7;
	found.first_seen_utc = 1000;
	found.last_seen_utc = 1000;

	QVERIFY(store.remember_station(found));
	QVERIFY(store.set_station_pinned(found.id, true));
	QCOMPARE(static_cast<int>(store.pinned_stations().size()), 1);

	/* Found again from somewhere else: nearer, renamed, later. */
	found.name = QStringLiteral("Vasastan north");
	found.distance_km = 1.2;
	found.first_seen_utc = 9999;
	found.last_seen_utc = 2000;
	QVERIFY(store.remember_station(found));

	const std::vector<bbq_station> all = store.stations();
	QCOMPARE(static_cast<int>(all.size()), 1);
	QVERIFY2(all.at(0).pinned, "rediscovery unpinned it");
	QCOMPARE(all.at(0).name, QStringLiteral("Vasastan north"));
	QCOMPARE(all.at(0).distance_km, 1.2);
	QCOMPARE(all.at(0).last_seen_utc, qint64(2000));

	/* First seen is when we first heard of it, not when we last did. */
	QCOMPARE(all.at(0).first_seen_utc, qint64(1000));
}

void test_history::a_station_s_own_cadence_still_pairs_with_the_hour() {
	/*
	 * REAL TIMESTAMPS, which never line up (project.md sec 12.16).
	 *
	 * Every other test here places observations at exactly the instants
	 * the forecasts are valid for, which is a fixture agreeing with the
	 * code by construction: it proves the arithmetic and says nothing
	 * about whether real data pairs at all. A station reports on its
	 * own cadence -- ISTOCK877's is 299 seconds -- and forecasts fall
	 * on hour boundaries, so the two are never equal and the whole
	 * feature rests on the match window being wide enough.
	 *
	 * Measured against that station's real archive before this was
	 * written: all 45 hour boundaries in two days of observations have
	 * one within 150 seconds, worst case 63.
	 */
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	const qint64 hour = 1600000000 / 3600 * 3600;

	store.record_forecast(QStringLiteral("ITEST1"),
	                      forecast_of(bbq_band::hourly, hour, 4, 15.0), hour - 3600);
	QCOMPARE(store.pending_count(QStringLiteral("ITEST1")), 4);

	/*
	 * The station's own clock: every 299 seconds, from an offset that
	 * is not a factor of an hour, so nothing can land on one by
	 * accident.
	 */
	std::vector<bbq_sample> measured;
	for (int i = 0; i < 60; ++i) {
		bbq_sample sample;
		sample.start_utc = hour - 1800 + 37 + i * 299;
		sample.duration_s = 299;
		sample.temperature = 17.0;
		measured.push_back(sample);
	}

	/*
	 * The fixture is only worth anything if it really does disagree
	 * with the forecast times, so that is asserted rather than assumed
	 * -- otherwise a later edit could quietly turn this back into the
	 * aligned test it exists to complement.
	 */
	for (const bbq_sample &sample : measured) {
		for (int i = 0; i < 4; ++i) {
			QVERIFY2(sample.start_utc != hour + i * 3600,
			         "an observation landed exactly on a forecast time, so "
			         "this fixture is no longer asking its question");
		}
	}

	bbq_series observed(bbq_band::observed, QStringLiteral("wunderground"));
	observed.set_samples(measured);
	store.record_observations(QStringLiteral("ITEST1"), observed);

	QCOMPARE(store.verify(QStringLiteral("ITEST1")), 4);
	QCOMPARE(store.pending_count(QStringLiteral("ITEST1")), 0);
}

void test_history::the_corrected_band_scores_like_any_other() {
	/*
	 * QUEUED IS NOT SCORED (project.md sec 12.19).
	 *
	 * test_feed proves the correction reaches the queue. That is half
	 * the claim: a band can sit in forecast_pending for ever if
	 * anything downstream declines it -- a lead bucket computed
	 * differently, a quantity name that does not match, the stuck
	 * sensor guard. The answer would otherwise arrive in three days, as
	 * an absence, which is the hardest kind of result to notice.
	 */
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	const qint64 issued = 1600000000;
	const qint64 valid = issued + 3600;

	store.record_forecast(QStringLiteral("ITEST1"),
	                      forecast_of(bbq_band::corrected, valid, 4, 15.0),
	                      issued);
	QCOMPARE(store.pending_count(QStringLiteral("ITEST1"),
	                             bbq_band::corrected), 4);

	/* It was 17 every hour: the correction ran two degrees cold. */
	std::vector<bbq_sample> measured;
	for (int i = 0; i < 4; ++i) {
		bbq_sample sample;
		sample.start_utc = valid + i * 3600;
		sample.duration_s = 300;
		sample.temperature = 17.0;
		measured.push_back(sample);
	}

	bbq_series observed(bbq_band::observed, QStringLiteral("wunderground"));
	observed.set_samples(measured);
	store.record_observations(QStringLiteral("ITEST1"), observed);

	QCOMPARE(store.verify(QStringLiteral("ITEST1")), 4);
	QCOMPARE(store.pending_count(QStringLiteral("ITEST1"),
	                             bbq_band::corrected), 0);

	const bbq_verification scored = store.verification(
	        QStringLiteral("ITEST1"), bbq_band::corrected,
	        QStringLiteral("temperature"), bbq_lead_bucket::hour);

	QVERIFY2(scored.count > 0, "the corrected band was queued and never scored");
	QCOMPARE(scored.bias, -2.0);
}

void test_history::the_verdict_is_scored_and_is_not_its_ingredients() {
	/*
	 * THE RECOMMENDATION, not the fields behind it (sec 12.20).
	 *
	 * The fixture is chosen so the wrong answer differs from the right
	 * one: the forecast is nearly right on every ingredient and badly
	 * wrong on the verdict: a tenth of a degree out on temperature,
	 * exactly right on wind, and rain it called dry. The rain ramp puts
	 * the verdict almost on the floor while the temperature record
	 * still reads as very nearly correct, which is the whole reason
	 * this quantity exists. A test where the
	 * verdict error merely tracked the temperature error would pass
	 * against code that never computed a verdict at all.
	 */
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	const qint64 issued = 1600000000;
	const qint64 valid = issued + 3600;

	std::vector<bbq_sample> predicted;
	for (int i = 0; i < 4; ++i) {
		bbq_sample sample;
		sample.start_utc = valid + i * 3600;
		sample.duration_s = 3600;
		sample.temperature = 20.0;
		sample.precip_rate = 0.0;   /* dry: the ramp is at full marks */
		sample.wind_kph = 5.0;
		predicted.push_back(sample);
	}

	bbq_series forecast(bbq_band::hourly, QStringLiteral("test"));
	forecast.set_samples(std::move(predicted));
	store.record_forecast(QStringLiteral("ITEST1"), forecast, issued);

	std::vector<bbq_sample> measured;
	for (int i = 0; i < 4; ++i) {
		bbq_sample sample;
		sample.start_utc = valid + i * 3600;
		sample.duration_s = 300;
		sample.temperature = 20.1;  /* a tenth of a degree out */
		sample.precip_rate = 1.8;   /* a downpour the forecast missed */
		sample.wind_kph = 5.0;
		measured.push_back(sample);
	}

	bbq_series observed(bbq_band::observed, QStringLiteral("wunderground"));
	observed.set_samples(measured);
	store.record_observations(QStringLiteral("ITEST1"), observed);

	QCOMPARE(store.verify(QStringLiteral("ITEST1")), 4);

	const bbq_verification temperature = store.verification(
	        QStringLiteral("ITEST1"), bbq_band::hourly,
	        QStringLiteral("temperature"), bbq_lead_bucket::hour);
	const bbq_verification verdict = store.verification(
	        QStringLiteral("ITEST1"), bbq_band::hourly,
	        QStringLiteral("grill"), bbq_lead_bucket::hour);

	QVERIFY2(verdict.count > 0, "the verdict was never scored");

	/*
	 * Nearly right on the ingredient, badly wrong on the answer. That
	 * gap is the finding sec 12.20 records, asserted rather than
	 * described.
	 */
	QVERIFY2(std::fabs(temperature.mean_absolute_error) < 0.2,
	         "the fixture is not nearly right on temperature");
	QVERIFY2(verdict.mean_absolute_error > 0.5,
	         qPrintable(QStringLiteral("the verdict error is only %1, so this "
	                                   "fixture cannot tell a verdict from "
	                                   "its ingredients")
	                            .arg(verdict.mean_absolute_error)));
}

void test_history::a_dry_spell_still_scores_the_verdict() {
	/*
	 * THE COMMON CASE (project.md sec 12.20.1).
	 *
	 * The stuck-sensor guard fires when a quantity never moves across
	 * enough samples and enough hours. For rain that is the ordinary
	 * state of good weather -- measured on the device, 68 observations
	 * across 23 hours all read 0.0 mm/h -- so vetoing the verdict on it
	 * made the quantity inert in exactly the weather somebody would
	 * light a fire in.
	 *
	 * Thirty hourly pairs, rain flat at zero, temperature and wind
	 * moving so that neither of those trips the guard instead.
	 */
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	const qint64 issued = 1600000000;
	const qint64 valid = issued + 3600;

	std::vector<bbq_sample> predicted;
	std::vector<bbq_sample> measured;

	for (int i = 0; i < 30; ++i) {
		bbq_sample forecast;
		forecast.start_utc = valid + i * 3600;
		forecast.duration_s = 3600;
		forecast.temperature = 18.0 + (i % 5);
		forecast.precip_rate = 0.0;
		forecast.wind_kph = 8.0 + (i % 3);
		predicted.push_back(forecast);

		bbq_sample seen;
		seen.start_utc = valid + i * 3600;
		seen.duration_s = 300;
		seen.temperature = 20.0 + (i % 5);
		seen.precip_rate = 0.0;   /* dry all day, which is not a fault */
		seen.wind_kph = 9.0 + (i % 3);
		measured.push_back(seen);
	}

	bbq_series forecast(bbq_band::hourly, QStringLiteral("test"));
	forecast.set_samples(std::move(predicted));
	store.record_forecast(QStringLiteral("ITEST1"), forecast, issued);

	bbq_series observed(bbq_band::observed, QStringLiteral("wunderground"));
	observed.set_samples(measured);
	store.record_observations(QStringLiteral("ITEST1"), observed);

	QCOMPARE(store.verify(QStringLiteral("ITEST1")), 30);

	/*
	 * Rain itself is still not scored -- the guard's own judgement on
	 * the quantity is left alone, because changing it would move every
	 * statistic already in the archive. What must survive is the
	 * VERDICT.
	 */
	int verdict_rows = 0;
	const bbq_lead_bucket every[] = {
		bbq_lead_bucket::hour, bbq_lead_bucket::three_hours,
		bbq_lead_bucket::six_hours, bbq_lead_bucket::twelve_hours,
		bbq_lead_bucket::day};

	for (bbq_lead_bucket bucket : every) {
		const bbq_verification scored = store.verification(
		        QStringLiteral("ITEST1"), bbq_band::hourly,
		        QStringLiteral("grill"), bucket);
		verdict_rows += scored.count;
	}

	QVERIFY2(verdict_rows > 0,
	         "a dry day left the verdict unscored, which is the weather it "
	         "matters most in");
}

void test_history::the_database_file_alone_carries_the_archive() {
	/*
	 * THE HAZARD, NOT THE MECHANISM (project.md sec 16.8).
	 *
	 * Under WAL a committed row can live entirely in the -wal file with
	 * the database holding almost nothing: measured on a phone at 4096
	 * bytes of database against 3.9 MB of log. SQLite reads the pair as
	 * one and nothing is at risk while both stay together.
	 *
	 * What breaks is everything that copies ONE file and believes it has
	 * the archive -- a backup, a file manager, a pull off a device. The
	 * copy is silently EMPTY rather than obviously broken, which is the
	 * worst way for it to fail and the reason this asserts on a copy
	 * rather than on a pragma having been issued.
	 *
	 * Copying the database without its log is precisely the mistake
	 * being guarded against, so the test performs it deliberately.
	 */
	QTemporaryDir scratch;
	QVERIFY(scratch.isValid());

	const QString live = scratch.filePath(QStringLiteral("live.sqlite"));

	std::vector<bbq_sample> samples;
	for (int i = 0; i < 200; ++i) {
		bbq_sample sample;
		sample.start_utc = 1756000000 + i * 300;
		sample.duration_s = 300;
		sample.temperature = 15.0 + i;
		samples.push_back(sample);
	}

	bbq_series series(bbq_band::observed, QStringLiteral("test"));
	series.set_samples(samples);

	{
		bbq_history store;
		QVERIFY(store.open(live));
		QCOMPARE(store.record_observations(QStringLiteral("ITEST1"), series),
		         200);

		QVERIFY2(store.checkpoint(), "the checkpoint was refused");

		/* Copied while the store is still OPEN, as a backup would be. */
		QVERIFY(QFile::copy(live, scratch.filePath(QStringLiteral("copy.sqlite"))));
	}

	/*
	 * The copy is opened with no -wal beside it. Without the checkpoint
	 * this finds an empty archive and reports it as one, which is
	 * exactly how the real mistake presents.
	 */
	bbq_history copied;
	QVERIFY(copied.open(scratch.filePath(QStringLiteral("copy.sqlite"))));
	QCOMPARE(copied.observation_count(QStringLiteral("ITEST1")), 200);
}

void test_history::rain_is_still_scored_through_a_dry_spell() {
	/*
	 * A DRY SPELL IS NOT A DEAD GAUGE, AND THE QUANTITY NEEDS THAT TOO
	 * (project.md sec 12.20.1).
	 *
	 * The stuck guard refuses to score a quantity whose observations
	 * never move, which for temperature or wind is a broken sensor. For
	 * rain a flat zero is the ordinary state of good weather. The
	 * verdict was given an exception for it; scoring the rain quantity
	 * itself was not, so rain went unscored on every dry day.
	 *
	 * That held the whole record line at "none yet", since it reports
	 * four quantities together -- and it threw away the informative
	 * case, which is a forecast that promised rain on a day that stayed
	 * dry. That error is only visible against observations that are all
	 * zero.
	 *
	 * Enough samples across enough hours to trip the guard: the point
	 * is that it trips and rain is scored anyway.
	 */
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	const qint64 issued = 1000000;
	const qint64 first = issued + 1800;

	/* A forecast that keeps promising rain. */
	std::vector<bbq_sample> promised;
	for (int i = 0; i < 30; ++i) {
		bbq_sample sample;
		sample.start_utc = first + i * 3600;
		sample.duration_s = 3600;
		sample.temperature = 15.0 + i;
		sample.precip_rate = 2.0;
		promised.push_back(sample);
	}

	bbq_series forecast(bbq_band::hourly, QStringLiteral("test"));
	forecast.set_samples(promised);
	store.record_forecast(QStringLiteral("ITEST1"), forecast, issued);

	/* And a day that stayed dry: rain flat at zero throughout. */
	std::vector<bbq_sample> truth;
	for (int i = 0; i < 30; ++i) {
		bbq_sample sample;
		sample.start_utc = first + i * 3600;
		sample.duration_s = 300;
		sample.temperature = 15.0 + i;
		sample.precip_rate = 0.0;
		truth.push_back(sample);
	}

	bbq_series observed(bbq_band::observed, QStringLiteral("wunderground"));
	observed.set_samples(truth);
	store.record_observations(QStringLiteral("ITEST1"), observed);

	QVERIFY(store.verify(QStringLiteral("ITEST1")) > 0);

	const bbq_verification rain = store.verification(
	        QStringLiteral("ITEST1"), bbq_band::hourly,
	        QStringLiteral("precip_rate"), bbq_lead_bucket::hour);

	QVERIFY2(rain.count > 0,
	         "rain went unscored because the sky was dry");

	/*
	 * And the score says the right thing: promised 2 mm/h against a dry
	 * day is an over-forecast of exactly that, which is the error the
	 * refusal was discarding.
	 */
	QCOMPARE(rain.mean_absolute_error, 2.0);

	/*
	 * Temperature moved, so it is scored either way -- present here to
	 * show the fixture is not simply scoring everything.
	 */
	const bbq_verification warmth = store.verification(
	        QStringLiteral("ITEST1"), bbq_band::hourly,
	        QStringLiteral("temperature"), bbq_lead_bucket::hour);
	QVERIFY(warmth.count > 0);
}

QTEST_GUILESS_MAIN(test_history)
#include "test_history.moc"
