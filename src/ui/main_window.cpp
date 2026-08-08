#include "ui/main_window.h"

#include <QDateTime>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStringList>
#include <QVBoxLayout>

#include "graph/forecast_graph.h"
#include "graph/interpolate.h"
#include "model/grill.h"
#include "ui/layout.h"
#include "model/settings.h"
#include "wu/feed.h"

bbq_main_window::bbq_main_window(QWidget *parent)
        : QWidget(parent), m_method_box(nullptr), m_smoothing_box(nullptr),
          m_layout_box(nullptr), m_controls(nullptr),
          m_station_box(nullptr), m_verdict(nullptr), freshness_label(nullptr),
          m_graph(nullptr), m_feed(nullptr) {
	setWindowTitle(tr("bbq-predictor"));

	m_graph = new bbq_forecast_graph(this);

	/*
	 * Remembered presentation, applied before the controls read the
	 * graph so both start from the same answer (sec 2.6.6).
	 */
	const int fallback_method = static_cast<int>(m_graph->interpolation());
	const int stored_method = bbq_settings::interpolation(fallback_method);
	m_graph->set_interpolation(static_cast<bbq_interpolation>(stored_method));
	m_graph->set_smoothing(bbq_settings::smoothing(m_graph->smoothing()));
	m_feed = new bbq_wu_feed(this);

	/*
	 * The answer to the question the program is named after, in words,
	 * because a shaded band on a graph says WHEN but not HOW GOOD and
	 * the difference between a fine evening and a merely tolerable one
	 * is the whole point of asking.
	 */
	m_verdict = new QLabel(this);
	m_verdict->setTextFormat(Qt::PlainText);

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
		bbq_interpolation::akima,
		bbq_interpolation::makima,
		bbq_interpolation::natural,
		bbq_interpolation::catmull,
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
		bbq_settings::set_interpolation(value);
	});

	/*
	 * Rounding, in time. Expressed as a duration because that is what
	 * it means -- how wide a corner is allowed to be -- rather than as
	 * an abstract strength nobody can reason about.
	 */
	m_smoothing_box = new QComboBox(this);
	QComboBox *smoothing = m_smoothing_box;
	smoothing->addItem(tr("Off"), 0);
	smoothing->addItem(tr("15 min"), 15 * 60);
	smoothing->addItem(tr("30 min"), 30 * 60);
	smoothing->addItem(tr("1 h"), 60 * 60);
	smoothing->addItem(tr("2 h"), 2 * 60 * 60);

	/*
	 * Selected from the graph rather than left on the first item.
	 *
	 * Both drop-downs now start from what the graph is actually doing.
	 * The alternative had already bitten twice -- a control showing one
	 * thing over a picture doing another -- and defaulting the graph to
	 * anything but the first entry would have made it three.
	 */
	const int initial = smoothing->findData(m_graph->smoothing());
	if (initial >= 0) {
		smoothing->setCurrentIndex(initial);
	}

	connect(smoothing, &QComboBox::currentIndexChanged, this,
	        [this, smoothing](int) {
		const int seconds = smoothing->currentData().toInt();
		m_graph->set_smoothing(seconds);
		bbq_settings::set_smoothing(seconds);
	});

	/*
	 * The station, editable here rather than only in a file. A tray
	 * applet whose one required setting can be given only on a command
	 * line is one nobody can configure from the thing they are looking
	 * at (sec 2.6.5).
	 */
	m_station_box = new QLineEdit(this);
	m_station_box->setPlaceholderText(tr("station, e.g. ISTOCK822"));
	/*
	 * A minimum as well as a maximum. Without the floor the controls
	 * row squeezed it to about forty pixels showing "stati...", which
	 * is the one field the applet cannot work without and the one the
	 * empty state tells you to fill.
	 */
	m_station_box->setMinimumWidth(150);
	m_station_box->setMaximumWidth(190);
	m_station_box->setText(bbq_settings::station());

	connect(m_station_box, &QLineEdit::editingFinished, this, [this]() {
		const QString wanted = m_station_box->text().trimmed();
		if (wanted == bbq_settings::station()) {
			return;
		}

		bbq_settings::set_station(wanted);
		m_feed->set_station(wanted);
		m_feed->refresh();
	});

	/*
	 * The layout, offered as a choice rather than only inferred.
	 * "Auto" is the device's own answer (sec 10.1) and is right until
	 * somebody disagrees with it, which is exactly the case a compiled
	 * default cannot see.
	 */
	m_layout_box = new QComboBox(this);
	m_layout_box->addItem(tr("Auto"), QStringLiteral("auto"));
	m_layout_box->addItem(tr("Desktop"), QStringLiteral("desktop"));
	m_layout_box->addItem(tr("Mobile"), QStringLiteral("mobile"));

	const int stored_layout = m_layout_box->findData(bbq_settings::layout());
	if (stored_layout >= 0) {
		m_layout_box->setCurrentIndex(stored_layout);
	}

	connect(m_layout_box, &QComboBox::currentIndexChanged, this, [this](int) {
		const QString wanted = m_layout_box->currentData().toString();
		bbq_settings::set_layout(wanted);
		set_layout(bbq_layout_resolve(wanted));
	});

	QCheckBox *windows = new QCheckBox(tr("Grill windows"), this);
	windows->setChecked(m_graph->show_windows());
	connect(windows, &QCheckBox::toggled, this, [this](bool on) {
		m_graph->set_show_windows(on);
	});

	QCheckBox *marks = new QCheckBox(tr("Mark samples"), this);
	marks->setChecked(m_graph->show_samples());
	connect(marks, &QCheckBox::toggled, this, [this](bool on) {
		m_graph->set_show_samples(on);
	});

	m_controls = new QWidget(this);

	/*
	 * Gathered in order rather than placed, because set_layout rebuilds
	 * the arrangement and cannot do that from widgets already owned by
	 * a layout it is about to replace.
	 */
	m_control_items << new QLabel(tr("Station:"), this) << m_station_box
	                << new QLabel(tr("Interpolation:"), this) << method
	                << new QLabel(tr("Rounding:"), this) << smoothing
	                << windows << marks
	                << new QLabel(tr("Layout:"), this) << m_layout_box;

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(m_verdict, 0);
	layout->addWidget(m_graph, 1);
	layout->addWidget(m_controls, 0);

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

	/*
	 * The device decides, unless the configuration disagrees. Applied
	 * before the first paint so nothing is drawn twice in two shapes.
	 */
	set_layout(bbq_layout_resolve(bbq_settings::layout()));

	refresh_status();
	resize(820, 400);
}

