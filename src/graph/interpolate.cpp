#include "graph/interpolate.h"

#include <algorithm>
#include <cmath>

const char *bbq_interpolation_name(bbq_interpolation method) {
	switch (method) {
	case bbq_interpolation::step:
		return "Step";
	case bbq_interpolation::linear:
		return "Linear";
	case bbq_interpolation::monotone:
		return "Monotone (PCHIP)";
	case bbq_interpolation::akima:
		return "Akima";
	case bbq_interpolation::makima:
		return "Akima (modified)";
	case bbq_interpolation::natural:
		return "Natural cubic";
	case bbq_interpolation::catmull:
		return "Catmull-Rom";
	}

	return "Linear";
}

namespace {

/* Hermite basis on a unit interval, scaled by the span. */
double hermite(double left, double right, double sl, double sr, double span, double t) {
	const double t2 = t * t;
	const double t3 = t2 * t;

	const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
	const double h10 = t3 - 2.0 * t2 + t;
	const double h01 = -2.0 * t3 + 3.0 * t2;
	const double h11 = t3 - t2;

	const double a = h00 * left + h10 * span * sl;
	const double b = h01 * right + h11 * span * sr;
	return a + b;
}

} // namespace

void bbq_smooth(std::vector<bbq_knot> &knots, double bandwidth) {
	if (bandwidth <= 0.0 || knots.size() < 3) {
		return;
	}

	const std::size_t n = knots.size();
	std::vector<double> smoothed(n, 0.0);

	/*
	 * Beyond three sigma a Gaussian contributes essentially nothing, so
	 * a knot outside that reach is skipped rather than weighed.
	 *
	 * That bounds the WORK per centre, not the iteration count: the
	 * inner loop still visits every knot and drops most of them at the
	 * distance test. An earlier comment here claimed the pass was
	 * therefore linear, which it is not -- it is quadratic in visits and
	 * linear in arithmetic. At the few hundred knots a column set
	 * produces the difference is not measurable, and making it truly
	 * linear means requiring the knots to be sorted by x, which is a
	 * contract this function does not currently impose on its callers.
	 * Left as it is, deliberately, rather than left described wrongly.
	 */
	const double reach = bandwidth * 3.0;

	for (std::size_t i = 0; i < n; ++i) {
		const double centre = knots[i].x;

		double sw = 0.0;
		double swx = 0.0;
		double swy = 0.0;
		double swxx = 0.0;
		double swxy = 0.0;

		for (std::size_t j = 0; j < n; ++j) {
			const double dx = knots[j].x - centre;
			if (std::fabs(dx) > reach) {
				continue;
			}

			const double t = dx / bandwidth;
			const double w = std::exp(-0.5 * t * t);

			sw += w;
			swx += w * dx;
			swy += w * knots[j].y;
			swxx += w * dx * dx;
			swxy += w * dx * knots[j].y;
		}

		/*
		 * Solve the weighted least-squares line and take its value at
		 * the centre, which is the intercept. A denominator at zero
		 * means every weighted neighbour sits at the same x, and the
		 * weighted mean is then the whole answer.
		 */
		const double denom = sw * swxx - swx * swx;
		if (sw <= 0.0) {
			smoothed[i] = knots[i].y;
		} else if (std::fabs(denom) < 1e-9) {
			smoothed[i] = swy / sw;
		} else {
			smoothed[i] = (swxx * swy - swx * swxy) / denom;
		}
	}

	for (std::size_t i = 0; i < n; ++i) {
		knots[i].y = smoothed[i];
	}
}

void bbq_curve::set(std::vector<bbq_knot> knots, bbq_interpolation method) {
	m_knots = std::move(knots);
	m_method = method;
	m_slopes.clear();
	m_second.clear();

	if (m_knots.size() < 2) {
		return;
	}

	switch (m_method) {
	case bbq_interpolation::step:
	case bbq_interpolation::linear:
		break;
	case bbq_interpolation::natural:
		compute_natural();
		break;
	default:
		compute_slopes();
		break;
	}
}

