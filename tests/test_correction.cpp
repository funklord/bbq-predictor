#include <QTemporaryDir>
#include <QTest>

#include "model/correction.h"

/*
 * The bias-corrected overlay (project.md sec 12.5).
 *
 * Everything here uses a seeded store in a temporary directory, which is
 * the only way to exercise a correction before a month of real weather
 * has gone by. The arithmetic is the part worth pinning: a correction
 * that is silently wrong looks exactly like one that is right.
 */
class test_correction : public QObject {
	Q_OBJECT

private slots:
	void nothing_verified_means_nothing_drawn();
	void too_few_comparisons_are_not_a_bias();
	void a_known_bias_is_subtracted();
	void the_bias_is_interpolated_across_lead_time();
	void measurements_are_never_corrected();

private:
	static bbq_composite forecast_at(qint64 start, int hours, double temperature);
};

bbq_composite test_correction::forecast_at(qint64 start, int hours,
                                           double temperature) {
	std::vector<bbq_sample> samples;

	for (int i = 0; i < hours; ++i) {
		bbq_sample sample;
		sample.start_utc = start + i * 3600;
		sample.duration_s = 3600;
		sample.temperature = temperature;
		samples.push_back(sample);
	}

	bbq_series series(bbq_band::hourly, QStringLiteral("wunderground"));
	series.set_samples(std::move(samples));

	bbq_composite composite;
	composite.set_series(std::move(series));
	return composite;
}

void test_correction::nothing_verified_means_nothing_drawn() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	const qint64 now = 1000000;
	const bbq_composite composite = forecast_at(now, 12, 20.0);

	const bbq_series corrected = bbq_corrected_forecast(
	        composite, store, QStringLiteral("ITEST1"), now, now + 12 * 3600, now);

	QVERIFY2(corrected.is_empty(),
	         "a fresh install drew a corrected band with nothing behind it");
}

void test_correction::too_few_comparisons_are_not_a_bias() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	/*
	 * Three comparisons say more about last Tuesday than about the
	 * provider. Below the minimum the band is absent, which is the
	 * honest way to say "not known yet" rather than moving a curve by
	 * an amount built from noise.
	 */
	store.set_verification(QStringLiteral("ITEST1"), bbq_band::hourly,
	                       QStringLiteral("temperature"), bbq_lead_bucket::hour,
	                       3, 2.0, 2.0, 2.0);

	const qint64 now = 1000000;
	const bbq_composite composite = forecast_at(now, 12, 20.0);

	const bbq_series corrected = bbq_corrected_forecast(
	        composite, store, QStringLiteral("ITEST1"), now, now + 12 * 3600, now);

	QVERIFY(corrected.is_empty());
}

void test_correction::a_known_bias_is_subtracted() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	/*
	 * The same bias in every bucket, so interpolation between them
	 * cannot change the answer and the sign is the only thing under
	 * test. Running two degrees warm means the corrected curve sits two
	 * degrees lower.
	 */
	const bbq_lead_bucket every[] = {
		bbq_lead_bucket::hour, bbq_lead_bucket::three_hours,
		bbq_lead_bucket::six_hours, bbq_lead_bucket::twelve_hours,
		bbq_lead_bucket::day};

	for (bbq_lead_bucket bucket : every) {
		store.set_verification(QStringLiteral("ITEST1"), bbq_band::hourly,
		                       QStringLiteral("temperature"), bucket, 50, 2.0,
		                       2.0, 2.0);
	}

	const qint64 now = 1000000;
	const bbq_composite composite = forecast_at(now, 12, 20.0);

	const bbq_series corrected = bbq_corrected_forecast(
	        composite, store, QStringLiteral("ITEST1"), now, now + 12 * 3600, now);

	QVERIFY(!corrected.is_empty());

	for (const bbq_sample &sample : corrected.samples()) {
		QVERIFY(sample.temperature.has_value());
		QCOMPARE(*sample.temperature, 18.0);
	}

	/*
	 * One point per forecast sample, not one per probe. The first
	 * version emitted every quarter hour and drew a staircase, because
	 * an hourly sample holds one value across its whole span (sec 12.9).
	 */
	QCOMPARE(static_cast<int>(corrected.samples().size()), 12);
}

void test_correction::the_bias_is_interpolated_across_lead_time() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	/*
	 * Zero at the one-hour bucket and four degrees at the twelve-hour
	 * one. A stepped correction would give every sample in between one
	 * of those two values; an interpolated one has to pass through
	 * something else.
	 */
	store.set_verification(QStringLiteral("ITEST1"), bbq_band::hourly,
	                       QStringLiteral("temperature"), bbq_lead_bucket::hour,
	                       50, 0.0, 1.0, 1.0);
	store.set_verification(QStringLiteral("ITEST1"), bbq_band::hourly,
	                       QStringLiteral("temperature"),
	                       bbq_lead_bucket::twelve_hours, 50, 4.0, 4.0, 4.0);

	const qint64 now = 1000000;
	const bbq_composite composite = forecast_at(now, 12, 20.0);

	const bbq_series corrected = bbq_corrected_forecast(
	        composite, store, QStringLiteral("ITEST1"), now, now + 12 * 3600, now);

	QVERIFY(!corrected.is_empty());

	bool found_between = false;
	for (const bbq_sample &sample : corrected.samples()) {
		const double value = *sample.temperature;

		/* Between the two endpoints, and equal to neither. */
		if (value < 19.99 && value > 16.01) {
			found_between = true;
		}

		QVERIFY(value >= 16.0 - 0.001);
		QVERIFY(value <= 20.0 + 0.001);
	}

	QVERIFY2(found_between,
	         "the bias steps between buckets instead of interpolating, which "
	         "is what drew the corrected curve as a staircase");
}

void test_correction::measurements_are_never_corrected() {
	QTemporaryDir directory;
	bbq_history store;
	QVERIFY(store.open(directory.filePath(QStringLiteral("h.sqlite"))));

	store.set_verification(QStringLiteral("ITEST1"), bbq_band::observed,
	                       QStringLiteral("temperature"), bbq_lead_bucket::hour,
	                       50, 2.0, 2.0, 2.0);

	const qint64 now = 1000000;

	std::vector<bbq_sample> samples;
	for (int i = 0; i < 12; ++i) {
		bbq_sample sample;
		sample.start_utc = now + i * 300;
		sample.duration_s = 300;
		sample.temperature = 20.0;
		samples.push_back(sample);
	}

	bbq_series observed(bbq_band::observed, QStringLiteral("wunderground"));
	observed.set_samples(std::move(samples));

	bbq_composite composite;
	composite.set_series(std::move(observed));

	const bbq_series corrected = bbq_corrected_forecast(
	        composite, store, QStringLiteral("ITEST1"), now, now + 3600, now);

	/*
	 * An observation has no lead time and nothing to be wrong about.
	 * "Correcting" one would be adjusting the very thing every forecast
	 * is scored against.
	 */
	QVERIFY2(corrected.is_empty(), "a measured band was corrected");
}

QTEST_GUILESS_MAIN(test_correction)
#include "test_correction.moc"