void bbq_main_window::set_layout(bbq_layout layout) {
	m_graph->set_layout(layout);

	const bbq_metrics metrics = bbq_metrics_for(layout);

	/*
	 * The controls change SHAPE, not just size. On a phone the row
	 * becomes six things a couple of millimetres wide, which is a row
	 * nobody can use, so it wraps and each item gets a height a finger
	 * can find.
	 */
	/*
	 * The old arrangement goes before the new one is built. Deleting
	 * the layout does not delete the widgets, which is the whole reason
	 * they are held separately.
	 */
	delete m_controls->layout();

	for (QWidget *item : m_control_items) {
		item->setMinimumHeight(metrics.control_height);
	}

	if (metrics.stack_controls) {
		/*
		 * Two columns. A phone is tall and narrow, so the row that
		 * suits a desktop becomes ten things a couple of millimetres
		 * wide -- a row nobody can hit. Pairs read as label-then-value
		 * down the screen instead.
		 */
		QGridLayout *grid = new QGridLayout(m_controls);
		grid->setContentsMargins(0, 0, 0, 0);
		grid->setSpacing(8);

		for (int i = 0; i < m_control_items.size(); ++i) {
			grid->addWidget(m_control_items.at(i), i / 2, i % 2);
		}

		grid->addWidget(freshness_label, m_control_items.size() / 2 + 1, 0, 1, 2);
	} else {
		QHBoxLayout *row = new QHBoxLayout(m_controls);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(6);

		for (QWidget *item : m_control_items) {
			row->addWidget(item, 0);
		}

		row->addStretch(1);
		row->addWidget(freshness_label, 0);
	}

	m_station_box->setVisible(metrics.show_station_field);

	/*
	 * The two long strings wrap on a narrow screen and do not on a wide
	 * one.
	 *
	 * A QLabel's size hint is its text on one line, so the verdict --
	 * "Best window: Fri 19:16 to 22:06 (2.8 h, score 0.90) +3 more" --
	 * was holding a phone-shaped window open to 1249 pixels. A sentence
	 * setting the width of a graph is a layout deciding itself from its
	 * least important element.
	 */
	m_verdict->setWordWrap(metrics.stack_controls);
	freshness_label->setWordWrap(metrics.stack_controls);

	/*
	 * Word wrap alone was not enough: a wrapped QLabel still reports a
	 * minimum width wide enough for its longest unbreakable run, so the
	 * verdict sentence held a phone-shaped window open to 1249 pixels.
	 * Ignoring its horizontal hint is what actually stops a sentence
	 * deciding the width of a graph.
	 */
	const QSizePolicy::Policy across =
	        metrics.stack_controls ? QSizePolicy::Ignored : QSizePolicy::Preferred;
	m_verdict->setSizePolicy(across, QSizePolicy::Minimum);
	freshness_label->setSizePolicy(across, QSizePolicy::Minimum);
	freshness_label->setAlignment(metrics.stack_controls
	                                      ? Qt::AlignLeft | Qt::AlignVCenter
	                                      : Qt::AlignRight | Qt::AlignVCenter);

	/*
	 * No resize here on purpose.
	 *
	 * On a phone the window is whatever the device gives it, so a
	 * layout that resized its own window would be arguing with the
	 * window manager about something it does not own. The shape has to
	 * work at the size it is handed, which is also the only way to know
	 * it works.
	 *
	 * Previewing the mobile shape on a desktop is a separate concern
	 * and belongs to whatever is doing the previewing.
	 */
}

