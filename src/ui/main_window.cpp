#include "ui/main_window.h"

#include <QLabel>
#include <QVBoxLayout>

#include "graph/forecast_graph.h"

bbq_main_window::bbq_main_window(QWidget *parent)
        : QWidget(parent), freshness_label(nullptr), graph(nullptr) {
	setWindowTitle(tr("bbqpredictor"));

	graph = new bbq_forecast_graph(this);

	/*
	 * "Never" is the honest value until a fetch layer exists, and it is
	 * shown rather than left blank for the reason in the header: an empty
	 * freshness line is indistinguishable from a fresh one at a glance.
	 */
	freshness_label = new QLabel(tr("Last updated: never"), this);
	freshness_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(graph, 1);
	layout->addWidget(freshness_label, 0);

	resize(760, 320);
}

void bbq_main_window::toggle_visibility() {
	if (isVisible()) {
		hide();
		return;
	}

	show();
	raise();
	activateWindow();
}
