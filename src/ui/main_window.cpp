#include "ui/main_window.h"

#include "ui/widget_picture.h"

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

#include <QDateTime>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QGridLayout>
#include <QResizeEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QShowEvent>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QVBoxLayout>
#include <QWindow>

#include "graph/forecast_graph.h"
#include "graph/interpolate.h"
#include "model/grill.h"
#include "ui/layout.h"
#include "model/settings.h"
#include "model/correction.h"
#include "ui/theme.h"
#include "wu/feed.h"

namespace {

/*
 * Whether a QSlider would abort this process (sec 11.6).
 *
 * True only on Android before API 33, where Qt's accessibility bridge
 * builds a RangeInfo with a constructor that does not exist yet and
 * leaves the JNI exception pending, so the next JNI call aborts.
 * Everywhere else -- desktop, and any Android new enough to carry that
 * constructor -- a slider is safe and is the better control for a
 * continuous value.
 */
bool bbq_slider_aborts_here() {
#ifdef Q_OS_ANDROID
	const int sdk = QJniObject::getStaticField<jint>(
	        "android/os/Build$VERSION", "SDK_INT");
	return sdk < 33;
#else
	return false;
#endif
}

} // namespace

bbq_main_window::bbq_main_window(QWidget *parent)
        : QWidget(parent), m_method_box(nullptr), m_smoothing_box(nullptr),
          m_layout_box(nullptr), m_controls(nullptr),
          m_station_box(nullptr), m_verdict(nullptr), m_freshness_label(nullptr),
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

	m_freshness_label = new QLabel(this);
	m_freshness_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	m_freshness_label->setTextFormat(Qt::PlainText);

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
	/*
	 * A LIST, not a blank field (sec 14.2).
	 *
	 * The old control was a text box somebody had to know the answer
	 * to, which is a poor way to start and a worse way to recover: the
	 * station this project ran against for months reports a dead
	 * thermometer, and replacing it meant querying an API by hand.
	 *
	 * Editable all the same. A station id somebody already knows should
	 * not require finding it on a map first, and discovery only ever
	 * finds what is near a coordinate it has been given.
	 */
	m_station_box = new QComboBox(this);
	m_station_box->setEditable(true);
	m_station_box->setInsertPolicy(QComboBox::NoInsert);
	m_station_box->lineEdit()->setPlaceholderText(
	        tr("station, e.g. ISTOCK822"));

	/*
	 * A floor low enough for a phone. The old minimum was set for a
	 * desktop row and became a lower bound on the whole window: on a
	 * narrow screen it pushed the controls wider than the display, and
	 * everything to its right was clipped (sec 10.5.1).
	 */
	m_station_box->setMinimumWidth(90);
	m_station_box->setMaximumWidth(230);

	connect(m_station_box, &QComboBox::activated, this, [this](int index) {
		const QString id = m_station_box->itemData(index).toString();
		watch_station(id.isEmpty() ? m_station_box->itemText(index) : id);
	});

	connect(m_station_box->lineEdit(), &QLineEdit::editingFinished, this,
	        [this]() {
		watch_station(m_station_box->currentText().trimmed());
	});

	/*
	 * Pinning is what makes a station cost requests, so it is a control
	 * rather than a consequence (sec 13). The list marks the pinned
	 * ones; this is how one becomes pinned.
	 */
	m_pin_box = new QCheckBox(tr("Pin"), this);
	m_pin_box->setToolTip(
	        tr("Keep fetching this station's history, so its forecasts can "
	           "be scored even while another is being watched."));

	connect(m_pin_box, &QCheckBox::toggled, this, [this](bool on) {
		const QString id = bbq_settings::station();
		if (id.isEmpty()) {
			return;
		}

		m_feed->history().set_station_pinned(id, on);
		refresh_station_list();
	});

	/*
	 * Somewhere else. Discovery can only look around a coordinate it
	 * has, so reaching another town means naming it first.
	 */
	m_find_button = new QPushButton(tr("Find..."), this);
	m_find_button->setToolTip(tr("Find stations near a place"));

	connect(m_find_button, &QPushButton::clicked, this, [this]() {
		bool typed = false;
		const QString place = QInputDialog::getText(
		        this, tr("Find stations"), tr("Place:"), QLineEdit::Normal,
		        QString(), &typed);

		if (!typed || place.trimmed().isEmpty()) {
			return;
		}

		m_feed->search_places(place);
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

	/*
	 * Wind, offered rather than imposed. It matters to the grilling
	 * score (sec 7) more than to the graph, and the plot already carries
	 * temperature, rain and chance -- a fourth line by default would
	 * cost every reader something to gain what only some of them want.
	 */
	m_wind_box = new QCheckBox(tr("Wind"), this);
	m_wind_box->setChecked(m_graph->show_wind());
	connect(m_wind_box, &QCheckBox::toggled, this, [this](bool on) {
		m_graph->set_show_wind(on);
	});

	/*
	 * How steady the temperature axis is (sec 3.14).
	 *
	 * A slider rather than a checkbox because the useful answer is not
	 * yes or no: a wide graph wants a firm scale to read a trend
	 * against, a narrow one wants the detail back, and where between
	 * those is a matter of what somebody is looking for.
	 *
	 * EXCEPT WHERE A SLIDER ABORTS THE PROGRAM (sec 11.6). Asked of the
	 * RUNTIME rather than the compiler: this was `#ifdef Q_OS_ANDROID`,
	 * which is wrong by one word, because the fault belongs to the API
	 * LEVEL and not to Android. On SDK 33 and later the constructor
	 * exists, the abort cannot happen, and the slider comes back.
	 * Qt's accessibility bridge builds a RangeInfo for any widget with a
	 * value, using a constructor that does not exist before API 33, and
	 * does not clear the JNI exception that raises -- so the next JNI
	 * call aborts the process. A drop-down carries no value range, so
	 * the node is never built and the fault cannot occur. The setting is
	 * the same number either way; only the way of choosing it differs.
	 */
	QWidget *steadiness = nullptr;

	if (bbq_slider_aborts_here()) {
		QComboBox *steadiness_box = new QComboBox(this);
		steadiness_box->addItem(tr("Off"), 0);
		steadiness_box->addItem(tr("Slight"), 25);
		steadiness_box->addItem(tr("Steady"), 60);
		steadiness_box->addItem(tr("Firm"), 100);

		/*
		 * Nearest rather than exact, because the stored value may have come
		 * from a desktop slider that can express anything in between, and a
		 * phone that silently reset it to Off would be worse than one that
		 * rounds.
		 */
		const int stored = bbq_settings::scale_steadiness(m_graph->scale_steadiness());
		int nearest = 0;
		for (int i = 0; i < steadiness_box->count(); ++i) {
			const int candidate = steadiness_box->itemData(i).toInt();
			const int best = steadiness_box->itemData(nearest).toInt();
			if (qAbs(candidate - stored) < qAbs(best - stored)) {
				nearest = i;
			}
		}

		steadiness_box->setCurrentIndex(nearest);
		m_graph->set_scale_steadiness(steadiness_box->currentData().toInt());

		connect(steadiness_box, &QComboBox::currentIndexChanged, this,
		        [this, steadiness_box](int) {
			const int value = steadiness_box->currentData().toInt();
			bbq_settings::set_scale_steadiness(value);
			m_graph->set_scale_steadiness(value);
		});

		steadiness = steadiness_box;
	} else {
		QSlider *steadiness_slider = new QSlider(Qt::Horizontal, this);
		steadiness_slider->setRange(0, 100);
		steadiness_slider->setMinimumWidth(80);
		steadiness_slider->setMaximumWidth(160);
		steadiness_slider->setValue(
		        bbq_settings::scale_steadiness(m_graph->scale_steadiness()));
		m_graph->set_scale_steadiness(steadiness_slider->value());

		connect(steadiness_slider, &QSlider::valueChanged, this, [this](int value) {
			bbq_settings::set_scale_steadiness(value);
			m_graph->set_scale_steadiness(value);
		});

		steadiness = steadiness_slider;
	}

	/*
	 * The theme. Beside the layout box because they are the same kind of
	 * choice: a compiled-in default, overridable, with "auto" meaning
	 * the device's own answer (sec 10.3).
	 */
	m_theme_box = new QComboBox(this);
	m_theme_box->addItem(tr("Auto"), QStringLiteral("auto"));
	m_theme_box->addItem(tr("Light"), QStringLiteral("light"));
	m_theme_box->addItem(tr("Dark"), QStringLiteral("dark"));

	const int stored_theme = m_theme_box->findData(bbq_settings::theme());
	if (stored_theme >= 0) {
		m_theme_box->setCurrentIndex(stored_theme);
	}

	connect(m_theme_box, &QComboBox::currentIndexChanged, this, [this](int index) {
		const QString wanted = m_theme_box->itemData(index).toString();
		bbq_settings::set_theme(wanted);
		apply_theme(bbq_theme_resolve(wanted));
	});

	m_controls = new QWidget(this);

	/*
	 * Gathered in order rather than placed, because set_layout rebuilds
	 * the arrangement and cannot do that from widgets already owned by
	 * a layout it is about to replace.
	 */
	m_control_items << new QLabel(tr("Station:"), this) << m_station_box
	                << m_pin_box << m_find_button
	                << new QLabel(tr("Interpolation:"), this) << method
	                << new QLabel(tr("Rounding:"), this) << smoothing
	                << windows << marks << m_wind_box
	                << new QLabel(tr("Steady scale:"), this) << steadiness
	                << new QLabel(tr("Layout:"), this) << m_layout_box
	                << new QLabel(tr("Theme:"), this) << m_theme_box;

	QVBoxLayout *layout = new QVBoxLayout(this);
	m_root_layout = layout;

	/* What the layout asks for before any system furniture is counted. */
	m_base_margins = layout->contentsMargins();
	layout->addWidget(m_verdict, 0);
	layout->addWidget(m_graph, 1);
	/*
	 * The controls go inside a scroll area, and that is what stops them
	 * starving the plot (sec 10.6).
	 *
	 * A QScrollArea's minimum height is its own, not its child's, so
	 * the layout is free to give the graph -- which has the stretch --
	 * everything the cap below does not reserve. Without it the
	 * controls' minimum was nine rows at finger height and the graph
	 * got whatever was left, which on the Fold's cover screen was a
	 * fifth of a very tall screen.
	 *
	 * Horizontal scrolling is off deliberately: the fields shrink to
	 * the viewport instead, which is what `setWidgetResizable` gives,
	 * and a control the reader has to scroll sideways to find is worse
	 * than a narrow one.
	 */
	m_control_scroll = new QScrollArea(this);
	m_control_scroll->setWidget(m_controls);
	m_control_scroll->setWidgetResizable(true);
	m_control_scroll->setFrameShape(QFrame::NoFrame);
	m_control_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_control_scroll->viewport()->setAutoFillBackground(false);
	m_control_scroll->setSizePolicy(QSizePolicy::Preferred,
	                                QSizePolicy::Maximum);

	/*
	 * A FINGER has to be able to scroll it (sec 10.6).
	 *
	 * A QScrollArea scrolls by its scrollbar and by the wheel, and by
	 * nothing else -- dragging its contents does nothing. On a desktop
	 * that is fine. On a phone the scrollbar is a few pixels wide and a
	 * drag is the only gesture anybody will try, so without this the
	 * controls below the fold were not merely awkward, they were
	 * unreachable: measured by swiping the panel on the device and
	 * watching nothing move.
	 *
	 * Found because the cap that made the plot bigger is also what put
	 * controls below a fold in the first place. The fix that gives the
	 * chart its room has to hand back a way to reach what it displaced.
	 */
	QScroller::grabGesture(m_control_scroll->viewport(),
	                       QScroller::LeftMouseButtonGesture);

	layout->addWidget(m_control_scroll, 0);

	/* The saved choice, applied before anything is shown. */
	apply_theme(bbq_theme_resolve(bbq_settings::theme()));

	connect(m_feed, &bbq_wu_feed::updated, this, [this]() {
		m_graph->set_composite(m_feed->composite());
		refresh_corrected();
		refresh_status();

		/*
		 * The home-screen widget draws whatever this last wrote (sec
		 * 16). Here rather than on the fetch, because what the widget
		 * shows is the GRAPH, and the graph is only correct once the
		 * composite and the corrected band have both been applied to
		 * it -- two lines above.
		 *
		 * A no-op off Android.
		 */
		bbq_write_widget_picture(m_graph);
	});

	/*
	 * Panning into last month is the same operation as looking at this
	 * afternoon (project.md sec 12.8): the view says what it needs and
	 * the feed makes sure that range is loaded from the store.
	 *
	 * The feed decides whether anything actually has to be read -- this
	 * fires on every mouse move of a drag, so it must be cheap when the
	 * answer is already in memory.
	 */
	connect(m_graph, &bbq_forecast_graph::view_changed, this,
	        [this](qint64 from_utc, qint64 to_utc) {
		m_feed->set_view_range(from_utc, to_utc);
		m_graph->set_composite(m_feed->composite());
		refresh_corrected();
	});

	/*
	 * Discovery changed the list, so the list is rebuilt. Nothing on the
	 * graph changes -- a station list is not weather (sec 13).
	 */
	/*
	 * Ask the device where it is, once, and use the answer for
	 * DISCOVERY ONLY (sec 14.3). The forecast stays where the watched
	 * station is.
	 */
	m_locator = new bbq_locator(this);

	connect(m_locator, &bbq_locator::located, this,
	        [this](double latitude, double longitude) {
		/*
		 * Only when the fix has actually moved (sec 15.7.4). A launch
		 * from the same kitchen cannot have changed which stations are
		 * nearby, and asking again spends a request on a scraped key to
		 * be told what the archive already holds.
		 */
		m_feed->discover_stations_if_moved(latitude, longitude);
	});

	connect(m_locator, &bbq_locator::unavailable, this,
	        [this](const QString &reason) {
		/*
		 * Said where it belongs, which is NOT the freshness line: that
		 * label is rewritten on every fetch, so a message left there
		 * survives until the next request and then vanishes without
		 * anybody having read it. It is also about the data rather than
		 * about the device.
		 *
		 * The reason goes on the control that offers the alternative,
		 * and the empty list gets a placeholder pointing at it. Where
		 * stations are already known there is nothing to say: a fix
		 * would have added to a list that already works.
		 */
		m_find_button->setToolTip(
		        tr("Find stations near a place\n(location: %1)").arg(reason));

		if (m_station_box->count() == 0) {
			m_station_box->lineEdit()->setPlaceholderText(
			        tr("no location -- use Find..."));
		}
	});

	connect(m_feed, &bbq_wu_feed::stations_discovered, this, [this](int) {
		refresh_station_list();
	});

	/*
	 * A search answers with places, not stations. One has to be chosen
	 * before there is a coordinate to look around, and offering the
	 * first match silently would send somebody to Shlisselburg for
	 * asking about Gothenburg -- which is the actual second result.
	 */
	connect(m_feed, &bbq_wu_feed::places_found, this,
	        [this](const std::vector<bbq_wu_place> &places) {
		if (places.empty()) {
			m_freshness_label->setText(tr("no such place"));
			return;
		}

		QStringList names;
		for (const bbq_wu_place &place : places) {
			names << place.address;
		}

		bool chose = false;
		const QString picked = QInputDialog::getItem(
		        this, tr("Find stations"), tr("Which one?"), names, 0, false,
		        &chose);

		if (!chose) {
			return;
		}

		const int index = names.indexOf(picked);
		if (index < 0) {
			return;
		}

		/*
		 * Pinned, because the reader named it. An unpinned coordinate
		 * would be replaced by the next one derived from the station
		 * and the search would appear to do nothing.
		 */
		m_feed->set_geocode(places.at(index).latitude,
		                    places.at(index).longitude, true);
		m_feed->discover_stations();
	});

	/*
	 * SCORING IS A REASON TO REDRAW (sec 14.6).
	 *
	 * The record note in the verdict is recomputed by refresh_status,
	 * which runs on `updated` -- and `updated` is emitted while a
	 * response is being handled, whereas scoring happens when the round
	 * settles, after it. So the note was always one round behind, and
	 * the round it was behind by is the one that matters: the first
	 * time anything is ever scored, the display goes on saying "record:
	 * none yet" until the next fetch, which is exactly when somebody is
	 * looking to see whether it worked.
	 *
	 * Nothing had listened to this signal at all. It was emitted for a
	 * consumer that was never written.
	 */
	connect(m_feed, &bbq_wu_feed::verified, this, [this](int count) {
		Q_UNUSED(count);
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

/*
 * WHICH SHAPE, asked of the WINDOW rather than of the device (sec 10.6).
 *
 * `stack_controls` answers "is this a phone", and a phone does not stop
 * being one when it is turned sideways or unfolded. Asking it alone left
 * the two-column stack in place on a 800dp landscape phone and on an
 * unfolded foldable's 674dp inner screen, which is where this layout has
 * the least height to spend and the most width going unused.
 *
 * 600dp is Android's compact/medium boundary, and it is the number
 * beerssh's layout manager already uses for the same decision in this
 * workspace -- one convention for "is this window narrow", rather than
 * two that can disagree. It is a WIDTH rather than an aspect ratio on
 * purpose: unfolding gives width whichever way the device is held, and
 * a 674x841 inner screen is portrait and still wide enough for the row.
 *
 * The desktop shape is unaffected: it is never stacked, so this returns
 * true for it at every size, exactly as before.
 */
bool bbq_main_window::wants_wide_controls() const {
	return !m_metrics.stack_controls || width() >= 600;
}

/*
 * A window that changed shape re-asks for the layout it already has.
 *
 * Rotation and unfolding do not reach the app any other way: the
 * manifest's configChanges list means Android resizes the window
 * instead of recreating the activity, so without this the controls kept
 * whichever shape they were given at startup for the life of the run.
 *
 * NOTHING HERE IS ALLOWED TO COST STATE, and the reason it does not is
 * that set_layout deletes the controls' LAYOUT and never the controls:
 * they are held in m_control_items precisely so a reshape can re-parent
 * them instead of rebuilding them. test_window's
 * turning_and_unfolding_the_device_keeps_what_was_on_screen asserts the
 * widget pointers themselves across four shapes for that reason -- the
 * values would survive a rebuild that restored them, and the pointers
 * would not.
 *
 * Re-run only when the shape would DIFFER. A window dragged across a
 * desktop delivers these continuously, and tearing a layout down per
 * pixel is visible.
 */
void bbq_main_window::cap_control_height() {
	if (m_control_scroll == nullptr) {
		return;
	}

	/*
	 * A FRACTION of the window, not a fixed number of rows (sec 10.6).
	 *
	 * The plot is the program, so it keeps the majority of the height
	 * on any screen; the controls get the rest and scroll for whatever
	 * does not fit. On a desktop, where the controls are one row, the
	 * cap is lifted -- a row is short and capping it would only clip.
	 */
	if (m_wide_controls) {
		m_control_scroll->setMinimumHeight(0);
		m_control_scroll->setMaximumHeight(QWIDGETSIZE_MAX);
		return;
	}

	/*
	 * A maximum alone was not enough. The scroll area's own size hint
	 * is small -- it does not inherit the child's -- so the layout gave
	 * it one row and everything else had to be scrolled for. The share
	 * is therefore FIXED at whatever the controls want, up to the cap:
	 * they get all of it when they fit, and 42% with a scrollbar when
	 * they do not.
	 */
	/*
	 * 30% by the copyright holder's choice, looked at on the device.
	 * The chart is the program, so it keeps the clear majority; what
	 * the controls lose they make up by scrolling.
	 */
	const int share = qMax(140, int(height() * 0.30));
	/*
	 * Asked of the MINIMUM, not the size hint. `setWidgetResizable`
	 * sizes the child to the viewport, so once it is inside a scroll
	 * area its hint reports the viewport back and the share it asked
	 * for collapsed to a single row. The minimum is the one number that
	 * still describes the stacked controls themselves -- set_layout
	 * computes it from the layout it has just built.
	 */
	const int content =
	        qMax(m_controls->minimumHeight(), m_controls->sizeHint().height());
	if (content <= 0) {
		return;
	}

	const int wanted = qMin(content, share);

	m_control_scroll->setMinimumHeight(wanted);
	m_control_scroll->setMaximumHeight(wanted);
}

void bbq_main_window::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);

	if (m_controls == nullptr) return;

	cap_control_height();
	if (wants_wide_controls() == m_wide_controls) return;

	set_layout(m_layout);
}

void bbq_main_window::set_layout(bbq_layout layout) {
	m_graph->set_layout(layout);

	const bbq_metrics metrics = bbq_metrics_for(layout);

	/*
	 * Kept, because the safe-area code needs to know which shape it is
	 * padding: the mobile one gives its horizontal margins to the plot
	 * (sec 10.4), and that decision is made where the margins are set
	 * rather than duplicated here.
	 */
	m_metrics = metrics;
	m_layout = layout;
	m_wide_controls = wants_wide_controls();
	apply_safe_area();

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

	if (!m_wide_controls) {
		/*
		 * Two columns. A phone is tall and narrow, so the row that
		 * suits a desktop becomes ten things a couple of millimetres
		 * wide -- a row nobody can hit. Pairs read as label-then-value
		 * down the screen instead.
		 */
		QGridLayout *grid = new QGridLayout(m_controls);
		grid->setContentsMargins(0, 0, 0, 0);
		grid->setSpacing(8);

		/*
		 * The value column takes the slack, so the fields shrink with
		 * the screen instead of pushing past it. Without this the
		 * controls kept their preferred width on a narrow phone and the
		 * right-hand ones were simply cut off -- found by rendering at
		 * the Fold's cover-screen width rather than on the Fold, which
		 * is what --size exists for.
		 */
		grid->setColumnStretch(0, 0);
		grid->setColumnStretch(1, 1);

		/*
		 * Paired by MEANING, not by position.
		 *
		 * The old arrangement walked the list two at a time, which works
		 * only while every control has a label. The checkboxes do not,
		 * so from the first one onward every label sat beside somebody
		 * else's control -- "Layout:" next to "Wind", its combo on the
		 * next row beside "Theme:". A label that names the wrong thing
		 * is worse than no label.
		 */
		int row = 0;

		for (int i = 0; i < m_control_items.size(); ++i) {
			QWidget *item = m_control_items.at(i);
			const bool is_label = qobject_cast<QLabel *>(item) != nullptr;

			if (is_label && i + 1 < m_control_items.size()) {
				grid->addWidget(item, row, 0);
				grid->addWidget(m_control_items.at(i + 1), row, 1);
				++i;
				++row;
				continue;
			}

			/*
			 * An unlabelled control -- a checkbox. Two of them share a
			 * row, which is what makes them read as a set rather than as
			 * values missing their names.
			 */
			if (i + 1 < m_control_items.size() &&
			    qobject_cast<QLabel *>(m_control_items.at(i + 1)) == nullptr) {
				grid->addWidget(item, row, 0);
				grid->addWidget(m_control_items.at(i + 1), row, 1);
				++i;
			} else {
				grid->addWidget(item, row, 0, 1, 2);
			}

			++row;
		}

		grid->addWidget(m_freshness_label, row, 0, 1, 2);
	} else {
		QHBoxLayout *row = new QHBoxLayout(m_controls);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(6);

		for (QWidget *item : m_control_items) {
			row->addWidget(item, 0);
		}

		row->addStretch(1);
		row->addWidget(m_freshness_label, 0);
	}

	/*
	 * The controls assert their own height, so the scroll area SCROLLS
	 * instead of squashing them (sec 10.6).
	 *
	 * `setWidgetResizable` sizes the child to the viewport, and a
	 * viewport shorter than the child compressed the grid rows until
	 * the labels overlapped each other -- readable as a smear at the
	 * bottom of the screen rather than as a control anybody could use.
	 * Stating the minimum is what turns "too little room" into a
	 * scrollbar.
	 *
	 * Cleared first: the value is computed from the layout that has
	 * just been built, and a minimum left over from the other shape
	 * would be measured into it.
	 */
	m_controls->setMinimumHeight(0);
	m_controls->layout()->activate();
	m_controls->setMinimumHeight(m_controls->sizeHint().height());

	/*
	 * And re-take the share, now that there is a minimum to measure.
	 * resizeEvent caps too, but it runs BEFORE this and finds nothing
	 * to measure -- so without this the cap was computed once, from
	 * zero, and the scroll area kept its own one-row size hint.
	 */
	cap_control_height();

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
	m_freshness_label->setWordWrap(metrics.stack_controls);

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
	m_freshness_label->setSizePolicy(across, QSizePolicy::Minimum);
	m_freshness_label->setAlignment(metrics.stack_controls
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

	/*
	 * The store, opened before anything is fetched so the first round is
	 * archived rather than lost. A failure is reported and not fatal:
	 * the applet still draws, it just does not remember (sec 12).
	 */
	if (!m_feed->open_history(m_history_path)) {
		m_last_error = tr("history: ") + m_feed->history_error();
	}

	m_feed->set_station(station);
	refresh_station_list();

	/*
	 * The forecast point, in the order sec 2.6.7 settled: an explicit
	 * override wins, then the coordinate cached from the station, and
	 * otherwise the station supplies one when it answers.
	 */
	QString point = geocode;
	bool pinned = true;
	if (point.isEmpty()) {
		point = bbq_settings::geocode_override();
	}
	if (point.isEmpty()) {
		/*
		 * The cache, and the only one of the three that belongs to a
		 * station rather than to the configuration -- so it is the only
		 * one that must not survive the station changing.
		 */
		point = bbq_settings::derived_geocode();
		pinned = false;
	}

	if (!point.isEmpty()) {
		const QStringList parts = point.split(QLatin1Char(','));
		if (parts.size() == 2) {
			m_feed->set_geocode(parts.at(0).toDouble(), parts.at(1).toDouble(),
			                    pinned);
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

	/*
	 * Asked after the feed is running, not before. A fix only fills the
	 * station list; nothing on screen waits for it, and the program is
	 * fully usable while it is outstanding or if it never arrives.
	 */
	m_locator->locate_once();
}

void bbq_main_window::apply_theme(bbq_theme theme) {
	/*
	 * The whole application, not only the graph. A dark plot inside a
	 * light window is worse than either, and the controls are what the
	 * eye lands on first on a phone.
	 */
	bbq_theme_apply(theme);
	m_graph->set_theme(theme);

	if (m_theme_box != nullptr) {
		const int index = m_theme_box->findData(
		        QString::fromLatin1(bbq_theme_name(theme)));
		if (index >= 0 && index != m_theme_box->currentIndex()) {
			m_theme_box->setCurrentIndex(index);
		}
	}
}

void bbq_main_window::apply_safe_area() {
	if (m_root_layout == nullptr) {
		return;
	}

	/*
	 * On mobile the graph runs to the screen edges (sec 10.4).
	 *
	 * A phone screen is narrow enough that a margin either side is not
	 * breathing room, it is lost plot: those pixels are the difference
	 * between reading an evening and squinting at it. The vertical
	 * margins stay, because the verdict above and the controls below
	 * need separating from the plot.
	 *
	 * This is deliberately OUTSIDE the version guard below. It was
	 * inside it once, which meant the shape depended on the Qt version
	 * rather than on the layout: the desktop build is 6.8, the guard
	 * compiled the whole block away, and the mobile shape silently kept
	 * its margins on the one platform where they could be looked at.
	 */
	const bool edge_to_edge = m_metrics.stack_controls;
	const int base_left = edge_to_edge ? 0 : m_base_margins.left();
	const int base_right = edge_to_edge ? 0 : m_base_margins.right();

	QMargins safe;

	/*
	 * Qt gained safe areas in 6.9. Where it has them they are ADDED to
	 * the margins above, never substituted for them -- assigning them
	 * wiped the ordinary padding on a device that reports zeroes, and
	 * pressed every widget flat against all four edges.
	 */
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
	const QWindow *handle = windowHandle();
	if (handle != nullptr) {
		safe = handle->safeAreaMargins();
	}
#endif

	m_root_layout->setContentsMargins(base_left + safe.left(),
	                                  m_base_margins.top() + safe.top(),
	                                  base_right + safe.right(),
	                                  m_base_margins.bottom() + safe.bottom());
}

void bbq_main_window::showEvent(QShowEvent *event) {
	QWidget::showEvent(event);

	/*
	 * The window handle does not exist until the window is shown, and
	 * the margins change when the device rotates or is unfolded -- this
	 * phone is a fold, so that is a routine event rather than an edge
	 * case.
	 */
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
	QWindow *handle = windowHandle();
	if (handle != nullptr) {
		connect(handle, &QWindow::safeAreaMarginsChanged, this,
		        &bbq_main_window::apply_safe_area, Qt::UniqueConnection);
	}
#endif

	apply_safe_area();
}

void bbq_main_window::set_show_wind(bool show) {
	if (m_wind_box != nullptr) {
		m_wind_box->setChecked(show);
	}

	m_graph->set_show_wind(show);
}

QString bbq_main_window::verification_note(const bbq_composite &composite,
                                           qint64 when_utc, qint64 now_utc) {
	/*
	 * The owner's band, because that is the band the graph drew
	 * (sec 3.18.1).
	 *
	 * This looks up how well a band has done at this lead time, and
	 * with at() it asked about whichever band won the instant -- which
	 * for the next two hours is radar, a band that carries no
	 * temperature and therefore has no temperature record to report.
	 * The note would have described a band the reader is not looking
	 * at, or gone silent for want of one.
	 */
	bbq_reading reading = composite.owner_at(when_utc);
	if (!reading.is_valid()) {
		reading = composite.at(when_utc);
	}

	if (!reading.is_valid() || m_feed->station().isEmpty()) {
		return QString();
	}

	const bbq_band band = reading.series->band();
	const bbq_lead_bucket bucket = bbq_lead_bucket_for(when_utc - now_utc);
	const bbq_history &store = m_feed->history();

	const bbq_verification temperature = store.verification(
	        m_feed->station(), band, QStringLiteral("temperature"), bucket);
	const bbq_brier rain = store.brier(m_feed->station(), band, bucket);

	/*
	 * The verdict's own record (sec 12.20). It is the last thing added
	 * to the store and the first thing a reader actually wants: the
	 * three ingredients say whether the numbers were right, and this
	 * says whether the ANSWER was.
	 */
	const bbq_verification verdict = store.verification(
	        m_feed->station(), band, QStringLiteral("grill"), bucket);

	if (temperature.count == 0 && rain.count == 0 && verdict.count == 0) {
		/*
		 * Said rather than left blank. An empty space reads as "nothing
		 * to report"; the truth is that nothing has been checked yet,
		 * which is a different thing and the normal state of a fresh
		 * install (sec 12.6 -- a forecast is only scored once the hour
		 * it predicted has been observed).
		 */
		return tr("   record: none yet");
	}

	QString note = tr("   record: ");
	note += QString::fromLatin1(bbq_band_name(band));
	note += QStringLiteral(" @");
	note += QString::fromLatin1(bbq_lead_bucket_name(bucket));

	if (temperature.count > 0) {
		/*
		 * The SIGN carries the meaning, so it is always shown: a band
		 * that runs warm and one that runs cold are different problems
		 * and "1.2" says neither.
		 */
		/*
		 * The sign is written, not asked for (sec 14.11).
		 *
		 * This passed the '+' as arg()'s fifth parameter, which is the
		 * FILL character -- used only to pad to a field width, and the
		 * width here is zero. So it never appeared, and a warm band
		 * printed "1.2": exactly the string the comment above says
		 * must not be produced. A cold band read correctly throughout,
		 * because its minus comes from the number, which is why
		 * nothing ever looked wrong.
		 */
		const QLatin1Char sign(temperature.bias < 0 ? '-' : '+');
		note += QStringLiteral("  bias %1%2 C")
		                .arg(sign)
		                .arg(qAbs(temperature.bias), 0, 'f', 1);
		note += QStringLiteral(", MAE %1")
		                .arg(temperature.mean_absolute_error, 0, 'f', 1);
	}

	if (rain.count > 0) {
		/*
		 * Skill rather than the raw Brier score. 0.1 is excellent in a
		 * dry climate and poor in a changeable one, and only the
		 * comparison against always-predicting-the-base-rate says which
		 * this is (sec 12.3).
		 */
		note += QStringLiteral(", rain skill %1").arg(rain.skill(), 0, 'f', 2);
	}

	if (verdict.count > 0) {
		/*
		 * As a tolerance rather than a signed bias. The score runs 0 to
		 * 1 and its sign says which way a recommendation erred, which
		 * is a second question; what the reader wants first is how far
		 * off the answer has been, and a plus-or-minus reads that way
		 * without inviting the other reading.
		 */
		note += QStringLiteral(", verdict %1%2")
		                .arg(QChar(0x00b1))
		                .arg(verdict.mean_absolute_error, 0, 'f', 2);
	}

	note += QStringLiteral(" (n=%1)")
	                .arg(qMax(qMax(temperature.count, rain.count),
	                          verdict.count));

	return note;
}

void bbq_main_window::refresh_corrected() {
	/*
	 * Recomputed for whatever the graph is looking at, because the
	 * correction depends on lead time and lead time depends on the view.
	 */
	const qint64 from = m_graph->view_from_utc();
	const qint64 to = from + m_graph->view_span_s();

	m_graph->set_corrected(m_feed->corrected_forecast(from, to));
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

		/*
		 * How well this band has actually done at this lead (sec 12.12).
		 *
		 * The verdict names a window; this says whether the forecast it
		 * came from has earned any trust at that distance. It is the
		 * only question the verification tables were ever collected to
		 * answer, and until now they answered it only to --history.
		 *
		 * The BAND and LEAD are taken from the window itself rather than
		 * chosen: a record for some other band at some other lead would
		 * be a true number about the wrong thing.
		 */
		verdict += verification_note(composite, best.start_utc, now);

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

	m_freshness_label->setText(text);
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

void bbq_main_window::watch_station(const QString &id) {
	QString wanted = id.trimmed();

	/*
	 * A LABEL IS NOT AN ID (sec 14.2.1).
	 *
	 * The list shows "ISTOCK877  4.0 km" because the distance is what
	 * makes one of ten choosable. That string is what the editable field
	 * hands back when somebody clicks into it and presses enter, so
	 * committing the field stored the label as the station -- and the
	 * next fetch asked Weather Underground for a station called
	 * "ISTOCK767  0.3 km". Measured on the device: the settings file
	 * came back holding exactly that.
	 *
	 * Resolved against the list rather than parsed. Splitting on the
	 * spaces would work until a station id contains one.
	 */
	const int listed = m_station_box->findText(wanted);
	if (listed >= 0) {
		const QString behind = m_station_box->itemData(listed).toString();
		if (!behind.isEmpty()) {
			wanted = behind;
		}
	}

	/*
	 * Compared against what the FEED is reading, for the same reason.
	 * Against the configuration, a run under --station would treat
	 * choosing the configured station as a no-op -- the one selection
	 * that actually needs to move the feed, since that is where the two
	 * disagree.
	 */
	if (wanted == m_feed->station()) {
		return;
	}

	bbq_settings::set_station(wanted);

	/*
	 * AND THE OLD STATION'S COMPLAINT GOES WITH ITS DATA (sec 14.8.2).
	 *
	 * set_station drops every band that described the previous station
	 * or the coordinate derived from it. This line is the same fact
	 * displayed: "hourly: Connection refused" is about a fetch for
	 * somewhere the reader has just navigated away from, and leaving it
	 * up reports a fault in the station now being watched.
	 *
	 * It does not weaken what the message exists for. A band that fails
	 * must not fail invisibly, and this one had already been shown --
	 * what it must not do is outlive the thing it was about.
	 */
	m_last_error.clear();

	m_feed->set_station(wanted);

	/*
	 * PUSH THE EMPTIED COMPOSITE, or none of the above is visible
	 * (sec 14.8.3).
	 *
	 * set_station drops every band belonging to the old station, and
	 * the graph holds a COPY -- set_composite takes one by value. So
	 * the model was correct and the screen was not: the graph went on
	 * drawing the previous station's curves until something else
	 * happened to push a composite through, which is `updated` on a
	 * successful fetch, or a view change. Where the fetch fails, which
	 * is the case the drop exists for, neither ever comes.
	 *
	 * The refresh below usually hides it by succeeding a second later.
	 * That is what made a fix of the model alone look like a fix.
	 */
	m_graph->set_composite(m_feed->composite());
	refresh_corrected();
	refresh_status();

	m_feed->refresh();
	refresh_station_list();
}

void bbq_main_window::refresh_station_list() {
	if (m_station_box == nullptr) {
		return;
	}

	/*
	 * THE STATION IN USE, not the configured one (sec 14.12).
	 *
	 * --station overrides the configuration for a run and deliberately
	 * does not write to it, so the two disagree exactly when somebody
	 * is trying a station out. Selecting the configured one then names
	 * a station the program is not reading, with every number beside it
	 * coming from another -- the two-places-on-one-axis failure of sec
	 * 2.6.7, in the control whose whole job is to say which place.
	 *
	 * They are the same string on every ordinary run.
	 */
	const QString watched = m_feed->station();

	/*
	 * Rebuilt wholesale rather than patched. The list is short, it
	 * changes only when discovery runs or a pin is toggled, and a
	 * partial update is how a control ends up disagreeing with the
	 * thing it describes -- which this project has already paid for
	 * twice (sec 3.11.5).
	 */
	const QSignalBlocker quiet(m_station_box);
	m_station_box->clear();

	const std::vector<bbq_station> known = m_feed->stations();
	bool watched_listed = false;

	for (const bbq_station &one : known) {
		/*
		 * Pinned ones are MARKED, because pinning is what spends
		 * requests and a cost the reader cannot see is one they did not
		 * choose. Bold as well as bulleted: the bullet survives a
		 * narrow screen, the weight survives a glance.
		 */
		QString label = one.pinned ? QStringLiteral("* ") : QString();
		label += one.id;

		if (one.distance_km >= 0.0) {
			label += QStringLiteral("  ");
			label += QString::number(one.distance_km, 'f', 1);
			label += tr(" km");
		}

		m_station_box->addItem(label, one.id);

		if (one.pinned) {
			QFont marked = m_station_box->font();
			marked.setBold(true);
			m_station_box->setItemData(m_station_box->count() - 1, marked,
			                           Qt::FontRole);
		}

		if (one.id == watched) {
			m_station_box->setCurrentIndex(m_station_box->count() - 1);
			watched_listed = true;
		}
	}

	/*
	 * A station being watched that discovery has never seen still has
	 * to show. It is the ordinary case on a fresh install, where the id
	 * came from a setting or the command line.
	 */
	if (!watched.isEmpty() && !watched_listed) {
		m_station_box->addItem(watched, watched);
		m_station_box->setCurrentIndex(m_station_box->count() - 1);
	}

	if (m_pin_box != nullptr) {
		const QSignalBlocker still(m_pin_box);
		bool pinned = false;

		for (const bbq_station &one : known) {
			if (one.id == watched) {
				pinned = one.pinned;
			}
		}

		m_pin_box->setChecked(pinned);
		m_pin_box->setEnabled(!watched.isEmpty());
	}
}
