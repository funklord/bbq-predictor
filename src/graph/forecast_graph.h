#ifndef BBQ_FORECAST_GRAPH_H
#define BBQ_FORECAST_GRAPH_H

#include <QColor>
#include <QSize>
#include <QWidget>

#include "model/composite.h"

class QPaintEvent;

/*
 * The colours, gathered in one place on purpose.
 *
 * project.md sec 0 asks for the Weather Underground chart aesthetic, and
 * THIS IS NOT IT -- it is a neutral stand-in derived from the widget
 * palette so the graph is legible in light and dark while the real
 * answer is unknown.
 *
 * It is unknown because it could not be observed. WU renders its chart
 * client-side from lazily-loaded bundles, there is no browser here to
 * look with, and Qt WebEngine on this machine has the runtime but
 * neither the Widgets library nor the headers to build one. Guessing
 * the palette from memory is precisely the move this project refuses
 * everywhere else, so it is not being made here either.
 *
 * Replace these with measured values once somebody can see the real
 * chart. Everything else in the widget is independent of them.
 */
struct bbq_graph_palette {
	QColor background;
	QColor grid;
	QColor axis_text;
	QColor temperature;
	QColor rain;
	QColor now_marker;
	QColor stale_warning;

	/* Per band, for the provenance ribbon (sec 3.4). */
	QColor band_observed;
	QColor band_current;
	QColor band_nowcast;
	QColor band_hourly;
};

/*
 * The temperature and rain graph -- the reason this program exists
 * (project.md sec 3).
 *
 * Draws a bbq_composite across one continuous time axis. What it must
 * get right is written down in sec 3.1 to 3.7 and each rule shows up in
 * the painting:
 *
 *   - bands keep their identity, so the curve is one line while a
 *     ribbon underneath says which band produced each stretch of it
 *     (sec 3.4 -- provenance survives resolution);
 *   - rain downsamples by MAXIMUM per pixel column, never by mean,
 *     because a five-minute downpour meaned into an hour disappears
 *     and that is the signal this program exists to show (sec 3.5);
 *   - nothing is upsampled, and a column no band covers is a break in
 *     the line rather than a segment drawn across it (sec 3.5, 3.6);
 *   - no blending at the seams; a step that survives normalisation is
 *     information and is drawn (sec 3.7).
 *
 * Hand-painted with QPainter rather than QtCharts, which sec 3.8 held
 * as a prior to confirm by trying. See that section for what trying it
 * actually taught.
 */
class bbq_forecast_graph : public QWidget {
	Q_OBJECT

public:
	explicit bbq_forecast_graph(QWidget *parent = nullptr);

	void set_composite(bbq_composite composite);
	const bbq_composite &composite() const { return m_composite; }

	/*
	 * The visible time window, as offsets from now in seconds. The
	 * default looks a little way back and a day forward, which is the
	 * span a question about this afternoon actually needs -- the hourly
	 * band reaches fifteen days and drawing all of it would compress
	 * today into a few pixels.
	 */
	void set_window(qint64 before_s, qint64 after_s);

	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	bbq_graph_palette m_palette;
	bbq_composite m_composite;
	qint64 m_before_s = 3 * 3600;
	qint64 m_after_s = 21 * 3600;
};

#endif
