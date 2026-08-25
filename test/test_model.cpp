#include <QTest>

#include "model/composite.h"
#include "model/grill.h"
#include "model/series.h"

/*
 * The model's own invariants (project.md sec 3.3 to 3.6, sec 7).
 *
 * Weighted towards the traps that were measured rather than guessed:
 * the endpoint that answers newest-first, the coverage that must not be
 * interpolated across, and the precedence that must stay declared.
 */
class test_model : public QObject {
	Q_OBJECT

private slots:
	void samples_are_sorted_on_the_way_in();
	void a_span_that_ended_does_not_cover_now();
	void range_takes_overlaps_not_containment();
	void a_gap_is_larger_than_the_step();
	void measured_beats_forecast();
	void an_absent_band_is_named();
	void a_band_never_fetched_poisons_the_freshness();
	void rain_outweighs_warmth();
	void a_short_window_is_not_offered();
	void range_agrees_with_the_scan_it_replaced();
	void radar_does_not_hide_the_cold();
	void radar_still_sharpens_the_rain();

private:
	static bbq_series band_of(bbq_band band, qint64 start, int step_s,
	                          int count, double temperature);
};

bbq_series test_model::band_of(bbq_band band, qint64 start, int step_s,
                               int count, double temperature) {
	bbq_series series(band, QStringLiteral("test"));

	std::vector<bbq_sample> samples;
	for (int i = 0; i < count; ++i) {
		bbq_sample sample;
		sample.start_utc = start + static_cast<qint64>(i) * step_s;
		sample.duration_s = step_s;
		sample.temperature = temperature;
		sample.precip_rate = 0.0;
		sample.wind_kph = 0.0;
		samples.push_back(sample);
	}

	series.set_samples(std::move(samples));
	series.set_fetched_utc(start);
	return series;
}

void test_model::samples_are_sorted_on_the_way_in() {
	/*
	 * Sec 2.6.2: one WU endpoint answers newest-first, and plotting
	 * that unreversed draws a mirror image of the last day that still
	 * looks like weather.
	 */
	std::vector<bbq_sample> descending;
	for (int i = 5; i >= 0; --i) {
		bbq_sample sample;
		sample.start_utc = 1000 + i * 100;
		sample.duration_s = 100;
		descending.push_back(sample);
	}

	bbq_series series(bbq_band::observed, QStringLiteral("test"));
	series.set_samples(std::move(descending));

	for (std::size_t i = 1; i < series.size(); ++i) {
		QVERIFY(series.samples()[i].start_utc > series.samples()[i - 1].start_utc);
	}
}

void test_model::a_span_that_ended_does_not_cover_now() {
	/*
	 * Starting before an instant is not covering it. Returning the
	 * previous sample would be interpolation across a gap by another
	 * name (sec 3.6).
	 */
	bbq_series series(bbq_band::observed, QStringLiteral("test"));

	std::vector<bbq_sample> samples;
	bbq_sample early;
	early.start_utc = 1000;
	early.duration_s = 100;
	samples.push_back(early);

	bbq_sample late;
	late.start_utc = 5000;
	late.duration_s = 100;
	samples.push_back(late);

	series.set_samples(std::move(samples));

	QVERIFY(series.at(1050) != nullptr);
	QVERIFY(series.at(3000) == nullptr);
	QVERIFY(series.at(5050) != nullptr);
}

void test_model::range_takes_overlaps_not_containment() {
	/*
	 * The distinction that made the temperature staircase survive a
	 * first fix (sec 3.8.1): an hourly sample overlaps every column
	 * inside it.
	 */
	const bbq_series series = band_of(bbq_band::hourly, 0, 3600, 4, 20.0);

	const std::pair<std::size_t, std::size_t> inside = series.range(600, 700);
	QCOMPARE(inside.second - inside.first, std::size_t(1));

	const std::pair<std::size_t, std::size_t> across = series.range(3500, 7300);
	QCOMPARE(across.second - across.first, std::size_t(3));
}

void test_model::a_gap_is_larger_than_the_step() {
	bbq_series series(bbq_band::observed, QStringLiteral("test"));

	std::vector<bbq_sample> samples;
	const qint64 starts[] = {0, 300, 600, 3000, 3300};
	for (qint64 start : starts) {
		bbq_sample sample;
		sample.start_utc = start;
		sample.duration_s = 300;
		samples.push_back(sample);
	}
	series.set_samples(std::move(samples));

	QCOMPARE(series.nominal_step_s(), 300);
	QVERIFY(!series.has_gap_after(0));
	QVERIFY(series.has_gap_after(2));
	QVERIFY(!series.has_gap_after(4));
}

