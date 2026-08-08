#include "ui/tray_icon.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QIcon>
#include <QMenu>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QPixmap>

#include <QDateTime>

#include "model/grill.h"

bbq_tray_icon::bbq_tray_icon(QObject *parent)
        : QSystemTrayIcon(parent), menu(nullptr) {
	setIcon(placeholder_icon());

	/*
	 * The tooltip carries the freshness too, for the same reason the
	 * window does (project.md sec 2.4). The tray is where this program is
	 * glanced at rather than read, so it is the likelier place for a stale
	 * reading to be believed.
	 */
	setToolTip(tr("bbqpredictor -- no data yet"));

	menu = new QMenu();

	QAction *show_action = menu->addAction(tr("Show / hide"));
	connect(show_action, &QAction::triggered,
	        this, &bbq_tray_icon::toggle_requested);

	menu->addSeparator();

	QAction *quit_action = menu->addAction(tr("Quit"));
	connect(quit_action, &QAction::triggered, qApp, &QApplication::quit);

	setContextMenu(menu);

	/*
	 * Only a plain click toggles. A context-menu request must not, or the
	 * menu appears and the window flaps at the same time.
	 */
	connect(this, &QSystemTrayIcon::activated, this,
	        [this](QSystemTrayIcon::ActivationReason reason) {
		if (reason == QSystemTrayIcon::Trigger ||
		    reason == QSystemTrayIcon::DoubleClick) {
			emit toggle_requested();
		}
	});
}

namespace {

/*
 * How old the oldest band may get before the tray says so.
 *
 * Two hours, which is two missed hourly refreshes -- the slowest band
 * legitimately reaches an hour old between fetches (sec 2.5.1), so
 * anything tighter would cry stale on a working applet. This is the
 * tray's half of sec 2.4: the failure that matters is not an error
 * dialog, it is a graph that keeps looking healthy.
 */
const qint64 stale_after_s = 2 * 60 * 60;

const QColor ink_fresh(0x1e, 0x1e, 0x1e);
const QColor ink_stale(0xd5, 0x20, 0x2a);

} // namespace

void bbq_tray_icon::show_state(const bbq_composite &composite,
                               const QString &verdict) {
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	const qint64 oldest = composite.oldest_fetch_utc();
	const bool stale = oldest == 0 || (now - oldest) > stale_after_s;

	const bbq_reading reading = composite.at(now);

	QString label = QStringLiteral("--");
	QString detail;

	if (reading.is_valid() && reading.sample->temperature.has_value()) {
		label = QString::number(*reading.sample->temperature, 'f', 0);
		detail = QString::number(*reading.sample->temperature, 'f', 1);
		detail += QStringLiteral(" C from ");
		detail += QString::fromLatin1(bbq_band_name(reading.series->band()));
	} else {
		detail = tr("No reading for now");
	}

	setIcon(reading_icon(label, stale ? ink_stale : ink_fresh));

	/*
	 * The tooltip carries what the icon cannot: how old the reading is,
	 * and the answer the program is named after. Stale is stated in
	 * words as well as colour, because a colour alone is a claim only
	 * somebody who already knows the convention can read.
	 */
	QString tip = QStringLiteral("bbqpredictor\n");
	tip += detail;
	tip += QStringLiteral("\n");

	if (oldest == 0) {
		tip += tr("Never updated");
	} else {
		const qint64 age = (now - oldest) / 60;
		tip += tr("Oldest band ");
		tip += QString::number(age);
		tip += tr(" min old");
		if (stale) {
			tip += tr("  -- STALE");
		}
	}

	if (!verdict.isEmpty()) {
		tip += QStringLiteral("\n");
		tip += verdict;
	}

	setToolTip(tip);
}

QIcon bbq_tray_icon::reading_icon(const QString &text, const QColor &ink) {
	QIcon icon;

	/*
	 * Two sizes rather than one scaled: a 22 pixel icon scaled up to 44
	 * is a blurred number, and the number is the whole content.
	 */
	const int sizes[] = {22, 44};

	for (int size : sizes) {
		QPixmap pixmap(size, size);
		pixmap.fill(Qt::transparent);

		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setRenderHint(QPainter::TextAntialiasing);

		/*
		 * Sized to FIT rather than to a fraction guessed at.
		 *
		 * The first version set the pixel size to the icon's height,
		 * which is the height of the em box and not of the digits, so
		 * "20" rendered a size too large and lost its top and bottom
		 * to the edges. Measuring is a few lines and cannot be wrong
		 * about a font it has not seen.
		 */
		QFont font = painter.font();
		font.setBold(true);

		int points = size;
		while (points > 6) {
			font.setPixelSize(points);
			const QFontMetrics metrics(font);
			const QRect bounds = metrics.tightBoundingRect(text);
			if (bounds.width() <= size && bounds.height() <= size) {
				break;
			}
			--points;
		}

		painter.setFont(font);
		painter.setPen(ink);
		painter.drawText(pixmap.rect(), Qt::AlignCenter, text);
		painter.end();

		icon.addPixmap(pixmap);
	}

	return icon;
}

bool bbq_tray_icon::is_available() {
	return QSystemTrayIcon::isSystemTrayAvailable();
}

QIcon bbq_tray_icon::placeholder_icon() {
	/*
	 * 22px is the conventional tray size on the desktops that have one.
	 * Drawn into a transparent pixmap so it sits on a panel of any colour.
	 */
	QPixmap pixmap(22, 22);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(QColor(210, 210, 210), 2));
	painter.setBrush(QBrush(QColor(190, 60, 40)));
	painter.drawEllipse(3, 3, 16, 16);

	return QIcon(pixmap);
}
