#ifndef BBQ_UI_WIDGET_PICTURE_H
#define BBQ_UI_WIDGET_PICTURE_H

class QWidget;

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
 */
void bbq_write_widget_picture(QWidget *source);

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
