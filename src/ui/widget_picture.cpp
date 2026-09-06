#include "ui/widget_picture.h"

#include <QFile>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QString>

#include "graph/forecast_graph.h"
#include "ui/theme.h"

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#endif

namespace {

/*
 * The name is agreed with GraphWidget.java, which looks for it in
 * getFilesDir(). Qt's AppDataLocation is that same directory on
 * Android. If the two ever disagree the widget shows its empty state
 * rather than a wrong picture, which is the right way for that mistake
 * to fail.
 */
const char *const picture_name = "/widget.png";

/*
 * WHAT TO DRAW WHEN ANDROID WILL NOT SAY HOW BIG THE WIDGET IS.
 *
 * A fallback, and it used to be the whole answer: 1000 by 440, chosen on
 * the reasoning that a launcher would scale the picture UP and it had
 * better stay sharp. Measured on the device, this launcher scales it
 * DOWN -- the placed widget is 440 by 195 physical pixels on the cover
 * screen, so every glyph was minified by about two and a third and the
 * readout, the caption and the axis labels were smears (sec 16.23).
 *
 * The size is asked for now, and this is what is left when the question
 * cannot be answered. Roughly the four-by-two cells the provider asks
 * for, at a dp size a phone will not have to shrink much.
 */
const int fallback_width = 440;
const int fallback_height = 190;

/*
 * Bounds on what Android is believed about.
 *
 * A widget host reporting nonsense -- zero, or a number that would have
 * this render a picture the size of a wall -- gets the fallback rather
 * than a crash or a 200 MB image. The floor is smaller than any home
 * screen cell and the ceiling is larger than a tablet's whole screen,
 * so a real answer is never refused.
 */
const int smallest_dp = 60;
const int largest_dp = 2000;

/*
 * HOW MUCH WALLPAPER SHOWS THROUGH, AND WHY IT IS A NUMBER RATHER THAN
 * NONE (sec 16.25).
 *
 * A fully transparent picture reads well on a plain wallpaper and badly
 * on a photograph: the curve competes with whatever is behind it, and
 * the middle of the plot was the worst of it. A fully opaque one is a
 * slab on somebody's home screen.
 *
 * A scrim of the theme's own ground is the trade, and it buys more than
 * a look. It BOUNDS the ground: whatever the wallpaper is, the composite
 * lies between the scrim over black and the scrim over white, and the
 * second of those is the worst case for a dark scrim. That is a ground
 * this program can name, which is the precondition harmonization.md sets
 * before a program may draw onto one it does not own.
 *
 * 0.75, so a quarter of the wallpaper carries. It started at 0.85 and
 * the sweep in sec 16.26 is why it did not go further: the clamp still
 * reaches the floor at every alpha down to 0.5, so "does it stay
 * legible" is the wrong question and stops discriminating. What moves
 * is how far the colours have to travel to get there -- Weather
 * Underground's red is #e55058 at 0.85, #ec8187 at 0.75 and #f3aeb2 at
 * 0.65, which is no longer a red anybody measured.
 */
const double scrim_alpha = 0.75;

/*
 * The floor the clamp holds the inks to, and it is the palette gate's,
 * deliberately. A second number here would be a second answer to "how
 * legible is legible".
 */
const double contrast_floor = 3.0;

#ifdef Q_OS_ANDROID

/*
 * Everything from here to the end of the namespace draws the number,
 * and only the Android half of this file calls it. Guarded rather than
 * left for the linker to drop: an unused static is a warning on the
 * desktop build, and a warning nobody can act on is one everybody
 * learns to scroll past.
 */

/*
 * The number over the graph, and the ink it is drawn in.
 *
 * Outlined for the reason the tray's is (sec 4.3), and the reason is
 * stronger here: the tray sits on a panel whose colour is merely
 * unknown, while this is drawn onto a transparent ground and lands on
 * whatever wallpaper somebody chose. A light halo under a dark fill
 * needs no guess -- the halo carries the contrast on a dark wallpaper
 * and the fill carries it on a light one.
 *
 * The two colours are the tray's, deliberately, so the number in the
 * notification area and the number on the home screen look like the
 * same number.
 */
const QColor reading_ink(0x1e, 0x1e, 0x1e);
const QColor reading_halo(0xf2, 0xf2, 0xf2);

/*
 * How big the number is, as shares of the picture -- WHICHEVER IS
 * SMALLER (sec 16.24.2).
 *
 * A quarter of the height alone was the first rule, tuned on a strip
 * one cell tall where it reads from arm's length. Dragged to four cells
 * the same rule gave a numeral 108 dp tall sitting across the curve: it
 * had been a share of the only dimension that varied, and then the
 * other one varied.
 *
 * A tenth of the width is what holds it. On the strip the height rule
 * still decides and nothing changes; on a tall block the width rule
 * takes over and the number stays a glance rather than a poster. Both
 * scale, so a tablet's widget gets a bigger one rather than a constant
 * somebody would have to revisit.
 */
const double reading_height_share = 0.25;
const double reading_width_share = 0.10;

/*
 * How big the placed widget actually is, in dp, or 0 for "Android did
 * not say". GraphWidget.java explains why dp rather than pixels.
 */
int asked_size(const char *method) {
	QJniObject context = QNativeInterface::QAndroidApplication::context();
	if (!context.isValid()) {
		return 0;
	}

	const jint size = QJniObject::callStaticMethod<jint>(
	        "se/vibes/bbq_predictor/GraphWidget", method,
	        "(Landroid/content/Context;)I", context.object());

	return size >= smallest_dp && size <= largest_dp ? size : 0;
}

/*
 * Draw the current temperature across the top of the picture.
 *
 * CENTRED, and the corners were tried first. Top right put it straight
 * over the rain gutter's "10 mm" plate and top left over the
 * temperature axis's "22 C" -- both measured on the device, both
 * unreadable where the two overlapped.
 *
 * The centre is the one place that is free whatever the weather does.
 * The axis labels are pinned to the corners by the graph's own layout
 * and the curve cannot reach the top edge, because the scale keeps
 * headroom above its warmest sample; what is left along the top is tick
 * marks. A corner that happens to be empty in today's data is not a
 * free corner, it is a collision waiting for a warmer afternoon.
 */
void draw_reading(QPainter &painter, const QSize &size, const QString &text) {
	if (text.isEmpty()) {
		return;
	}

	QFont font = painter.font();
	font.setBold(true);
	font.setPixelSize(qMax(10, int(qMin(size.height() * reading_height_share,
	                                    size.width() * reading_width_share))));

	/*
	 * Placed on the INK box rather than the em box, for the reason the
	 * tray gives: the line box includes ascent and descent the digits
	 * do not use, so a path placed by it sits visibly high.
	 */
	const QFontMetrics metrics(font);
	const QRect tight = metrics.tightBoundingRect(text);

	const double halo_width = qMax(1.0, font.pixelSize() / 12.0);
	const double margin = halo_width + size.height() / 30.0;

	const double left = (size.width() - tight.width()) / 2.0 - tight.left();
	const double baseline = margin - tight.top();

	QPainterPath glyphs;
	glyphs.addText(QPointF(left, baseline), font, text);

	/*
	 * Stroked first and filled over the top. A stroke is centred on the
	 * outline, so half of it falls inside the glyph -- filling
	 * afterwards puts the weight back and keeps the digits the shape
	 * the font drew them.
	 */
	painter.setPen(QPen(reading_halo, halo_width, Qt::SolidLine, Qt::RoundCap,
	                    Qt::RoundJoin));
	painter.setBrush(Qt::NoBrush);
	painter.drawPath(glyphs);

	painter.fillPath(glyphs, reading_ink);
}

#endif

} // namespace

