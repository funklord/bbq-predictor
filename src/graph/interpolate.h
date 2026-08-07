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
	natural,
};

const char *bbq_interpolation_name(bbq_interpolation method);

/* A real sample, positioned. Never an interpolated point. */
struct bbq_knot {
	double x = 0.0;
	double y = 0.0;
};

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

	std::vector<bbq_knot> m_knots;
	std::vector<double> m_slopes;
	bbq_interpolation m_method = bbq_interpolation::linear;
};

#endif
