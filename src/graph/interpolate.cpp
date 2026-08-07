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
		return "Monotone cubic";
	case bbq_interpolation::natural:
		return "Natural cubic";
	}

	return "Linear";
}

void bbq_curve::set(std::vector<bbq_knot> knots, bbq_interpolation method) {
	m_knots = std::move(knots);
	m_method = method;
	m_slopes.clear();

	if (m_method == bbq_interpolation::monotone ||
	    m_method == bbq_interpolation::natural) {
		compute_slopes();
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
	if (n < 2) {
		return;
	}

	std::vector<double> h(n - 1);
	std::vector<double> delta(n - 1);

	for (std::size_t i = 0; i + 1 < n; ++i) {
		h[i] = m_knots[i + 1].x - m_knots[i].x;
		delta[i] = h[i] > 0.0 ? (m_knots[i + 1].y - m_knots[i].y) / h[i] : 0.0;
	}

	m_slopes.assign(n, 0.0);

	if (m_method == bbq_interpolation::natural) {
		/*
		 * Catmull-Rom tangents: a centred difference, smoothest of the
		 * four and the only one that can OVERSHOOT.
		 *
		 * That is not a defect to fix here, it is the property that
		 * makes it a separate choice from monotone -- and it is why
		 * sec 3.11.2 warns about it. Between two samples this can draw
		 * a value higher than either, and no mark on the samples
		 * catches that, because the invented extreme sits between the
		 * marks. Callers clamp the quantities that have a physical
		 * floor.
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

	/*
	 * Fritsch-Carlson: smooth, and bounded by its neighbouring samples
	 * by construction, so it cannot invent a peak that no sample
	 * contains. That boundedness is the whole reason this is the
	 * sensible smooth default (sec 3.11.2).
	 */
	m_slopes[0] = delta[0];
	m_slopes[n - 1] = delta[n - 2];

	for (std::size_t i = 1; i + 1 < n; ++i) {
		const double a = delta[i - 1];
		const double b = delta[i];

		/*
		 * A sign change or a flat neighbour means this knot is a local
		 * extreme, and a zero tangent there is what stops the curve
		 * running past it.
		 */
		if (a * b <= 0.0) {
			m_slopes[i] = 0.0;
			continue;
		}

		const double w1 = 2.0 * h[i] + h[i - 1];
		const double w2 = h[i] + 2.0 * h[i - 1];
		m_slopes[i] = (w1 + w2) / (w1 / a + w2 / b);
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

	/* The interval containing x. */
	std::size_t i = 0;
	while (i + 2 < m_knots.size() && m_knots[i + 1].x <= x) {
		++i;
	}

	const bbq_knot &left = m_knots[i];
	const bbq_knot &right = m_knots[i + 1];
	const double h = right.x - left.x;

	if (h <= 0.0) {
		return left.y;
	}

	switch (m_method) {
	case bbq_interpolation::step:
		/*
		 * Hold each value across its own span, which for a quantity
		 * that is a MEAN across a span is not an approximation -- it
		 * is the measurement (sec 3.11.2).
		 */
		return left.y;

	case bbq_interpolation::linear:
		return left.y + (right.y - left.y) * ((x - left.x) / h);

	case bbq_interpolation::monotone:
	case bbq_interpolation::natural:
		break;
	}

	/* Cubic Hermite, with whichever tangents were computed. */
	const double t = (x - left.x) / h;
	const double t2 = t * t;
	const double t3 = t2 * t;

	const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
	const double h10 = t3 - 2.0 * t2 + t;
	const double h01 = -2.0 * t3 + 3.0 * t2;
	const double h11 = t3 - t2;

	return h00 * left.y + h10 * h * m_slopes[i] + h01 * right.y +
	       h11 * h * m_slopes[i + 1];
}
