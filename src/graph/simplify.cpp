#include "graph/simplify.h"

#include <cmath>
#include <utility>
#include <vector>

/*
 * Perpendicular distance from `p` to the infinite line through `a` and
 * `b`, or to `a` itself where the two are the same point.
 *
 * The degenerate case is not hypothetical: a run whose columns all carry
 * the same value gives a first and last point with the same coordinates
 * once the recursion has narrowed, and dividing by that length would put
 * a NaN into the comparison -- which compares false, so every point
 * would be kept and the whole exercise would quietly do nothing.
 */
static double bbq_line_distance(const QPointF &p, const QPointF &a,
                                const QPointF &b) {
	const double dx = b.x() - a.x();
	const double dy = b.y() - a.y();
	const double len = std::hypot(dx, dy);

	if (len < 1e-9) {
		return std::hypot(p.x() - a.x(), p.y() - a.y());
	}

	return std::fabs(dy * (p.x() - a.x()) - dx * (p.y() - a.y())) / len;
}

QPolygonF bbq_simplify_polyline(const QPolygonF &line, double tolerance) {
	if (line.size() < 3 || tolerance <= 0.0) {
		return line;
	}

	std::vector<bool> keep(size_t(line.size()), false);
	keep.front() = true;
	keep.back() = true;

	/*
	 * Iterative rather than recursive. The depth is bounded by the point
	 * count in the worst case -- a curve where every point matters --
	 * and a stroked polyline is as long as the plot is wide, so this is
	 * a stack somebody else's screen size chooses.
	 */
	std::vector<std::pair<int, int>> todo;
	todo.push_back({0, int(line.size()) - 1});

	while (!todo.empty()) {
		const std::pair<int, int> span = todo.back();
		todo.pop_back();

		if (span.second <= span.first + 1) {
			continue;
		}

		int worst = -1;
		double worst_distance = tolerance;

		for (int at = span.first + 1; at < span.second; ++at) {
			const double distance = bbq_line_distance(
			        line[at], line[span.first], line[span.second]);
			if (distance > worst_distance) {
				worst_distance = distance;
				worst = at;
			}
		}

		if (worst < 0) {
			continue;
		}

		keep[size_t(worst)] = true;
		todo.push_back({span.first, worst});
		todo.push_back({worst, span.second});
	}

	QPolygonF kept;
	kept.reserve(line.size());

	for (int at = 0; at < line.size(); ++at) {
		if (keep[size_t(at)]) {
			kept << line[at];
		}
	}

	return kept;
}