void test_model::measured_beats_forecast() {
	/*
	 * Sec 3.3, and the reason the table must not become a computation:
	 * the hourly band is coarser AND lower priority, so a test that
	 * only checked resolution would pass for the wrong reason. Both
	 * bands here step identically, so only the declared order can
	 * decide.
	 */
	bbq_composite composite;
	composite.set_series(band_of(bbq_band::hourly, 0, 3600, 5, 10.0));
	composite.set_series(band_of(bbq_band::observed, 0, 3600, 5, 20.0));

	const bbq_reading reading = composite.at(1800);
	QVERIFY(reading.is_valid());
	QCOMPARE(reading.series->band(), bbq_band::observed);
	QCOMPARE(*reading.sample->temperature, 20.0);
}

void test_model::an_absent_band_is_named() {
	/*
	 * Sec 2.6.6: a flattened array could only produce a hole, and a
	 * hole meaning "not configured" is drawn like one meaning "it was
	 * not raining".
	 */
	bbq_composite composite;
	composite.set_series(band_of(bbq_band::hourly, 0, 3600, 3, 15.0));

	const std::vector<bbq_band> missing = composite.missing_bands();
	QVERIFY(std::find(missing.begin(), missing.end(), bbq_band::observed) !=
	        missing.end());
	QVERIFY(std::find(missing.begin(), missing.end(), bbq_band::hourly) ==
	        missing.end());
}

void test_model::a_band_never_fetched_poisons_the_freshness() {
	/* Sec 2.4: the oldest, and never-fetched is older than anything. */
	bbq_composite composite;
	composite.set_series(band_of(bbq_band::hourly, 0, 3600, 3, 15.0));

	bbq_series stale = band_of(bbq_band::observed, 0, 3600, 3, 15.0);
	stale.set_fetched_utc(0);
	composite.set_series(std::move(stale));

	QCOMPARE(composite.oldest_fetch_utc(), qint64(0));
}

void test_model::rain_outweighs_warmth() {
	/*
	 * Sec 7.2: the factors multiply, so a warm downpour must lose to a
	 * cool dry hour. An averaging score would recommend grilling in the
	 * rain because it was warm, and this is the test that says so.
	 */
	const QTimeZone utc(0);
	const bbq_grill_policy policy;

	/* 18:00 UTC on an arbitrary day, inside prime hours either way. */
	const qint64 when = 1786125600;

	bbq_series warm_wet(bbq_band::hourly, QStringLiteral("test"));
	std::vector<bbq_sample> wet;
	bbq_sample a;
	a.start_utc = when - 3600;
	a.duration_s = 7200;
	a.temperature = 30.0;
	a.precip_rate = 3.0;
	a.wind_kph = 0.0;
	wet.push_back(a);
	warm_wet.set_samples(std::move(wet));

	bbq_composite rainy;
	rainy.set_series(std::move(warm_wet));

	bbq_series cool_dry(bbq_band::hourly, QStringLiteral("test"));
	std::vector<bbq_sample> dry;
	bbq_sample b;
	b.start_utc = when - 3600;
	b.duration_s = 7200;
	b.temperature = 16.0;
	b.precip_rate = 0.0;
	b.wind_kph = 0.0;
	dry.push_back(b);
	cool_dry.set_samples(std::move(dry));

	bbq_composite fine;
	fine.set_series(std::move(cool_dry));

	const double wet_score = bbq_grill_score(rainy, utc, when, policy);
	const double dry_score = bbq_grill_score(fine, utc, when, policy);

	QVERIFY2(dry_score > wet_score,
	         "a warm downpour outscored a cool dry hour, so the factors are "
	         "no longer multiplying");
}

void test_model::a_short_window_is_not_offered() {
	/* Sec 7.1: two hours is a floor, not a preference. */
	const QTimeZone utc(0);
	const bbq_grill_policy policy;
	const qint64 when = 1786125600;

	bbq_composite composite;
	composite.set_series(band_of(bbq_band::hourly, when, 1800, 2, 30.0));

	const std::vector<bbq_window> windows =
	        bbq_grill_windows(composite, utc, when, when + 3600, policy);

	QVERIFY2(windows.empty(),
	         "a one-hour stretch was offered as a grilling window");
}

void test_model::range_agrees_with_the_scan_it_replaced() {
	/*
	 * The proof for a mechanical change (project.md sec 13.1).
	 *
	 * range() used to scan from index zero and now seeks with a binary
	 * search. The claim is that it returns exactly what the scan
	 * returned, so the check is not a handful of chosen cases but the
	 * old implementation, run beside the new one over every window in a
	 * sweep.
	 *
	 * The fixture deliberately mixes durations. The seek is only exact
	 * because it starts a full m_max_duration_s early, so a series whose
	 * spans are all the same length would not exercise the reason the
	 * subtraction is there.
	 */
	std::vector<bbq_sample> samples;
	const qint64 base = 1000000;

	for (int i = 0; i < 40; ++i) {
		bbq_sample sample;
		sample.start_utc = base + i * 300;

		/* Mostly five minutes, occasionally a two-hour straddler. */
		sample.duration_s = (i % 7 == 3) ? 7200 : 300;
		sample.temperature = 10.0 + i;
		samples.push_back(sample);
	}

	bbq_series series(bbq_band::observed, QStringLiteral("test"));
	series.set_samples(samples);

	const std::vector<bbq_sample> &sorted = series.samples();

	for (qint64 from = base - 9000; from < base + 15000; from += 137) {
		for (qint64 width : {1, 60, 300, 3600, 20000}) {
			const qint64 to = from + width;

			/* The implementation that was replaced, verbatim. */
			std::size_t want_first = 0;
			while (want_first < sorted.size() &&
			       sorted[want_first].end_utc() <= from) {
				++want_first;
			}

			std::size_t want_last = want_first;
			while (want_last < sorted.size() &&
			       sorted[want_last].start_utc < to) {
				++want_last;
			}

			const std::pair<std::size_t, std::size_t> got =
			        series.range(from, to);

			QCOMPARE(got.first, want_first);
			QCOMPARE(got.second, want_last);
		}
	}
}

