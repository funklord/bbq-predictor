#ifndef BBQ_FORECAST_GRAPH_H
#define BBQ_FORECAST_GRAPH_H

#include <QSize>
#include <QWidget>

class QPaintEvent;

/*
 * The temperature and rain graph -- the reason this program exists
 * (project.md sec 3).
 *
 * This is a placeholder and paints a message saying so. It draws no curve
 * because there is no data layer yet, and a graph drawn from nothing is
 * precisely the failure sec 2.4 forbids: a display that looks healthy
 * while saying nothing true.
 *
 * What replaces it is a data problem before it is a drawing problem. Three
 * bands share one continuous time axis at three different cadences --
 * observed behind now, sub-hourly nowcast for the next couple of hours,
 * hourly forecast out several days -- and the joins between them must not
 * read as joins. That compositing model is the first real design work and
 * it has not been done.
 *
 * QWidget and QPainter rather than QtCharts, which is a PRIOR and not a
 * finding (sec 3.1): QtCharts is heavy and opinionated about exactly this
 * case, dense custom rendering over irregular sampling. Confirm it by
 * trying it, then replace this paragraph with what was learned.
 */
class bbq_forecast_graph : public QWidget {
	Q_OBJECT

public:
	explicit bbq_forecast_graph(QWidget *parent = nullptr);

	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
};

#endif
