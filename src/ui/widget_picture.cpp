#include "ui/widget_picture.h"

#include <QFile>
#include <QPixmap>
#include <QStandardPaths>
#include <QWidget>

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
 * A HOME-SCREEN SHAPE, WHICH IS NOT THE ON-SCREEN ONE.
 *
 * The graph is as tall as whatever it is sitting in -- 840 by 1047 on a
 * folded Fold, which is portrait. A widget slot is wide and short, so
 * fitting that picture into one leaves most of the widget empty and the
 * graph too small to read, which is the whole point of it lost.
 *
 * Roughly the four-by-two cells the provider asks for. Renders at twice
 * that so it stays sharp when a launcher scales it up.
 */
const int picture_width = 1000;
const int picture_height = 440;

} // namespace

void bbq_write_widget_picture(QWidget *source) {
#ifdef Q_OS_ANDROID
	if (source == nullptr) {
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
	 * Rendered at the widget's shape by resizing for the grab and
	 * putting the size straight back.
	 *
	 * grab() paints into an offscreen pixmap rather than onto the
	 * screen, and the layout restores the real geometry on its next
	 * pass, so nothing of this reaches the display. The alternative was
	 * a second graph object configured to match this one, and that is
	 * twelve setters to keep in step -- a thirteenth added later would
	 * be missed silently, and the widget would quietly stop agreeing
	 * with the window it claims to show.
	 */
	const QSize was = source->size();
	source->resize(picture_width, picture_height);
	const QPixmap picture = source->grab();
	source->resize(was);

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
#endif
}
