#ifndef BBQ_MAIN_WINDOW_H
#define BBQ_MAIN_WINDOW_H

#include <QWidget>

class QLabel;
class bbq_forecast_graph;

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
	QLabel *freshness_label;
	bbq_forecast_graph *graph;
};

#endif