bbq_borrowed_graph::bbq_borrowed_graph(bbq_forecast_graph *graph)
        : m_graph(graph) {
	if (m_graph == nullptr) {
		return;
	}

	m_size = m_graph->size();
	m_contrast_ground = m_graph->contrast_ground();
	m_cursor_column = m_graph->cursor_column();
	m_opaque_background = m_graph->opaque_background();

	m_following_now = m_graph->is_following_now();
	m_view_from = m_graph->view_from_utc();
	m_view_span_s = m_graph->view_span_s();
}

bbq_borrowed_graph::~bbq_borrowed_graph() {
	if (m_graph == nullptr) {
		return;
	}

	m_graph->resize(m_size);
	m_graph->set_opaque_background(m_opaque_background);
	m_graph->set_contrast_ground(m_contrast_ground, contrast_floor);
	m_graph->set_cursor_column(m_cursor_column);

	/*
	 * FOLLOWING IS NOT A VIEW, so it cannot be restored as one.
	 * set_view() pins the range and stops the graph tracking the clock,
	 * which for a window that was following would silently freeze it at
	 * whatever second the render happened.
	 */
	if (m_following_now) {
		m_graph->follow_now();
	} else {
		m_graph->set_view(m_view_from, m_view_span_s);
	}
}

void bbq_pose_graph_for_picture(bbq_forecast_graph *graph,
                                const QColor &ground, double floor,
                                const QSize &shape) {
	if (graph == nullptr) {
		return;
	}

	graph->set_opaque_background(false);
	graph->set_contrast_ground(ground, floor);

	/*
	 * NO PARKED READOUT (sec 16.35).
	 *
	 * The readout follows a cursor, and on a phone a drag leaves it
	 * parked where the finger stopped -- deliberately, so a touch can
	 * read a value at all. On the home screen there is no question and
	 * no cursor, so it is a stale sentence over the number the widget
	 * exists to show.
	 */
	graph->set_cursor_column(-1);

	/*
	 * AND BACK TO NOW (sec 16.36).
	 *
	 * The same fault as the readout, one field along and worse. The
	 * view belongs to whoever last dragged the graph, and the render
	 * never touched it -- so a window left panned at last Tuesday put
	 * last Tuesday on the home screen, under a current temperature
	 * drawn from the composite at now and beside a now-marker that had
	 * gone off the edge. Every part of that picture is correct and the
	 * picture is a lie.
	 *
	 * A widget is a glance at the present. The user's view is put back
	 * by bbq_borrowed_graph the moment the render is done, so the
	 * window they are looking at does not move under them.
	 */
	graph->follow_now();

	graph->resize(shape);
}

