#ifndef BBQ_UI_WIDGET_PICTURE_H
#define BBQ_UI_WIDGET_PICTURE_H

#include <QColor>
#include <QSize>
#include <QtGlobal>

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
/*
 * Everything about the picture that is not Android's to answer, so that
 * a test can produce one without a device (sec 16.119). Returns whether
 * the file landed.
 */
bool bbq_render_widget_picture(bbq_forecast_graph *source,
                               const QString &reading, const QSize &shape,
                               const QString &path);

void bbq_write_widget_picture(bbq_forecast_graph *source,
                              const QString &reading);

/*
 * The graph settings a widget render borrows, put back when it leaves
 * (project.md sec 16.35).
 *
 * FIVE things change for the duration of a render: the graph's size,
 * its opaque ground, its contrast clamp, the readout parked by whatever
 * the user last touched, and the view they panned to. All five belong
 * to the window the user is looking at, and the render happens on that
 * live graph every five minutes.
 *
 * The view was the fifth and arrived late. bbq_pose_graph_for_picture
 * started calling follow_now() and this restored four things -- so a
 * render would have snapped a panned window back to the present under
 * the user's hand, every five minutes, which is a worse fault than the
 * one that change was fixing.
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
	qint64 m_view_from = 0;
	qint64 m_view_span_s = 0;
	int m_cursor_column = -1;
	bool m_opaque_background = true;
	bool m_following_now = true;
};

/*
 * Put the graph into the state a home-screen picture is drawn from
 * (project.md sec 16.36).
 *
 * Five things, and every one of them is a difference between what a
 * window is for and what a widget is for:
 *
 *   the ground     off, so the scrim carries it
 *   the clamp      on, against a ground this program did not choose
 *   the readout    cleared; a home screen has no cursor
 *   the view       back to now; a widget is a glance at the present
 *   the size       the widget's box
 *
 * Available off Android, unlike the render, so the pose can be tested
 * where the JNI cannot go. That is deliberate: the readout fix
 * (sec 16.35) left its clearing reachable only by screenshot, and this
 * is the shape that does not.
 *
 * Pair it with bbq_borrowed_graph, which puts it all back.
 */
void bbq_pose_graph_for_picture(bbq_forecast_graph *graph,
                                const QColor &ground, double floor,
                                const QSize &shape);

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
