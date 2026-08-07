#include "graph/forecast_graph.h"

#include <QColor>
#include <QFont>
#include <QPaintEvent>
#include <QPainter>
#include <QRect>

namespace {

/*
 * Wide rather than tall, because the x axis is time and carries days of
 * it (project.md sec 3). A square default would be wrong for every real
 * layout this widget ends up in.
 */
const int default_width  = 720;
const int default_height = 260;

} // namespace

bbq_forecast_graph::bbq_forecast_graph(QWidget *parent) : QWidget(parent) {
	setMinimumSize(320, 140);
}

QSize bbq_forecast_graph::sizeHint() const {
	return QSize(default_width, default_height);
}

void bbq_forecast_graph::paintEvent(QPaintEvent *event) {
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.fillRect(event->rect(), palette().base());

	/*
	 * Said plainly, and said on the surface itself. An empty panel reads
	 * as "loading" or as a bug; this reads as what it is.
	 */
	painter.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));

	QFont message_font = font();
	message_font.setPointSizeF(message_font.pointSizeF() * 1.2);
	painter.setFont(message_font);

	painter.drawText(rect(), Qt::AlignCenter,
	                 tr("No forecast data.\n"
	                    "Nothing is fetched yet -- see project.md sec 2."));
}