QColor bbq_widget_scrim(const QColor &ground) {
	QColor scrim = ground;
	scrim.setAlphaF(scrim_alpha);
	return scrim;
}

QColor bbq_widget_worst_ground(const QColor &scrim) {
	/*
	 * A light theme inverts it, and the expression follows the scrim
	 * rather than assuming: over black when the scrim is light.
	 */
	const bool scrim_is_dark = bbq_relative_luminance(scrim) < 0.5;
	const int wallpaper = scrim_is_dark ? 255 : 0;

	/*
	 * THE SCRIM'S OWN ALPHA, not the constant above.
	 *
	 * It read scrim_alpha, which made this correct for the one scrim
	 * bbq_widget_scrim builds and a lie about every other -- so it
	 * answered "bounded" for a scrim thin enough to be plainly
	 * unbounded. Found by the control in
	 * a_scrim_light_enough_to_pass_an_ink_is_reported_unbounded, which
	 * is the whole reason that test constructs a scrim rather than
	 * reusing the real one: a check exercised only on the value it was
	 * written for cannot notice that it ignores its argument.
	 */
	const double alpha = scrim.alphaF();

	/*
	 * Rounded, not truncated. Truncation shifts the composite towards
	 * black by up to one level, which for a dark scrim makes the
	 * "worst" ground very slightly better than the real worst -- a bound
	 * that is not quite a bound. It is a fraction of a level and it
	 * would never be visible; it would also be wrong in the one
	 * direction a bound must never be wrong in.
	 */
	const auto mix = [&](int channel) {
		return qRound(alpha * channel + (1.0 - alpha) * wallpaper);
	};

	return QColor(mix(scrim.red()), mix(scrim.green()), mix(scrim.blue()));
}

bool bbq_widget_scrim_is_bounded(const QColor &scrim, const QColor &ink) {
	const double at_scrim = bbq_relative_luminance(scrim);
	const double at_worst = bbq_relative_luminance(bbq_widget_worst_ground(scrim));
	const double of_ink = bbq_relative_luminance(ink);

	/*
	 * The ink must be on the same side of both, so no wallpaper can put
	 * the ground between the two and reverse which way the clamp walks.
	 */
	return (of_ink > at_scrim && of_ink > at_worst) ||
	       (of_ink < at_scrim && of_ink < at_worst);
}

