#include "ui/layout.h"

#include <QtGlobal>

bbq_layout bbq_layout_for_device() {
	/*
	 * Compiled in. A build for Android is a build for a phone, and the
	 * alternative -- asking the screen its size at runtime -- guesses
	 * wrong on the machines people actually notice: a desktop with a
	 * touchscreen, a tablet in a keyboard case, a phone plugged into a
	 * monitor.
	 *
	 * Where the guess would be wrong the preference is the answer
	 * (sec 10.1), and a preference somebody set is better evidence than
	 * a pixel count.
	 */
#ifdef Q_OS_ANDROID
	return bbq_layout::mobile;
#else
	return bbq_layout::desktop;
#endif
}

bbq_layout bbq_layout_resolve(const QString &preference) {
	if (preference == QStringLiteral("desktop")) {
		return bbq_layout::desktop;
	}

	if (preference == QStringLiteral("mobile")) {
		return bbq_layout::mobile;
	}

	return bbq_layout_for_device();
}

bbq_metrics bbq_metrics_for(bbq_layout layout) {
	bbq_metrics metrics;

	if (layout == bbq_layout::desktop) {
		return metrics;
	}

	/*
	 * Mobile. Every number here has a reason that is not "bigger":
	 *
	 * The axis gutters shrink because a phone has few pixels across and
	 * the plot is what people came for -- but not to nothing, since the
	 * temperature and rain scales still have to be readable.
	 *
	 * The tick step doubles because three-hourly labels at this width
	 * collide, and a collided label is worse than a missing one.
	 *
	 * The window narrows to half a day. The same span on a narrower
	 * screen is the same data drawn thinner, and this graph's whole
	 * claim is resolution.
	 *
	 * The marks and the line grow because a finger is not a cursor and
	 * a phone is held further from the eye than a monitor sits from it.
	 */
	metrics.margin_left = 34;
	/*
	 * 40 was too tight and clipped "1.0 mm/h" to "1.0 mm" -- a units
	 * label losing its units, which is the one part of it that carries
	 * information. Measured against the longest string it has to hold
	 * rather than chosen to look narrow.
	 */
	metrics.margin_right = 54;
	metrics.margin_bottom = 30;
	metrics.ribbon_height = 7;
	metrics.tick_step_s = 6 * 3600;
	metrics.sample_radius = 3.0;
	metrics.line_width = 2.6;
	metrics.label_scale = 1.0;

	metrics.window_before_s = 2 * 3600;
	metrics.window_after_s = 10 * 3600;

	/*
	 * Controls stack, and are tall enough to hit. A row that fits a
	 * desktop window becomes six things two millimetres wide on a
	 * phone, which is a row nobody can use.
	 */
	metrics.control_height = 44;
	metrics.stack_controls = true;

	return metrics;
}