void test_model::radar_does_not_hide_the_cold() {
	/*
	 * Sec 3.18, in the place it costs the most.
	 *
	 * MET Norway's radar nowcast carries precipitation_rate and nothing
	 * else on twenty-two of its twenty-three steps, and it outranks
	 * every forecast band -- so for the next two hours it won the
	 * instant outright. The scorer treats an absent temperature as
	 * neutral, which is right when nothing knows it and wrong here,
	 * where the hourly band knows it and was merely outranked. A
	 * freezing dry evening therefore scored as though it were warm, in
	 * exactly the window somebody is deciding whether to light a fire.
	 */
	const QTimeZone utc(0);
	const bbq_grill_policy policy;
	const qint64 when = 1786125600;

	bbq_series hourly(bbq_band::hourly, QStringLiteral("test"));
	std::vector<bbq_sample> warm_enough;
	bbq_sample h;
	h.start_utc = when - 3600;
	h.duration_s = 7200;
	h.temperature = 1.0; /* nobody grills in this */
	h.precip_rate = 0.0;
	h.wind_kph = 0.0;
	warm_enough.push_back(h);
	hourly.set_samples(std::move(warm_enough));

	/* Rain only, as the real band is after its first step. */
	bbq_series radar(bbq_band::nowcast_fine, QStringLiteral("test"));
	std::vector<bbq_sample> fine;
	bbq_sample r;
	r.start_utc = when - 300;
	r.duration_s = 600;
	r.precip_rate = 0.0;
	fine.push_back(r);
	radar.set_samples(std::move(fine));

	bbq_composite composite;
	composite.set_series(std::move(hourly));
	composite.set_series(std::move(radar));

	/*
	 * The cold has to reach the score. Without the fix the radar sample
	 * owns the instant, carries no temperature, and the score comes out
	 * as though the evening were fine.
	 */
	const double cold = bbq_grill_score(composite, utc, when, policy);

	bbq_composite alone;
	bbq_series only_hourly(bbq_band::hourly, QStringLiteral("test"));
	std::vector<bbq_sample> same;
	same.push_back(h);
	only_hourly.set_samples(std::move(same));
	alone.set_series(std::move(only_hourly));

	const double without_radar = bbq_grill_score(alone, utc, when, policy);

	QVERIFY(cold >= 0.0);
	QCOMPARE(cold, without_radar);
	QVERIFY2(cold < 0.5, "1 C scored as though the temperature were unknown");
}

void test_model::radar_still_sharpens_the_rain() {
	/*
	 * The other half of sec 3.18, and the half that is easy to lose
	 * while fixing the first. Keeping radar from owning the column is
	 * only correct if its rain still arrives: five-minute
	 * precipitation is a better answer than an hourly mean, and a fix
	 * that quietly discarded it would trade one silent loss for
	 * another.
	 */
	const qint64 when = 1786125600;

	bbq_series hourly(bbq_band::hourly, QStringLiteral("test"));
	std::vector<bbq_sample> coarse;
	bbq_sample h;
	h.start_utc = when - 1800;
	h.duration_s = 3600;
	h.temperature = 20.0;
	h.precip_rate = 5.0; /* the hourly mean, which radar disagrees with */
	coarse.push_back(h);
	hourly.set_samples(std::move(coarse));

	bbq_series radar(bbq_band::nowcast_fine, QStringLiteral("test"));
	std::vector<bbq_sample> fine;
	bbq_sample r;
	r.start_utc = when - 150;
	r.duration_s = 300;
	r.precip_rate = 0.0; /* it has stopped, and radar knows first */
	fine.push_back(r);
	radar.set_samples(std::move(fine));

	bbq_composite composite;
	composite.set_series(std::move(hourly));
	composite.set_series(std::move(radar));

	const bbq_sample resolved = composite.resolved_at(when);

	QVERIFY(resolved.temperature.has_value());
	QCOMPARE(*resolved.temperature, 20.0);

	QVERIFY(resolved.precip_rate.has_value());
	QCOMPARE(*resolved.precip_rate, 0.0);
}

QTEST_APPLESS_MAIN(test_model)
#include "test_model.moc"