double bbq_curve::first_x() const {
	return m_knots.empty() ? 0.0 : m_knots.front().x;
}

double bbq_curve::last_x() const {
	return m_knots.empty() ? 0.0 : m_knots.back().x;
}

void bbq_curve::compute_slopes() {
	const std::size_t n = m_knots.size();

	std::vector<double> h(n - 1);
	std::vector<double> delta(n - 1);

	for (std::size_t i = 0; i + 1 < n; ++i) {
		h[i] = m_knots[i + 1].x - m_knots[i].x;
		delta[i] = h[i] > 0.0 ? (m_knots[i + 1].y - m_knots[i].y) / h[i] : 0.0;
	}

	m_slopes.assign(n, 0.0);

	if (m_method == bbq_interpolation::catmull) {
		/*
		 * A centred difference. The cheapest smooth option and the one
		 * most willing to overshoot, kept because it is what a lot of
		 * tools mean by "spline" and it is useful to be able to see
		 * what the others are avoiding.
		 */
		m_slopes[0] = delta[0];
		m_slopes[n - 1] = delta[n - 2];

		for (std::size_t i = 1; i + 1 < n; ++i) {
			const double span = m_knots[i + 1].x - m_knots[i - 1].x;
			if (span > 0.0) {
				m_slopes[i] = (m_knots[i + 1].y - m_knots[i - 1].y) / span;
			}
		}

		return;
	}

	if (m_method == bbq_interpolation::akima ||
	    m_method == bbq_interpolation::makima) {
		/*
		 * Akima's method, and the reason it belongs in a weather graph
		 * specifically.
		 *
		 * A cubic spline decides each tangent from the whole curve, so
		 * one sharp change ripples outward and wobbles regions that
		 * were flat. Akima decides each tangent from only the four
		 * nearest slopes, so a sudden change stays local -- which is
		 * exactly the shape of this data, where a temperature plateau
		 * sits next to a fast evening drop.
		 *
		 * The weighting is what does it: a tangent is pulled towards
		 * whichever neighbouring slope is in the more AGREEABLE
		 * company, so a slope flanked by two similar slopes wins over
		 * one flanked by a jump.
		 */
		std::vector<double> s(n + 3, 0.0);
		for (std::size_t i = 0; i + 1 < n; ++i) {
			s[i + 2] = delta[i];
		}

		/* Akima's own end extension, so the ends need no special case. */
		s[1] = 2.0 * s[2] - s[3];
		s[0] = 2.0 * s[1] - s[2];
		s[n + 1] = 2.0 * s[n] - s[n - 1];
		s[n + 2] = 2.0 * s[n + 1] - s[n];

		for (std::size_t i = 0; i < n; ++i) {
			const double m0 = s[i];
			const double m1 = s[i + 1];
			const double m2 = s[i + 2];
			const double m3 = s[i + 3];

			double w1 = std::fabs(m3 - m2);
			double w2 = std::fabs(m1 - m0);

			if (m_method == bbq_interpolation::makima) {
				/*
				 * The modified weights. Plain Akima divides by zero in
				 * spirit when three samples in a row are equal -- the
				 * weights both vanish and the tangent falls back to an
				 * average, which puts a small kink in a flat run. This
				 * adds the slopes' magnitude, so a flat run stays flat.
				 */
				w1 += std::fabs(m3 + m2) / 2.0;
				w2 += std::fabs(m1 + m0) / 2.0;
			}

			const double total = w1 + w2;
			if (total <= 0.0) {
				m_slopes[i] = (m1 + m2) / 2.0;
			} else {
				m_slopes[i] = (w1 * m1 + w2 * m2) / total;
			}
		}

		return;
	}

	/*
	 * Fritsch-Carlson: bounded by its neighbouring samples by
	 * construction, so it cannot invent a peak no sample contains.
	 */
	m_slopes[0] = delta[0];
	m_slopes[n - 1] = delta[n - 2];

	for (std::size_t i = 1; i + 1 < n; ++i) {
		const double a = delta[i - 1];
		const double b = delta[i];

		if (a * b <= 0.0) {
			m_slopes[i] = 0.0;
			continue;
		}

		const double w1 = 2.0 * h[i] + h[i - 1];
		const double w2 = h[i] + 2.0 * h[i - 1];
		m_slopes[i] = (w1 + w2) / (w1 / a + w2 / b);
	}
}

