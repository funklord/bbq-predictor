#include <QTest>

#include "ui/layout.h"

/*
 * The two shapes (project.md sec 10).
 *
 * Sec 10 states its differences as a table, and a table in a document
 * is a claim like any other. These assert the ones that carry meaning
 * -- not every number, but every number the document gives a reason
 * for, because those are the ones a later tidy-up would quietly
 * "simplify" back into one shape.
 */
class test_layout : public QObject {
	Q_OBJECT

private slots:
	void a_preference_beats_the_device();
	void anything_else_means_auto();
	void mobile_shows_less_time_not_smaller_time();
	void mobile_spaces_its_ticks_further_apart();
	void mobile_stacks_its_controls_and_makes_them_hittable();
	void mobile_draws_heavier_for_an_arm_s_length();
	void desktop_is_the_untouched_default();
};

void test_layout::a_preference_beats_the_device() {
	/*
	 * Sec 10.1: the compiled-in default is right until somebody
	 * disagrees, and a preference somebody set is better evidence than
	 * a pixel count.
	 */
	QCOMPARE(bbq_layout_resolve(QStringLiteral("desktop")), bbq_layout::desktop);
	QCOMPARE(bbq_layout_resolve(QStringLiteral("mobile")), bbq_layout::mobile);
}

void test_layout::anything_else_means_auto() {
	/*
	 * Including an empty string and a typo. A config file is edited by
	 * hand, and "Mobile" or "phone" must not land somewhere undefined
	 * -- the device's own answer is the safe reading of anything
	 * unrecognised.
	 */
	const bbq_layout device = bbq_layout_for_device();

	QCOMPARE(bbq_layout_resolve(QStringLiteral("auto")), device);
	QCOMPARE(bbq_layout_resolve(QString()), device);
	QCOMPARE(bbq_layout_resolve(QStringLiteral("Mobile")), device);
	QCOMPARE(bbq_layout_resolve(QStringLiteral("phone")), device);
}

void test_layout::mobile_shows_less_time_not_smaller_time() {
	/*
	 * The reason sec 10 gives: the same span on a narrower screen is
	 * the same data drawn thinner, and resolution is this graph's whole
	 * claim. A "mobile" layout that kept the desktop window would be
	 * the scaling this design refused.
	 */
	const bbq_metrics desktop = bbq_metrics_for(bbq_layout::desktop);
	const bbq_metrics mobile = bbq_metrics_for(bbq_layout::mobile);

	const qint64 desktop_span = desktop.window_before_s + desktop.window_after_s;
	const qint64 mobile_span = mobile.window_before_s + mobile.window_after_s;

	QVERIFY2(mobile_span < desktop_span,
	         "the mobile window is not shorter than the desktop one");

	/* Still enough to answer "this evening", which is the point of it. */
	QVERIFY(mobile.window_after_s >= 8 * 3600);
}

void test_layout::mobile_spaces_its_ticks_further_apart() {
	/* Labels collide at that width, and a collided label is worse. */
	QVERIFY(bbq_metrics_for(bbq_layout::mobile).tick_step_s >
	        bbq_metrics_for(bbq_layout::desktop).tick_step_s);
}

void test_layout::mobile_stacks_its_controls_and_makes_them_hittable() {
	const bbq_metrics mobile = bbq_metrics_for(bbq_layout::mobile);

	QVERIFY2(mobile.stack_controls,
	         "mobile does not stack, so the shape is only a font size -- "
	         "which is exactly what it was on the first attempt");

	/*
	 * 44 points is the usual floor for something a finger has to find.
	 * Asserted as a floor rather than a value, so tuning it upward is
	 * not a test failure.
	 */
	QVERIFY(mobile.control_height >= 40);
	QCOMPARE(bbq_metrics_for(bbq_layout::desktop).stack_controls, false);
}

void test_layout::mobile_draws_heavier_for_an_arm_s_length() {
	const bbq_metrics desktop = bbq_metrics_for(bbq_layout::desktop);
	const bbq_metrics mobile = bbq_metrics_for(bbq_layout::mobile);

	QVERIFY(mobile.sample_radius > desktop.sample_radius);
	QVERIFY(mobile.line_width > desktop.line_width);
	QVERIFY(mobile.label_scale >= desktop.label_scale);
}

void test_layout::desktop_is_the_untouched_default() {
	/*
	 * The desktop metrics are the struct's own defaults, so a field
	 * added later without a mobile answer keeps the desktop value
	 * rather than silently becoming zero on a phone.
	 */
	const bbq_metrics defaults;
	const bbq_metrics desktop = bbq_metrics_for(bbq_layout::desktop);

	QCOMPARE(desktop.margin_left, defaults.margin_left);
	QCOMPARE(desktop.tick_step_s, defaults.tick_step_s);
	QCOMPARE(desktop.window_after_s, defaults.window_after_s);
	QCOMPARE(desktop.stack_controls, defaults.stack_controls);
}

QTEST_APPLESS_MAIN(test_layout)
#include "test_layout.moc"
