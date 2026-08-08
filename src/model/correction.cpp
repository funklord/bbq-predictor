#include "model/correction.h"

#include <algorithm>
#include <map>
#include <vector>

namespace {

/*
 * How often the composite is asked which band is winning.
 *
 * Five minutes, matching the finest band, so a handover from one band to
 * another is not missed. This is NOT the spacing of the result: a
 * corrected point is emitted per underlying forecast SAMPLE, not per
 * probe, for the reason in sec 12.9.
 */
const qint64 probe_s = 5 * 60;

/*
 * The bands worth correcting.
 *
 * Measurements are not among them, which is the whole point: an
 * observation has no lead time and no error to speak of, and
 * "correcting" one would be adjusting the thing everything else is
 * being scored against.
 */
bool is_forecast(bbq_band band) {
	switch (band) {
	case bbq_band::nowcast_fine:
	case bbq_band::nowcast:
	case bbq_band::hourly:
	case bbq_band::extended:
		return true;
	case bbq_band::observed:
	case bbq_band::current:
	case bbq_band::corrected:
		return false;
	}

	return false;
}

} // namespace

bbq_series bbq_corrected_forecast(const bbq_composite &composite,
                                  const bbq_history &history,
                                  const QString &station, qint64 from_utc,
                                  qint64 to_utc, qint64 issued_utc,
                                  int minimum_count) {
	bbq_series corrected(bbq_band::corrected, QStringLiteral("bias-corrected"));

	if (!history.is_open() || station.isEmpty() || to_utc <= from_utc) {
		return corrected;
	}

	/*
	 * The bias as a function of lead time, per band, built once.
	 *
	 * Looked up by bucket and then INTERPOLATED between bucket centres,
	 * which the first rendering showed is not optional (sec 12.9). A
	 * bucketed bias is piecewise constant, so subtracting it directly
	 * drew the corrected curve as a staircase -- a jump at every bucket
	 * edge, giving the line structure that came from the bucketing
	 * rather than from the weather.
	 */
	struct bias_point {
		qint64 lead_s = 0;
		double bias = 0.0;
	};

	/*
	 * Keyed by band AND quantity, because they are corrected
	 * independently: a provider can be reliably warm and perfectly good
	 * about rain, and one number covering both would describe neither.
	 */
	std::map<std::pair<int, QString>, std::vector<bias_point>> curves;

	/*
	 * The bias a band has been showing for a quantity at a given lead,
	 * or nothing when there is not enough evidence to say.
	 *
	 * Built on first use and then reused, so the store is asked once per
	 * band and quantity rather than once per sample -- sixteen days of
	 * hourly data is hundreds of points and this runs on every repaint.
	 */
	const auto bias_for = [&](bbq_band band, const QString &quantity,
	                          qint64 lead_s, double *out) {
		const std::pair<int, QString> key(static_cast<int>(band), quantity);

		if (curves.find(key) == curves.end()) {
			std::vector<bias_point> points;

			const bbq_lead_bucket every[] = {
				bbq_lead_bucket::hour, bbq_lead_bucket::three_hours,
				bbq_lead_bucket::six_hours, bbq_lead_bucket::twelve_hours,
				bbq_lead_bucket::day, bbq_lead_bucket::two_days,
				bbq_lead_bucket::four_days, bbq_lead_bucket::week,
				bbq_lead_bucket::beyond};

			for (bbq_lead_bucket candidate : every) {
				const bbq_verification score =
				        history.verification(station, band, quantity, candidate);

				if (score.count >= minimum_count) {
					bias_point point;
					point.lead_s = bbq_lead_bucket_centre_s(candidate);
					point.bias = score.bias;
					points.push_back(point);
				}
			}

			curves[key] = points;
		}

		const std::vector<bias_point> &points = curves[key];
		if (points.empty()) {
			return false;
		}

		/*
		 * Held flat outside the range that has evidence, at BOTH ends.
		 *
		 * Clamping only the top was a real defect (sec 12.9): every lead
		 * shorter than the first bucket's centre ran the line backwards
		 * off the end of its data and produced a bias of the wrong sign,
		 * making the forecast worse at the one lead where it is most
		 * nearly right.
		 */
		if (lead_s <= points.front().lead_s) {
			*out = points.front().bias;
			return true;
		}

		if (lead_s >= points.back().lead_s) {
			*out = points.back().bias;
			return true;
		}

		for (std::size_t i = 1; i < points.size(); ++i) {
			if (lead_s > points[i].lead_s) {
				continue;
			}

			const bias_point &left = points[i - 1];
			const bias_point &right = points[i];
			const double span = static_cast<double>(right.lead_s - left.lead_s);

			if (span <= 0.0) {
				*out = right.bias;
				return true;
			}

			const double t = (lead_s - left.lead_s) / span;
			*out = left.bias + (right.bias - left.bias) * t;
			return true;
		}

		*out = points.back().bias;
		return true;
	};

	std::vector<bbq_sample> samples;

	/*
	 * Only ahead of now. Behind it there are observations, and a
	 * corrected forecast drawn across a period that was actually
	 * measured would be arguing with the record.
	 */
	const qint64 start = std::max(from_utc, issued_utc);

	/*
	 * One corrected point per real forecast sample, not one per probe.
	 *
	 * The first version emitted every quarter hour and drew a staircase,
	 * and the bucketing was not the cause. A forecast sample holds one
	 * temperature across its whole span (sec 3.1), so probing an hourly
	 * band four times an hour repeats the same value four times. The
	 * red line is smooth because the graph joins sample STARTS; the
	 * correction has to be built on the same starts or it disagrees with
	 * the curve it is drawn against, in a way that looks like data.
	 */
	qint64 last_start = 0;
	bool have_last = false;

	for (qint64 when = start; when < to_utc; when += probe_s) {
		const bbq_reading reading = composite.at(when);
		/*
		 * Only that SOMETHING is covered here. Requiring a temperature
		 * was left over from when temperature was the only quantity
		 * corrected, and it silently dropped every sample that carried
		 * rain without one -- so rain could never be corrected on its
		 * own. Which quantities are present is decided below, per
		 * quantity, where the evidence for each is also checked.
		 */
		if (!reading.is_valid()) {
			continue;
		}

		const bbq_band band = reading.series->band();
		if (!is_forecast(band)) {
			continue;
		}

		const qint64 sample_start = reading.sample->start_utc;
		if (have_last && sample_start == last_start) {
			continue;
		}

		have_last = true;
		last_start = sample_start;

		/*
		 * Placed at the sample's own start, except where that is behind
		 * the range being drawn -- a sample straddling now begins the
		 * curve at now rather than before it.
		 */
		const qint64 place = std::max(sample_start, start);
		const qint64 lead = place - issued_utc;

		double temperature_bias = 0.0;
		double rain_bias = 0.0;
		double wind_bias = 0.0;

		const bool know_temperature =
		        reading.sample->temperature.has_value() &&
		        bias_for(band, QStringLiteral("temperature"), lead,
		                 &temperature_bias);

		const bool know_rain =
		        reading.sample->precip_rate.has_value() &&
		        bias_for(band, QStringLiteral("precip_rate"), lead, &rain_bias);

		const bool know_wind =
		        reading.sample->wind_kph.has_value() &&
		        bias_for(band, QStringLiteral("wind_kph"), lead, &wind_bias);

		if (!know_temperature && !know_rain && !know_wind) {
			continue;
		}

		bbq_sample sample;
		sample.start_utc = place;
		sample.duration_s = reading.sample->duration_s;

		/*
		 * Subtracted, because the bias is forecast minus observed: a
		 * band that has been running warm gets its prediction brought
		 * down by however much it has been running warm by.
		 */
		if (know_temperature) {
			sample.temperature = *reading.sample->temperature - temperature_bias;
		}

		if (know_rain) {
			/*
			 * Floored at zero. A band that over-forecasts rain would
			 * otherwise be corrected into negative rainfall, which is
			 * not a thing -- the same clamp sec 3.11.2 puts on the drawn
			 * curve, for the same reason.
			 */
			const double rate = *reading.sample->precip_rate - rain_bias;
			sample.precip_rate = std::max(0.0, rate);
		}

		if (know_wind) {
			/* Floored for the same reason rain is: there is no negative wind. */
			const double speed = *reading.sample->wind_kph - wind_bias;
			sample.wind_kph = std::max(0.0, speed);
		}

		samples.push_back(sample);
	}

	corrected.set_samples(std::move(samples));
	return corrected;
}