QString bbq_main_window::verdict() const {
	return m_verdict->text();
}

void bbq_main_window::set_interpolation(bbq_interpolation method) {
	m_graph->set_interpolation(method);

	const int index = m_method_box->findData(static_cast<int>(method));
	if (index >= 0) {
		m_method_box->setCurrentIndex(index);
	}
}

void bbq_main_window::set_smoothing(int seconds) {
	m_graph->set_smoothing(seconds);

	const int index = m_smoothing_box->findData(seconds);
	if (index >= 0) {
		m_smoothing_box->setCurrentIndex(index);
	}
}

void bbq_main_window::begin(const QString &station_id, const QString &geocode) {
	/*
	 * The command line overrides the file for this run and does not
	 * write to it, so trying a different station leaves the configured
	 * one alone -- which is what an override should mean.
	 */
	const QString station =
	        station_id.isEmpty() ? bbq_settings::station() : station_id;

	m_feed->set_station(station);
	if (m_station_box->text().trimmed().isEmpty()) {
		m_station_box->setText(station);
	}

	/*
	 * The forecast point, in the order sec 2.6.7 settled: an explicit
	 * override wins, then the coordinate cached from the station, and
	 * otherwise the station supplies one when it answers.
	 */
	QString point = geocode;
	if (point.isEmpty()) {
		point = bbq_settings::geocode_override();
	}
	if (point.isEmpty()) {
		point = bbq_settings::derived_geocode();
	}

	if (!point.isEmpty()) {
		const QStringList parts = point.split(QLatin1Char(','));
		if (parts.size() == 2) {
			m_feed->set_geocode(parts.at(0).toDouble(), parts.at(1).toDouble());
		}
	}

	/*
	 * Cached only when the station in use is the CONFIGURED one.
	 *
	 * A --station override that wrote here would file one station's
	 * coordinates against a config naming another, so the next
	 * argument-free run would place the forecast bands at a station it
	 * is not reading -- two places on one axis, which is the whole
	 * thing sec 2.6.7 exists to prevent. An override overrides the run,
	 * not the configuration.
	 */
	const bool configured = station == bbq_settings::station();

	connect(m_feed, &bbq_wu_feed::geocode_derived, this,
	        [configured](double latitude, double longitude) {
		if (!configured) {
			return;
		}

		bbq_settings::set_derived_geocode(latitude, longitude);
	});

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

	/*
	 * The verdict, recomputed whenever a band lands. Absent rather than
	 * cheerful when nothing qualifies: "no good window" is a useful
	 * answer and inventing a mediocre one to fill the line would not
	 * be.
	 */
	const QTimeZone zone = composite.zone();
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	const bbq_grill_policy policy;
	const std::vector<bbq_window> windows =
	        bbq_grill_windows(composite, zone, now, now + 3 * 24 * 3600, policy);

	if (windows.empty()) {
		m_verdict->setText(tr("No grilling window in the next three days."));
	} else {
		const bbq_window &best = windows.front();
		QDateTime start = QDateTime::fromSecsSinceEpoch(best.start_utc);
		QDateTime end = QDateTime::fromSecsSinceEpoch(best.end_utc);
		if (zone.isValid()) {
			start = QDateTime::fromSecsSinceEpoch(best.start_utc, zone);
			end = QDateTime::fromSecsSinceEpoch(best.end_utc, zone);
		}

		QString verdict = tr("Best window: ");
		verdict += start.toString(QStringLiteral("ddd HH:mm"));
		verdict += QStringLiteral(" to ");
		verdict += end.toString(QStringLiteral("HH:mm"));
		verdict += QStringLiteral("  (");
		verdict += QString::number(best.duration_s() / 3600.0, 'f', 1);
		verdict += tr(" h, score ");
		verdict += QString::number(best.score, 'f', 2);
		verdict += QStringLiteral(")");

		if (windows.size() > 1) {
			verdict += QStringLiteral("   +");
			verdict += QString::number(windows.size() - 1);
			verdict += tr(" more");
		}

		m_verdict->setText(verdict);
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
