#include <QTest>

#include "ui/layout.h"
#include "graph/ticks.h"

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
	void the_plot_runs_to_the_edge_and_the_controls_do_not();
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
	/*
	 * THROUGH THE MECHANISM THAT DOES IT (sec 16.52).
	 *
	 * This asserted `bbq_metrics::tick_step_s`, a field the graph
	 * stopped reading when the view became zoomable -- the comment
	 * above `bbq_ticks_for` says so in as many words. The test was
	 * named for behaviour that happens and asserted a number nothing
	 * consulted, so it would have passed for ever while the axis went
	 * wrong.
	 *
	 * What actually makes a phone's axis coarser is the width: the same
	 * span over fewer pixels asks for fewer labels and lands further up
	 * the ladder. So that is what is asked here, with the two widths
	 * the layouts really have.
	 */
	const int phone_labels = 420 / 90;
	const int desk_labels = 820 / 90;

	/*
	 * NEVER FINER, and coarser where the ladder has a rung between
	 * them. The old assertion said "further apart" flatly, which is not
	 * true: at a twelve-hour span both land on three hours, because no
	 * rung falls between four labels and nine. It was only ever true of
	 * the dead field, where it was true by construction.
	 */
	for (qint64 span : {3 * 3600, 6 * 3600, 12 * 3600, 24 * 3600,
	                    7 * 24 * 3600, 30 * 24 * 3600}) {
		const bbq_tick_choice phone = bbq_ticks_for(span, phone_labels);
		const bbq_tick_choice desk = bbq_ticks_for(span, desk_labels);

		QVERIFY2(phone.step_s >= desk.step_s,
		         qPrintable(QStringLiteral("at a span of %1 s a phone "
		                                   "ticks every %2 and a desktop "
		                                   "every %3, which is finer on "
		                                   "the smaller screen")
		                            .arg(span)
		                            .arg(phone.step_s)
		                            .arg(desk.step_s)));
	}

	/*
	 * And a witness that it is not merely equal everywhere, which an
	 * inequality alone would allow -- a broken chooser returning one
	 * constant would satisfy every line above.
	 */
	QVERIFY2(bbq_ticks_for(6 * 3600, phone_labels).step_s >
	                 bbq_ticks_for(6 * 3600, desk_labels).step_s,
	         "the two layouts never differ, so the width is being "
	         "ignored");

	/*
	 * And the label follows the STEP rather than the span, which is the
	 * other half of that function and was never tested either: a step
	 * of half a day needs the day named, and one of a quarter of an
	 * hour must not repeat the same date twice.
	 */
	/*
	 * The rule, stated as the thing that goes wrong without it: a step
	 * finer than a day must NAME THE TIME, or two ticks inside one day
	 * print the same words. The comment beside bbq_ticks_for records
	 * exactly that -- "Tue 11, Tue 11, Wed 12, Wed 12" -- from a
	 * version that chose the format from the span.
	 *
	 * A first draft asserted only that a four-day view contains "ddd",
	 * and PASSED against that bug: the broken format is "ddd d", which
	 * contains it too. Checking for the day was checking the half both
	 * versions agree on.
	 */
	for (qint64 span : {3 * 3600, 12 * 3600, 2 * 24 * 3600,
	                    4 * 24 * 3600, 10 * 24 * 3600}) {
		for (int wanted : {4, 9}) {
			const bbq_tick_choice choice = bbq_ticks_for(span, wanted);
			if (choice.step_s >= 24 * 3600) {
				continue;
			}

			QVERIFY2(choice.format.contains(QStringLiteral("HH")),
			         qPrintable(QStringLiteral("a step of %1 s labels "
			                                   "with \"%2\", so two ticks "
			                                   "in one day read alike")
			                            .arg(choice.step_s)
			                            .arg(choice.format)));
		}
	}
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
	QCOMPARE(desktop.window_after_s, defaults.window_after_s);
	QCOMPARE(desktop.stack_controls, defaults.stack_controls);
}

void test_layout::the_plot_runs_to_the_edge_and_the_controls_do_not() {
	/*
	 * TWO DIFFERENT QUESTIONS THAT LOOKED LIKE ONE (project.md sec 16.7).
	 *
	 * On mobile the root layout runs edge to edge, and that is right for
	 * the plot: a margin either side is lost plot rather than breathing
	 * room, which sec 10.5 argued and this project has measured. The
	 * controls inherited it and they are not plot -- a label starting at
	 * column zero has its first glyph shaved by the screen edge, which
	 * is how "Station:" came to read as "tation:" on a device.
	 *
	 * Asserted as the RELATIONSHIP rather than as either number, so it
	 * survives both being retuned: what must hold is that the controls
	 * are inset where the root layout is not.
	 */
	const bbq_metrics mobile = bbq_metrics_for(bbq_layout::mobile);
	const bbq_metrics desktop = bbq_metrics_for(bbq_layout::desktop);

	QVERIFY2(mobile.stack_controls,
	         "the mobile layout has stopped being the edge-to-edge one");
	QVERIFY2(mobile.control_margin > 0,
	         "the controls sit flush against the screen edge again");

	/*
	 * And the desktop must NOT gain one: there the root layout supplies
	 * the margin already, so a second would be a double indent nobody
	 * asked for.
	 */
	QVERIFY2(!desktop.stack_controls,
	         "the desktop layout has become the stacked one");
	QCOMPARE(desktop.control_margin, 0);
}

QTEST_APPLESS_MAIN(test_layout)
#include "test_layout.moc"