void bbq_write_widget_picture(bbq_forecast_graph *source,
                              const QString &reading) {
#ifdef Q_OS_ANDROID
	if (source == nullptr) {
		return;
	}

	/*
	 * NOTHING TO DRAW FOR (sec 16.21).
	 *
	 * Rendering the graph and writing 120 kB is not free, and it
	 * happened on every fetch whether or not anybody had put a widget on
	 * a home screen. The Java side has always declined to broadcast in
	 * that case, which saved the broadcast and none of the work.
	 *
	 * Asked first now. A reader who adds the widget later gets its empty
	 * state until the next fetch, which is a few minutes and is what the
	 * empty state is for.
	 */
	QJniObject placed_context =
	        QNativeInterface::QAndroidApplication::context();

	/*
	 * An invalid context is the question failing rather than a "no", so
	 * it draws. Only a definite answer of false skips, for the reason
	 * anyPlaced states on its own side.
	 */
	if (placed_context.isValid() &&
	    !QJniObject::callStaticMethod<jboolean>(
	            "se/vibes/bbq_predictor/GraphWidget", "anyPlaced",
	            "(Landroid/content/Context;)Z", placed_context.object())) {
		return;
	}

	const QString directory =
	        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if (directory.isEmpty()) {
		return;
	}

	const QString path = directory + QString::fromLatin1(picture_name);
	const QString partial = path + QStringLiteral(".part");

	/*
	 * THE WIDGET'S OWN SHAPE, ASKED FOR RATHER THAN GUESSED.
	 *
	 * Both or neither: a width from Android and a fallback height would
	 * be an aspect ratio nobody chose, and the launcher would letterbox
	 * or crop it. Either the host answered about this widget or it did
	 * not.
	 */
	const int asked_width = asked_size("wantedWidth");
	const int asked_height = asked_size("wantedHeight");
	const bool answered = asked_width > 0 && asked_height > 0;

	const QSize shape(answered ? asked_width : fallback_width,
	                  answered ? asked_height : fallback_height);

	/*
	 * Rendered at the widget's shape by resizing for the render and
	 * putting the size straight back.
	 *
	 * render() paints into an image rather than onto the screen, and
	 * the layout restores the real geometry on its next pass, so
	 * nothing of this reaches the display. The alternative was a second
	 * graph object configured to match this one, and that is twelve
	 * setters to keep in step -- a thirteenth added later would be
	 * missed silently, and the widget would quietly stop agreeing with
	 * the window it claims to show.
	 */
	const bbq_borrowed_graph borrowed(source);

	/*
	 * The scrim is the graph's own ground, made translucent, so the
	 * picture is the window's colours seen through the wallpaper rather
	 * than a second scheme nobody set.
	 */
	const QColor scrim = bbq_widget_scrim(source->palette_colours().background);
	const QColor ground = bbq_widget_worst_ground(scrim);

	bbq_pose_graph_for_picture(source, ground, contrast_floor, shape);

	/*
	 * Rendered at the device's pixel ratio so the file is at the
	 * widget's real pixel size, and filled with the scrim so the ground
	 * is bounded rather than unknown. render() draws what
	 * paintEvent draws, and with the ground turned off paintEvent draws
	 * no rectangle -- so what is not a curve, a band or a label keeps
	 * the fill this image was created with.
	 */
	const double ratio = source->devicePixelRatioF();
	QImage picture(shape * ratio, QImage::Format_ARGB32_Premultiplied);
	picture.setDevicePixelRatio(ratio);
	picture.fill(scrim);

	source->render(&picture, QPoint(), QRegion(), QWidget::DrawChildren);


	/*
	 * The number over the top, after the render rather than inside it:
	 * the graph draws the weather and knows nothing about home screens,
	 * and a widget-only overlay inside paintEvent would be a mode the
	 * on-screen graph carried and never used.
	 */
	QPainter painter(&picture);
	painter.setRenderHint(QPainter::Antialiasing);
	draw_reading(painter, shape, reading);
	painter.end();

	if (picture.isNull() || !picture.save(partial, "PNG")) {
		QFile::remove(partial);
		return;
	}

	/*
	 * rename() will not replace an existing file, so the old one goes
	 * first. The window between them is real and is the reason the
	 * widget treats an undecodable file as "no picture" rather than as
	 * an error worth saying anything about.
	 */
	QFile::remove(path);
	if (!QFile::rename(partial, path)) {
		QFile::remove(partial);
		return;
	}

	/*
	 * Tell the widget. Without this it changes only when Android asks,
	 * which is at most every thirty minutes and not at all while the
	 * device is idle -- so the picture would routinely be older than
	 * the one sitting on disk beside it.
	 */
	QJniObject context = QNativeInterface::QAndroidApplication::context();
	if (context.isValid()) {
		QJniObject::callStaticMethod<void>(
		        "se/vibes/bbq_predictor/GraphWidget", "refresh",
		        "(Landroid/content/Context;)V", context.object());
	}
#else
	Q_UNUSED(source);
	Q_UNUSED(reading);
#endif
}

void bbq_schedule_background_fetch() {
#ifdef Q_OS_ANDROID
	QJniObject context = QNativeInterface::QAndroidApplication::context();
	if (!context.isValid()) {
		return;
	}

	QJniObject::callStaticMethod<void>(
	        "se/vibes/bbq_predictor/FetchJobService", "schedule",
	        "(Landroid/content/Context;)V", context.object());
#endif
}
