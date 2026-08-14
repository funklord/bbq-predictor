#include <QDir>
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
	void a_chance_forecast_is_scored_by_occurrence_not_by_error();
	void a_brier_score_is_read_against_its_baseline();
	void a_stuck_sensor_does_not_score_a_forecast();
	void rediscovery_does_not_unpin_a_station();

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

QTEST_GUILESS_MAIN(test_history)
#include "test_history.moc"
