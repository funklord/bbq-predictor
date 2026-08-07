#include <QTest>

#include <cmath>

#include "graph/interpolate.h"

/*
 * The invariants the curve design rests on (project.md sec 3.11).
 *
 * These are not coverage for its own sake. Each one is a claim the
 * document makes -- that interpolation passes through the data, that
 * monotone cannot overshoot, that Catmull-Rom can -- and a claim
 * nothing checks is a claim that quietly stops being true.
 */
class test_interpolate : public QObject {
	Q_OBJECT

private slots:
	void every_method_passes_through_its_knots();
	void step_holds_its_value();
	void linear_splits_the_difference();
	void monotone_never_overshoots();
	void catmull_rom_does_overshoot();
	void smoothing_is_identity_at_zero();
	void smoothing_leaves_a_straight_line_alone();
	void smoothing_flattens_a_staircase();

private:
	static std::vector<bbq_knot> staircase();
};

std::vector<bbq_knot> test_interpolate::staircase() {
	/* Quantised the way WU reports whole degrees (sec 3.11.2.2). */
	const double values[] = {20, 20, 20, 19, 19, 18, 18, 18, 17, 16, 16, 15};

	std::vector<bbq_knot> knots;
	for (int i = 0; i < 12; ++i) {
		bbq_knot knot;
		knot.x = i * 10.0;
		knot.y = values[i];
		knots.push_back(knot);
	}

	return knots;
}

void test_interpolate::every_method_passes_through_its_knots() {
	/*
	 * The line between interpolation and approximation, and the reason
	 * rounding is a separate control rather than another method here.
	 */
	const bbq_interpolation methods[] = {
		bbq_interpolation::step,     bbq_interpolation::linear,
		bbq_interpolation::monotone, bbq_interpolation::akima,
		bbq_interpolation::makima,   bbq_interpolation::natural,
		bbq_interpolation::catmull,
	};

	for (bbq_interpolation method : methods) {
		bbq_curve curve;
		curve.set(staircase(), method);

		for (const bbq_knot &knot : staircase()) {
			const double drawn = curve.at(knot.x);
			QVERIFY2(std::fabs(drawn - knot.y) < 1e-6,
			         qPrintable(QStringLiteral("%1 missed a knot at x=%2")
			                            .arg(bbq_interpolation_name(method))
			                            .arg(knot.x)));
		}
	}
}

void test_interpolate::step_holds_its_value() {
	bbq_curve curve;
	curve.set(staircase(), bbq_interpolation::step);

	/* Anywhere inside a span reads as that span's own value. */
	QCOMPARE(curve.at(1.0), 20.0);
	QCOMPARE(curve.at(9.9), 20.0);
	QCOMPARE(curve.at(35.0), 19.0);
}

void test_interpolate::linear_splits_the_difference() {
	bbq_curve curve;
	curve.set(staircase(), bbq_interpolation::linear);

	/* Halfway between 20 and 19 is 19.5, and nothing cleverer. */
	QVERIFY(std::fabs(curve.at(25.0) - 19.5) < 1e-9);
}

void test_interpolate::monotone_never_overshoots() {
	/*
	 * The property that makes monotone the safe default (sec 3.11.2).
	 * Checked densely rather than at a few points, because an overshoot
	 * is a bulge between samples and sampling sparsely is how you miss
	 * one.
	 */
	const std::vector<bbq_knot> knots = staircase();
	bbq_curve curve;
	curve.set(knots, bbq_interpolation::monotone);

	for (std::size_t i = 0; i + 1 < knots.size(); ++i) {
		const double low = std::min(knots[i].y, knots[i + 1].y);
		const double high = std::max(knots[i].y, knots[i + 1].y);

		for (int step = 0; step <= 40; ++step) {
			const double x = knots[i].x +
			                 (knots[i + 1].x - knots[i].x) * (step / 40.0);
			const double y = curve.at(x);
			QVERIFY2(y >= low - 1e-9 && y <= high + 1e-9,
			         qPrintable(QStringLiteral("monotone left [%1,%2] at x=%3 "
			                                   "with %4")
			                            .arg(low).arg(high).arg(x).arg(y)));
		}
	}
}

void test_interpolate::catmull_rom_does_overshoot() {
	/*
	 * The counter-property, and it is a test rather than a comment
	 * because it is WHY monotone is the default. If a future change
	 * made Catmull-Rom bounded, this failing is the right way to find
	 * out -- the choice between them would then need revisiting.
	 */
	const std::vector<bbq_knot> knots = staircase();
	bbq_curve curve;
	curve.set(knots, bbq_interpolation::catmull);

	bool left_the_envelope = false;

	for (std::size_t i = 0; i + 1 < knots.size(); ++i) {
		const double low = std::min(knots[i].y, knots[i + 1].y);
		const double high = std::max(knots[i].y, knots[i + 1].y);

		for (int step = 0; step <= 40; ++step) {
			const double x = knots[i].x +
			                 (knots[i + 1].x - knots[i].x) * (step / 40.0);
			const double y = curve.at(x);
			if (y < low - 1e-6 || y > high + 1e-6) {
				left_the_envelope = true;
			}
		}
	}

	QVERIFY2(left_the_envelope,
	         "Catmull-Rom stayed inside its samples on data chosen to make "
	         "it ring; sec 3.11.2's reason for preferring monotone would "
	         "need rechecking");
}

void test_interpolate::smoothing_is_identity_at_zero() {
	std::vector<bbq_knot> knots = staircase();
	const std::vector<bbq_knot> before = knots;

	bbq_smooth(knots, 0.0);

	for (std::size_t i = 0; i < knots.size(); ++i) {
		QCOMPARE(knots[i].y, before[i].y);
	}
}

void test_interpolate::smoothing_leaves_a_straight_line_alone() {
	/*
	 * Local LINEAR regression reproduces a line exactly, including at
	 * the ends. A weighted average would sag at both, which is the
	 * specific reason the extra terms are there (sec 3.11.4).
	 */
	std::vector<bbq_knot> knots;
	for (int i = 0; i < 20; ++i) {
		bbq_knot knot;
		knot.x = i * 10.0;
		knot.y = 3.0 + 0.5 * knot.x;
		knots.push_back(knot);
	}

	std::vector<bbq_knot> smoothed = knots;
	bbq_smooth(smoothed, 25.0);

	for (std::size_t i = 0; i < knots.size(); ++i) {
		QVERIFY2(std::fabs(smoothed[i].y - knots[i].y) < 1e-6,
		         qPrintable(QStringLiteral("bent a straight line at i=%1: "
		                                   "%2 became %3")
		                            .arg(i).arg(knots[i].y).arg(smoothed[i].y)));
	}
}

void test_interpolate::smoothing_flattens_a_staircase() {
	/* What it is for: the corners come off (sec 3.11.4). */
	const std::vector<bbq_knot> before = staircase();
	std::vector<bbq_knot> after = before;
	bbq_smooth(after, 20.0);

	const auto roughness = [](const std::vector<bbq_knot> &knots) {
		double total = 0.0;
		for (std::size_t i = 1; i + 1 < knots.size(); ++i) {
			const double bend =
			        knots[i + 1].y - 2.0 * knots[i].y + knots[i - 1].y;
			total += std::fabs(bend);
		}
		return total;
	};

	QVERIFY2(roughness(after) < roughness(before),
	         "rounding did not reduce the curvature of a staircase");
}

QTEST_APPLESS_MAIN(test_interpolate)
#include "test_interpolate.moc"