void bbq_curve::compute_natural() {
	/*
	 * A real natural cubic spline: continuous in the second derivative,
	 * with zero curvature at both ends.
	 *
	 * This is what "natural cubic" means, and it is NOT what this
	 * project called by that name until now -- that was Catmull-Rom,
	 * which is only C1 and is a different curve. The two are both
	 * offered now under their own names.
	 *
	 * Solved with the Thomas algorithm; the system is tridiagonal and
	 * diagonally dominant, so no pivoting is needed.
	 */
	const std::size_t n = m_knots.size();
	m_second.assign(n, 0.0);

	if (n < 3) {
		return;
	}

	std::vector<double> h(n - 1);
	for (std::size_t i = 0; i + 1 < n; ++i) {
		h[i] = m_knots[i + 1].x - m_knots[i].x;
	}

	std::vector<double> alpha(n, 0.0);
	for (std::size_t i = 1; i + 1 < n; ++i) {
		if (h[i] <= 0.0 || h[i - 1] <= 0.0) {
			continue;
		}

		const double right = (m_knots[i + 1].y - m_knots[i].y) / h[i];
		const double left = (m_knots[i].y - m_knots[i - 1].y) / h[i - 1];
		alpha[i] = 3.0 * (right - left);
	}

	std::vector<double> l(n, 1.0);
	std::vector<double> mu(n, 0.0);
	std::vector<double> z(n, 0.0);

	for (std::size_t i = 1; i + 1 < n; ++i) {
		l[i] = 2.0 * (m_knots[i + 1].x - m_knots[i - 1].x) - h[i - 1] * mu[i - 1];
		if (l[i] == 0.0) {
			l[i] = 1.0;
		}
		mu[i] = h[i] / l[i];
		z[i] = (alpha[i] - h[i - 1] * z[i - 1]) / l[i];
	}

	for (std::size_t i = n - 1; i-- > 0;) {
		m_second[i] = z[i] - mu[i] * m_second[i + 1];
	}
}

double bbq_curve::at(double x) const {
	if (m_knots.empty()) {
		return 0.0;
	}

	if (m_knots.size() == 1 || x <= m_knots.front().x) {
		return m_knots.front().y;
	}

	if (x >= m_knots.back().x) {
		return m_knots.back().y;
	}

	std::size_t i = 0;
	while (i + 2 < m_knots.size() && m_knots[i + 1].x <= x) {
		++i;
	}

	const bbq_knot &left = m_knots[i];
	const bbq_knot &right = m_knots[i + 1];
	const double span = right.x - left.x;

	if (span <= 0.0) {
		return left.y;
	}

	switch (m_method) {
	case bbq_interpolation::step:
		return left.y;

	case bbq_interpolation::linear:
		return left.y + (right.y - left.y) * ((x - left.x) / span);

	case bbq_interpolation::natural: {
		if (m_second.size() != m_knots.size()) {
			return left.y;
		}

		const double dx = x - left.x;
		const double c = m_second[i];
		const double c_next = m_second[i + 1];
		const double slope = (right.y - left.y) / span;
		const double b = slope - span * (c_next + 2.0 * c) / 3.0;
		const double d = (c_next - c) / (3.0 * span);
		return left.y + b * dx + c * dx * dx + d * dx * dx * dx;
	}

	default:
		break;
	}

	if (m_slopes.size() != m_knots.size()) {
		return left.y;
	}

	const double t = (x - left.x) / span;
	return hermite(left.y, right.y, m_slopes[i], m_slopes[i + 1], span, t);
}
