#ifndef BBQ_INTERPOLATE_H
#define BBQ_INTERPOLATE_H

#include <cstddef>
#include <vector>

/*
 * How the curve is drawn between samples (project.md sec 3.11).
 *
 * A choice rather than a policy: several methods, picked by whoever is
 * reading the graph, which is what a serious series tool offers.
 */
enum class bbq_interpolation {
	step,
	linear,
	monotone,
	akima,
	makima,
	natural,
	catmull,
};

const char *bbq_interpolation_name(bbq_interpolation method);

/* A real sample, positioned. Never an interpolated point. */
struct bbq_knot {
	double x = 0.0;
	double y = 0.0;
};

/*
 * Smooth a set of knots in place (project.md sec 3.11.4).
 *
 * This is APPROXIMATION, not interpolation, and the difference is the
 * whole point: the returned knots no longer sit on the samples, so a
 * curve drawn through them does not pass through the data. That is a
 * larger claim than any method in bbq_interpolation makes, which is
 * why it is a separate control and why the samples stay marked at
 * their real values -- the visible gap between a mark and the curve is
 * the claim being made honestly.
 *
 * Local linear regression with Gaussian weights. A plain weighted
 * average would have been fewer lines and is worse in two specific
 * ways: it flattens peaks, and it bends towards the interior at both
 * ends, which would invent a turn at the edge of the window. Fitting a
 * LINE rather than a level through each neighbourhood removes both.
 *
 * `bandwidth` is in the same units as knot.x and is the Gaussian's
 * sigma. Zero or less leaves the knots untouched.
 *
 * Weighting by actual distance rather than by neighbour count matters
 * here: the bands sample at roughly 5, 15 and 60 minutes, so counting
 * neighbours would smooth an hour of the hourly band as hard as five
 * minutes of the observed one.
 */
void bbq_smooth(std::vector<bbq_knot> &knots, double bandwidth);

/*
 * Evaluates one of the methods over a set of knots.
 *
 * Lives here rather than in the widget because sec 3.11.1 requires
 * interpolated values to stay out of the series: this takes real
 * samples in and hands drawn values out, and keeps nothing. The graph
 * can still say which points were measured, because the knots are the
 * measurements and everything else is computed on demand.
 *
 * Knots must be sorted by x and have distinct x.
 */
class bbq_curve {
public:
	void set(std::vector<bbq_knot> knots, bbq_interpolation method);

	bool is_usable() const { return m_knots.size() >= 2; }
	const std::vector<bbq_knot> &knots() const { return m_knots; }

	double first_x() const;
	double last_x() const;

	/*
	 * The value at x, clamped to the knot range at either end. Callers
	 * are expected to stay inside [first_x, last_x] -- see the note in
	 * the widget about why the ends fall back to measured values rather
	 * than extrapolating.
	 */
	double at(double x) const;

private:
	void compute_slopes();
	void compute_natural();

	std::vector<bbq_knot> m_knots;

	/* Hermite tangents, for every method except step/linear/natural. */
	std::vector<double> m_slopes;

	/* Second derivatives, for the natural cubic's own evaluation. */
	std::vector<double> m_second;
	bbq_interpolation m_method = bbq_interpolation::linear;
};

#endif
