#ifndef BBQ_SAMPLE_H
#define BBQ_SAMPLE_H

#include <QtGlobal>

#include <optional>

/*
 * One sample of weather over a span of time (project.md sec 3.1).
 *
 * A SPAN, not a point, and the duration is load-bearing rather than
 * decorative. The bands step at roughly 5, 15 and 60 minutes, so "the
 * next sample" is not a fixed distance away; rain is a mean across an
 * interval rather than a value at a moment; and a band's coverage has
 * to end somewhere expressible, since with bare timestamps a series
 * that stops at +7 hours is indistinguishable from one with a gap at
 * the end.
 *
 * "Roughly" is not hedging. The observed band arrives at 288 and 306
 * second intervals rather than a clean 300, measured on real responses,
 * so a model that assumed a fixed cadence would have been wrong on the
 * very first band it read.
 *
 * The two quantities do NOT have the same semantics, and that is
 * deliberate -- it comes from the data, and hiding it would only move
 * it somewhere less visible. Temperature is the value at start_utc.
 * Rain is the mean across the whole span.
 */
struct bbq_sample {
	/*
	 * Epoch seconds, UTC. The one representation of time in this model;
	 * every reader converts into it, including the nowcast band, which
	 * arrives as local time with an offset and no UTC field at all
	 * (sec 2.6.2).
	 */
	qint64 start_utc = 0;

	/*
	 * How long this sample covers. Never inferred from a band's
	 * nominal step -- see the 288-versus-306 note above.
	 */
	int duration_s = 0;

	/* Degrees Celsius. */
	std::optional<double> temperature;

	/* Millimetres per hour, the mean across [start_utc, end_utc). */
	std::optional<double> precip_rate;

	/*
	 * Probability of precipitation over this span, 0 to 100.
	 *
	 * A third quantity rather than a flavour of the second, and the
	 * distinction is the point: a rate says how hard it would rain and
	 * this says whether it will. Ten percent of heavy rain and ninety
	 * percent of drizzle are different afternoons, and neither number
	 * can be recovered from the other.
	 *
	 * Absent on the measured bands, and correctly so -- an observation
	 * has no probability, it either rained or it did not.
	 */
	std::optional<double> precip_chance;

	/*
	 * Wind speed in kilometres per hour, which is what units=m returns
	 * and is kept rather than converted -- one representation, and the
	 * conversion nobody performs is the one nobody gets wrong.
	 *
	 * Here because sec 7 scores it: wind steals heat from a grill,
	 * blows smoke at the cook and carries embers.
	 */
	std::optional<double> wind_kph;

	qint64 end_utc() const {
		return start_utc + duration_s;
	}

	bool covers(qint64 when_utc) const {
		return when_utc >= start_utc && when_utc < end_utc();
	}
};

/*
 * An accumulation over a span, converted to a rate in mm/h (sec 3.2).
 *
 * Rain is stored as a RATE because a rate is intensive -- the same
 * weather reads the same at any cadence -- where an accumulation per
 * step is extensive and would give one number at five minutes and a
 * different one at sixty for identical weather.
 *
 * The division is written out even though the only caller today passes
 * exactly one hour, making it a division by one. The day a provider
 * offers a three-hourly qpf, the shortcut everybody would have taken
 * is a silent factor of three.
 *
 * A span with no duration yields no rate rather than a zero one: zero
 * millimetres per hour is a claim that it is not raining, and "this
 * sample cannot say" is a different statement the optional keeps
 * distinct.
 */
inline std::optional<double> bbq_rate_from_accumulation(double millimetres,
                                                        int duration_s) {
	if (duration_s <= 0) {
		return std::nullopt;
	}

	return millimetres * 3600.0 / duration_s;
}

#endif
