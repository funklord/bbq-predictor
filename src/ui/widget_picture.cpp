#include "ui/widget_picture.h"

#include <QFile>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QString>

#include "graph/forecast_graph.h"

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
 * How much of the picture's height the number takes. A quarter reads
 * from arm's length on a phone and still leaves the curve the room that
 * makes it a graph rather than a decoration.
 */
const double reading_share = 0.25;

#ifdef Q_OS_ANDROID
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
#endif

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
	font.setPixelSize(qMax(10, int(size.height() * reading_share)));

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

} // namespace

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
	const QSize was = source->size();
	const bool was_opaque = source->opaque_background();

	source->set_opaque_background(false);
	source->resize(shape);

	/*
	 * Rendered at the device's pixel ratio so the file is at the
	 * widget's real pixel size, and filled transparent so the ground is
	 * the wallpaper's rather than the theme's. render() draws what
	 * paintEvent draws, and with the ground turned off paintEvent draws
	 * no rectangle -- so what is not a curve, a band or a label keeps
	 * the fill this image was created with.
	 */
	const double ratio = source->devicePixelRatioF();
	QImage picture(shape * ratio, QImage::Format_ARGB32_Premultiplied);
	picture.setDevicePixelRatio(ratio);
	picture.fill(Qt::transparent);

	source->render(&picture, QPoint(), QRegion(), QWidget::DrawChildren);

	source->resize(was);
	source->set_opaque_background(was_opaque);

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
