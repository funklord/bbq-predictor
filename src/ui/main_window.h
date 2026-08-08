#ifndef BBQ_MAIN_WINDOW_H
#define BBQ_MAIN_WINDOW_H

#include <QList>
#include <QWidget>

#include "graph/interpolate.h"
#include "ui/layout.h"

class QComboBox;
class QLabel;
class QLineEdit;
class bbq_forecast_graph;
class bbq_wu_feed;

/*
 * The window half of the applet (project.md sec 0). The tray is the other
 * half and is bbq_tray_icon.
 *
 * Deliberately thin: the graph is the content, and everything around it
 * exists to say how old that content is. See freshness_label below -- sec
 * 2.4 makes that a requirement rather than a decoration.
 */
class bbq_main_window : public QWidget {
	Q_OBJECT

public:
	explicit bbq_main_window(QWidget *parent = nullptr);

	/* Configure and start fetching. Empty station is a normal state. */
	/*
	 * Start fetching. Empty arguments mean "use what is configured",
	 * so the applet run with no arguments at all is the normal case
	 * rather than the broken one.
	 */
	void begin(const QString &station_id, const QString &geocode);

	bbq_wu_feed *feed() const { return m_feed; }
	bbq_forecast_graph *graph() const { return m_graph; }

	/* What the window says above the graph, for the tray to echo. */
	QString verdict() const;

	/*
	 * Set the curve and keep the drop-down agreeing with it.
	 *
	 * Exists because the two came apart: a command-line override went
	 * straight to the graph, so every rendered comparison showed a
	 * control naming a method the graph was not using. A widget that
	 * misreports the state it controls is worse than no widget.
	 */
	void set_interpolation(bbq_interpolation method);

	/* Same contract: sets the graph AND the control that reports it. */
	void set_smoothing(int seconds);

	/* Re-shape for a device kind, controls and graph together (sec 10). */
	void set_layout(bbq_layout layout);

public slots:
	/*
	 * Show and raise, or hide if already visible. What the tray icon calls
	 * when it is clicked.
	 */
	void toggle_visibility();

private:
	/*
	 * When the data was last successfully fetched, on the surface rather
	 * than in a tooltip.
	 *
	 * project.md sec 2.4: the key is scraped and WILL stop working, and
	 * the failure mode is not an error -- it is a graph that keeps drawing
	 * yesterday's curve while looking perfectly healthy. A stale graph
	 * that looks fresh is worse than no graph, because somebody plans an
	 * afternoon on it. So the age of the data is part of the display, and
	 * a silent fall back to cache is a defect.
	 */
	void refresh_status();

	QComboBox *m_method_box;
	QComboBox *m_smoothing_box;
	QComboBox *m_layout_box;
	QWidget *m_controls;

	/*
	 * The controls in order, so the shape can be rebuilt rather than
	 * merely restyled. Re-flowing needs the widgets; keeping them here
	 * is what lets one row become two columns without constructing
	 * anything twice.
	 */
	QList<QWidget *> m_control_items;
	QLineEdit *m_station_box;
	QLabel *m_verdict;
	QLabel *freshness_label;
	bbq_forecast_graph *m_graph;
	bbq_wu_feed *m_feed;
	QString m_last_error;
};

#endif
