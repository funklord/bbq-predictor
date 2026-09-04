#ifndef BBQ_LAYOUT_H
#define BBQ_LAYOUT_H

#include <QString>
#include <QtGlobal>

/*
 * Desktop or mobile (project.md sec 10).
 *
 * Two shapes rather than one that scales, because the differences are
 * not sizes. A phone has no pointer to hover, no tray to sit in, a
 * screen that is tall rather than wide, and a finger instead of a
 * cursor -- so the controls, the time window and the readout each want
 * a different answer, not a bigger one.
 */
enum class bbq_layout {
	desktop,
	mobile,
};

/*
 * What this device is, before any preference is applied.
 *
 * Compiled in rather than measured: a build for Android is for a phone,
 * and asking the screen at runtime would guess wrong on exactly the
 * machines people care about -- a desktop with a touchscreen, a tablet
 * in a keyboard case.
 */
bbq_layout bbq_layout_for_device();

/*
 * The stored preference, which may override the device (sec 10.1).
 * Reads "auto", "desktop" or "mobile"; anything else is auto.
 */
bbq_layout bbq_layout_resolve(const QString &preference);

/*
 * The numbers each shape wants. Gathered rather than scattered so the
 * two layouts can be compared by reading one struct instead of hunting
 * through paint code.
 */
struct bbq_metrics {
	/* Graph chrome. */
	int margin_left = 46;
	int margin_right = 62;
	int margin_bottom = 34;
	int ribbon_height = 5;
	int tick_step_s = 3 * 3600;
	double sample_radius = 2.0;
	double line_width = 2.0;
	double label_scale = 0.85;

	/*
	 * How much of the window the graph shows. A phone has fewer pixels
	 * across, so the same span would be a smear -- less time, drawn
	 * legibly, beats more time drawn thin.
	 */
	qint64 window_before_s = 3 * 3600;
	qint64 window_after_s = 21 * 3600;

	/* Controls. */
	/*
	 * Horizontal breathing room for the CONTROLS, which is not the same
	 * question as the plot's margins above (project.md sec 16.7).
	 *
	 * On mobile the root layout runs edge to edge, because a margin
	 * either side of the graph is lost plot rather than breathing room.
	 * The controls are not plot: a label starting at column zero has
	 * its first glyph shaved by the screen edge, which is how "Station:"
	 * came to read as "tation:". Zero on the desktop, where the root
	 * layout supplies the margin already.
	 */
	int control_margin = 0;

	int control_height = 0;
	bool stack_controls = false;
	bool show_station_field = true;
};

bbq_metrics bbq_metrics_for(bbq_layout layout);

#endif
