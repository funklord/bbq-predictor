#ifndef BBQ_UI_WIDGET_PICTURE_H
#define BBQ_UI_WIDGET_PICTURE_H

#include <QColor>
#include <QSize>

class QString;
class bbq_forecast_graph;

/*
 * Render `source` to the picture the home-screen widget reads, and tell
 * the widget it has changed (project.md sec 16).
 *
 * A no-op off Android, where there is no home screen to put it on.
 *
 * The GRAPH is passed rather than the window: a widget is a glance at
 * the weather, and the station picker and the interpolation drop-down
 * are neither glanceable nor wanted at that size.
 *
 * Writes through a temporary and renames, because the widget host can
 * decode the file at any moment and a half-written PNG decodes to
 * nothing -- which the widget would correctly report as no picture at
 * all, having no way to tell that from a file still being written.
 *
 * `reading` is the number to draw over the graph -- the tray's, from
 * bbq_tray_icon::reading_label, so that the two cannot disagree.
 */
void bbq_write_widget_picture(bbq_forecast_graph *source,
                              const QString &reading);

/*
 * The graph settings a widget render borrows, put back when it leaves
 * (project.md sec 16.35).
 *
 * Four things change for the duration of a render: the graph's size,
 * its opaque ground, its contrast clamp, and the readout parked by
 * whatever the user last touched. All four belong to the window the
 * user is looking at, and the render happens on that live graph every
 * five minutes.
 *
 * A destructor rather than four lines at the end, so a return added
 * later cannot leave the window resized, unclamped or missing the
 * answer to a question somebody had just asked. Available off Android,
 * unlike the render itself, so a test can hold it to that.
 */
class bbq_borrowed_graph {
public:
	explicit bbq_borrowed_graph(bbq_forecast_graph *graph);
	~bbq_borrowed_graph();

	bbq_borrowed_graph(const bbq_borrowed_graph &) = delete;
	bbq_borrowed_graph &operator=(const bbq_borrowed_graph &) = delete;

private:
	bbq_forecast_graph *m_graph = nullptr;
	QSize m_size;
	QColor m_contrast_ground;
	int m_cursor_column = -1;
	bool m_opaque_background = true;
};

/*
 * The translucent ground the picture is filled with, from the graph's
 * own opaque one (project.md sec 16.26).
 *
 * Available off Android, unlike the rendering above, so a test can hold
 * the pair below to the invariant the whole scheme rests on.
 */
QColor bbq_widget_scrim(const QColor &ground);

/*
 * The furthest the scrim can be pushed by a wallpaper: over white when
 * it is dark, over black when it is light. The ground every ink in the
 * picture has to clear.
 *
 * THE INVARIANT, and it is not obvious: this is the worst case ONLY
 * while the scrim stays on one side of every ink it protects. Let the
 * composite pass an ink's luminance and the clamp reverses -- the
 * temperature curve stops being lifted towards pink and starts being
 * pushed towards black -- and the picture changes character at a
 * threshold nobody chose. bbq_widget_scrim_is_bounded says whether it
 * holds.
 */
QColor bbq_widget_worst_ground(const QColor &scrim);

/*
 * Whether `ink` stays on the same side of the worst ground as it is of
 * the scrim itself -- the precondition above, asked about one colour.
 */
bool bbq_widget_scrim_is_bounded(const QColor &scrim, const QColor &ink);

/*
 * Ask Android to fetch periodically even when nothing is on screen
 * (project.md sec 17).
 *
 * A no-op off Android, and idempotent on it: scheduling the same job id
 * replaces rather than stacks, so calling it on every launch is both
 * harmless and the only thing that puts the job back after a reboot.
 */
void bbq_schedule_background_fetch();

#endif
