#include "ui/main_window.h"

#include <QDateTime>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QStringList>
#include <QVBoxLayout>

#include "graph/forecast_graph.h"
#include "graph/interpolate.h"
#include "wu/feed.h"

bbq_main_window::bbq_main_window(QWidget *parent)
        : QWidget(parent), m_method_box(nullptr), freshness_label(nullptr),
          m_graph(nullptr), m_feed(nullptr) {
	setWindowTitle(tr("bbqpredictor"));

	m_graph = new bbq_forecast_graph(this);
	m_feed = new bbq_wu_feed(this);

	freshness_label = new QLabel(this);
	freshness_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	freshness_label->setTextFormat(Qt::PlainText);

	/*
	 * The interpolation controls (project.md sec 3.11).
	 *
	 * A drop-down rather than a setting buried in a dialog, because the
	 * whole question -- which curve tells the truth about this data --
	 * is answered by looking at the graph change while you switch. Sec
	 * 3.8.1 is the precedent: both of that section's defects were found
	 * by looking at a picture, not by reasoning about it.
	 */
	m_method_box = new QComboBox(this);
	QComboBox *method = m_method_box;
	const bbq_interpolation methods[] = {
		bbq_interpolation::step,
		bbq_interpolation::linear,
		bbq_interpolation::monotone,
		bbq_interpolation::natural,
	};

	for (bbq_interpolation candidate : methods) {
		method->addItem(QString::fromLatin1(bbq_interpolation_name(candidate)),
		                static_cast<int>(candidate));
		if (candidate == m_graph->interpolation()) {
			method->setCurrentIndex(method->count() - 1);
		}
	}

	connect(method, &QComboBox::currentIndexChanged, this, [this, method](int) {
		const int value = method->currentData().toInt();
		m_graph->set_interpolation(static_cast<bbq_interpolation>(value));
	});

	QCheckBox *marks = new QCheckBox(tr("Mark samples"), this);
	marks->setChecked(m_graph->show_samples());
	connect(marks, &QCheckBox::toggled, this, [this](bool on) {
		m_graph->set_show_samples(on);
	});

	QHBoxLayout *controls = new QHBoxLayout;
	controls->addWidget(new QLabel(tr("Interpolation:"), this), 0);
	controls->addWidget(method, 0);
	controls->addWidget(marks, 0);
	controls->addStretch(1);
	controls->addWidget(freshness_label, 0);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(m_graph, 1);
	layout->addLayout(controls, 0);

	connect(m_feed, &bbq_wu_feed::updated, this, [this]() {
		m_graph->set_composite(m_feed->composite());
		refresh_status();
	});

	connect(m_feed, &bbq_wu_feed::band_failed, this,
	        [this](const QString &band, const QString &reason) {
		/*
		 * Kept and shown rather than logged and forgotten. A band that
		 * failed is the difference between a graph that is thin and a
		 * graph that is wrong, and sec 2.4 will not have that
		 * difference living only in a terminal nobody is reading.
		 */
		m_last_error = band + QStringLiteral(": ") + reason;
		refresh_status();
	});

	refresh_status();
	resize(820, 360);
}

void bbq_main_window::set_interpolation(bbq_interpolation method) {
	m_graph->set_interpolation(method);

	const int index = m_method_box->findData(static_cast<int>(method));
	if (index >= 0) {
		m_method_box->setCurrentIndex(index);
	}
}

void bbq_main_window::begin(const QString &station_id, const QString &geocode) {
	m_feed->set_station(station_id);

	if (!geocode.isEmpty()) {
		const QStringList parts = geocode.split(QLatin1Char(','));
		if (parts.size() == 2) {
			m_feed->set_geocode(parts.at(0).toDouble(), parts.at(1).toDouble());
		}
	}

	m_feed->refresh();
	m_feed->start_auto_refresh();
}

void bbq_main_window::refresh_status() {
	const bbq_composite &composite = m_feed->composite();
	QString text;

	/*
	 * The OLDEST fetch across the bands, not the newest (sec 2.4). A
	 * display showing the newest goes on looking fresh while one band
	 * quietly stops updating, which is the failure this line exists to
	 * make impossible.
	 */
	const qint64 oldest = composite.oldest_fetch_utc();
	if (oldest == 0) {
		text = tr("Never updated");
	} else {
		const QDateTime when = QDateTime::fromSecsSinceEpoch(oldest);
		text = tr("Oldest band fetched ");
		text += when.toString(QStringLiteral("HH:mm:ss"));
	}

	const std::vector<bbq_band> missing = composite.missing_bands();
	if (!missing.empty()) {
		text += tr("   missing: ");
		for (bbq_band band : missing) {
			text += QString::fromLatin1(bbq_band_name(band));
			text += QStringLiteral(" ");
		}
	}

	if (!m_last_error.isEmpty()) {
		text += tr("   last error: ");
		text += m_last_error;
	}

	freshness_label->setText(text);
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
