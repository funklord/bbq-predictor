#include <QApplication>
#include <QCoreApplication>
#include <QPixmap>
#include <QTimer>
#include <QStringList>
#include <QTextStream>

#include "ui/main_window.h"
#include "ui/tray_icon.h"
#include "graph/forecast_graph.h"
#include "graph/interpolate.h"
#include "ui/layout.h"
#include "wu/feed.h"
#include "wu/fetch_once.h"
#include "model/settings.h"
#include "store/history.h"

/*
 * bbq-predictor -- a Qt Widgets weather applet for the tray and a window.
 *
 * The high-resolution temperature and rain graphs are the point of the
 * program; everything here is the shell around them. See project.md.
 *
 * Nothing is fetched yet. This binary comes up, shows an empty graph that
 * says it is empty, and sits in the tray.
 */

#ifndef BBQ_VERSION_STRING
/*
 * Defined by bbq-predictor.pro from the VERSION file. The fallback keeps a
 * bare compile working while making it obvious the build did not go
 * through `make`, rather than quietly claiming a version it does not know.
 */
#define BBQ_VERSION_STRING "unknown"
#endif

namespace {

void print_usage(QTextStream &out) {
	out << "usage: bbq-predictor [--version] [--help]\n";
	out << "       bbq-predictor --fetch-once [--station ID]";
	out << " [--geocode LAT,LON]\n";
	out << "\n";
	out << "A weather applet for the system tray and a window.\n";
	out << "\n";
	out << "  --version      print the version and exit\n";
	out << "  --help         print this and exit\n";
	out << "  --fetch-once   fetch every band once, report what arrived,\n";
	out << "                 and exit. A diagnostic, not a feature.\n";
	out << "  --station ID   the pinned weather station, e.g. ISTOCK822\n";
	out << "  --geocode      LAT,LON for the forecast bands; derived from\n";
	out << "                 the station when omitted\n";
	out << "  --interp M     step|linear|monotone|akima|makima|natural|\n";
	out << "  --layout L     auto|desktop|mobile, for --shot\n";
	out << "  --cursor N     park the readout on column N, for --shot\n";
	out << "  --tray-icon F  also save the tray icon, for --shot\n";
	out << "  --shot FILE    fetch, render the window to a PNG, and exit.\n";
	out << "                 A diagnostic: looking at the picture is how\n";
	out << "                 layout defects actually get found.\n";
}

/* The value after `name`, or empty when absent or last. */
QString option_value(const QStringList &arguments, const QString &name) {
	const int index = arguments.indexOf(name);
	if (index < 0 || index + 1 >= arguments.size()) {
		return QString();
	}

	return arguments.at(index + 1);
}

} // namespace

int main(int argc, char *argv[]) {
	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("bbq-predictor"));
	QApplication::setApplicationVersion(QStringLiteral(BBQ_VERSION_STRING));

	/*
	 * Answered before anything is constructed, so both work without a
	 * display -- which is what makes them usable from a build chroot or a
	 * CI job that has no X or Wayland session.
	 */
	QTextStream out(stdout);
	const QStringList arguments = QApplication::arguments();

	if (arguments.contains(QStringLiteral("--version"))) {
		out << "bbq-predictor " << BBQ_VERSION_STRING << "\n";
		return 0;
	}

	if (arguments.contains(QStringLiteral("--help"))) {
		print_usage(out);
		return 0;
	}

	/*
	 * Answered before any widget is built, so it runs headless -- in a
	 * build chroot, over ssh, or anywhere without a display. Bounded by
	 * its own timeout rather than by whatever invokes it.
	 */
	if (arguments.contains(QStringLiteral("--fetch-once"))) {
		return bbq_wu_fetch_once(
		        option_value(arguments, QStringLiteral("--station")),
		        option_value(arguments, QStringLiteral("--geocode")), 30,
		        option_value(arguments, QStringLiteral("--history-path")));
	}

	/*
	 * A tray applet must not exit when its window is closed -- closing
	 * the window is how it gets put away, not how it is quit.
	 *
	 * That holds only where there IS a tray. Without one the window is
	 * the entire user interface, and keeping the process alive after it
	 * closes leaves something running with no way to see it, reach it or
	 * quit it short of kill. So the answer follows the tray (sec 4.1),
	 * and is decided before the window is shown.
	 */
	QApplication::setQuitOnLastWindowClosed(!bbq_tray_icon::is_available());

	const QString station = option_value(arguments, QStringLiteral("--station"));
	const QString geocode = option_value(arguments, QStringLiteral("--geocode"));

	bbq_main_window window;
	bbq_tray_icon tray;

	QObject::connect(&tray, &bbq_tray_icon::toggle_requested,
	                 &window, &bbq_main_window::toggle_visibility);

	/*
	 * The tray follows the data. Updated on a failure as well as a
	 * success, because sec 2.4's point is that a refresh which stopped
	 * working must show somewhere, and the tray is where a glance
	 * lands.
	 */
	const auto refresh_tray = [&tray, &window]() {
		tray.show_state(window.feed()->composite(), window.verdict());
	};

	QObject::connect(window.feed(), &bbq_wu_feed::updated, &window, refresh_tray);
	QObject::connect(window.feed(), &bbq_wu_feed::settled, &window, refresh_tray);

	/*
	 * Said out loud on the session that cannot show a tray, rather than
	 * discovered as an icon that never appears (project.md sec 4.1). With
	 * no tray and no window there would be no way back to the program at
	 * all, so the window is shown in that case instead of hidden.
	 */
	if (bbq_tray_icon::is_available()) {
		tray.show();
		window.show();
	} else {
		QTextStream error(stderr);
		error << "bbq-predictor: no system tray on this session.\n";
		error << "bbq-predictor:   On GNOME this needs a StatusNotifierItem\n";
		error << "bbq-predictor:   shell extension. Running as a plain window.\n";
		window.show();
	}

	/*
	 * Pick the curve for a shot, so four renderings can be compared
	 * side by side. The window's drop-down is the real control.
	 */
	const QString interp = option_value(arguments, QStringLiteral("--interp"));
	if (interp == QStringLiteral("step")) {
		window.set_interpolation(bbq_interpolation::step);
	} else if (interp == QStringLiteral("linear")) {
		window.set_interpolation(bbq_interpolation::linear);
	} else if (interp == QStringLiteral("akima")) {
		window.set_interpolation(bbq_interpolation::akima);
	} else if (interp == QStringLiteral("makima")) {
		window.set_interpolation(bbq_interpolation::makima);
	} else if (interp == QStringLiteral("natural")) {
		window.set_interpolation(bbq_interpolation::natural);
	} else if (interp == QStringLiteral("catmull")) {
		window.set_interpolation(bbq_interpolation::catmull);
	} else if (interp == QStringLiteral("monotone")) {
		window.set_interpolation(bbq_interpolation::monotone);
	}

	const QString want_layout = option_value(arguments, QStringLiteral("--layout"));
	if (!want_layout.isEmpty()) {
		window.set_layout(bbq_layout_resolve(want_layout));
	}

	const QString smooth = option_value(arguments, QStringLiteral("--smooth"));
	if (!smooth.isEmpty()) {
		window.set_smoothing(smooth.toInt());
	}

	/*
	 * The view, for looking at a zoom or a pan without a mouse
	 * (project.md sec 13). "span" alone, or "span,from" to place the
	 * left edge at an absolute moment.
	 *
	 * Interaction is the one thing a screenshot cannot exercise by
	 * itself, so this is how a zoomed graph gets checked the same way
	 * every other layout question in this project has been: by rendering
	 * it and looking.
	 */
	/*
	 * What the store actually holds (project.md sec 12). Opens, reports
	 * and exits without fetching anything -- the same shape as the other
	 * diagnostics, and the only way to see whether the archive is
	 * growing without waiting a month to find out it is not.
	 */
	/*
	 * A store somewhere other than the real one. For tests, for looking
	 * at an archive without opening it in the applet, and for the
	 * seeding below -- which must never be aimed at the real thing.
	 */
	const QString history_path =
	        option_value(arguments, QStringLiteral("--history-path"));

	/*
	 * Synthetic verification statistics, so the corrected band can be
	 * exercised before a month of real weather has gone by (sec 12.5).
	 *
	 * It REFUSES to write to the default store. The whole value of the
	 * archive is that everything in it was measured, and a diagnostic
	 * that can quietly put invented numbers there would destroy that for
	 * the sake of a screenshot.
	 */
	const QString seed =
	        option_value(arguments, QStringLiteral("--seed-verification"));
	if (!seed.isEmpty()) {
		QTextStream report(stdout);

		if (history_path.isEmpty()) {
			report << "seed: refusing to write invented statistics to the "
			       << "real archive.\n";
			report << "seed:   give --history-path with a scratch file.\n";
			return 1;
		}

		const double bias = seed.toDouble();
		const QString wanted =
		        station.isEmpty() ? bbq_settings::station() : station;

		bbq_history store;
		if (!store.open(history_path)) {
			report << "seed: cannot open: " << store.last_error() << "\n";
			return 1;
		}

		const bbq_band bands[] = {
			bbq_band::nowcast_fine, bbq_band::nowcast,
			bbq_band::hourly, bbq_band::extended};
		const bbq_lead_bucket buckets[] = {
			bbq_lead_bucket::hour, bbq_lead_bucket::three_hours,
			bbq_lead_bucket::six_hours, bbq_lead_bucket::twelve_hours,
			bbq_lead_bucket::day, bbq_lead_bucket::two_days,
			bbq_lead_bucket::four_days, bbq_lead_bucket::week,
			bbq_lead_bucket::beyond};

		int written = 0;
		int bucket_index = 0;

		for (bbq_lead_bucket bucket : buckets) {
			++bucket_index;

			for (bbq_band band : bands) {
				/*
				 * Growing with lead time, because that is the shape real
				 * forecast error has: a one-hour prediction is nearly
				 * right and a ten-day one is a guess. A flat synthetic
				 * bias would draw a corrected curve parallel to the
				 * forecast and would not exercise the stratification at
				 * all.
				 */
				const double scaled = bias * bucket_index;

				if (store.set_verification(wanted, band,
				                           QStringLiteral("temperature"), bucket,
				                           50, scaled, qAbs(scaled) + 0.5,
				                           qAbs(scaled) + 0.8)) {
					++written;
				}

				/*
				 * Rain in mm/h, so a tenth of the temperature figure --
				 * a band over-forecasting rain by half a degree's worth
				 * would be a downpour.
				 */
				if (store.set_verification(wanted, band,
				                           QStringLiteral("precip_rate"), bucket,
				                           50, scaled / 10.0,
				                           qAbs(scaled / 10.0) + 0.05,
				                           qAbs(scaled / 10.0) + 0.08)) {
					++written;
				}

				/* Wind in km/h, so a few times the temperature figure. */
				if (store.set_verification(wanted, band,
				                           QStringLiteral("wind_kph"), bucket, 50,
				                           scaled * 2.0, qAbs(scaled * 2.0) + 1.0,
				                           qAbs(scaled * 2.0) + 1.5)) {
					++written;
				}

				/*
				 * A reliability curve that drifts off the diagonal with
				 * lead time: the band over-predicts rain, and does so
				 * more the further ahead it looks. A curve sitting
				 * exactly on the diagonal would exercise the arithmetic
				 * without showing whether it can report a fault.
				 */
				for (int bin = 0; bin <= 10; ++bin) {
					const double said = bin / 10.0;
					const double drift = 0.04 * bucket_index;
					const double happened =
					        qBound(0.0, said - drift, 1.0);

					const int trials = 20;
					const int rained =
					        static_cast<int>(happened * trials + 0.5);
					const double error =
					        (said - happened) * (said - happened) * trials;

					if (store.set_reliability(wanted, band, bucket, bin, trials,
					                          rained, error)) {
						++written;
					}
				}
			}
		}

		report << "seed: wrote " << written << " synthetic rows to "
		       << history_path << "\n";
		report << "seed:   bias " << QString::number(bias, 'f', 2)
		       << " C per lead bucket, n=50 each\n";
		report << "seed:   THESE ARE INVENTED. Do not read them as measurements.\n";
		return 0;
	}

	if (arguments.contains(QStringLiteral("--history"))) {
		const QString wanted =
		        station.isEmpty() ? bbq_settings::station() : station;

		bbq_history store;
		QTextStream report(stdout);

		if (!store.open(history_path)) {
			report << "history: cannot open: " << store.last_error() << "\n";
			return 1;
		}

		report << "history: " << store.location() << "\n";
		report << "station: " << wanted << "\n";

		const int observations = store.observation_count(wanted);
		report << "observations: " << observations << "\n";

		if (observations > 0) {
			const QDateTime first =
			        QDateTime::fromSecsSinceEpoch(store.earliest_observation(wanted));
			report << "earliest: " << first.toString(Qt::ISODate) << "\n";
		}

		report << "forecasts awaiting a check: " << store.pending_count(wanted)
		       << "\n";

		const bbq_band bands[] = {
			bbq_band::nowcast_fine, bbq_band::nowcast,
			bbq_band::hourly, bbq_band::extended};
		const bbq_lead_bucket buckets[] = {
			bbq_lead_bucket::hour, bbq_lead_bucket::three_hours,
			bbq_lead_bucket::six_hours, bbq_lead_bucket::twelve_hours,
			bbq_lead_bucket::day, bbq_lead_bucket::two_days,
			bbq_lead_bucket::four_days, bbq_lead_bucket::week,
			bbq_lead_bucket::beyond};

		const QString quantities[] = {
			QStringLiteral("temperature"),
			QStringLiteral("precip_rate"),
			QStringLiteral("wind_kph")};

		bool any = false;

		for (const QString &quantity : quantities) {
			report << "\n" << quantity << " error, by band and lead time:\n";

			for (bbq_band band : bands) {
				for (bbq_lead_bucket bucket : buckets) {
					const bbq_verification score =
					        store.verification(wanted, band, quantity, bucket);

					if (score.count == 0) {
						continue;
					}

					any = true;
					report << "  " << bbq_band_name(band) << " at "
					       << bbq_lead_bucket_name(bucket) << ": n=" << score.count
					       << " bias=" << QString::number(score.bias, 'f', 2)
					       << " MAE="
					       << QString::number(score.mean_absolute_error, 'f', 2)
					       << " RMSE="
					       << QString::number(score.root_mean_square_error, 'f', 2)
					       << "\n";
				}
			}
		}

		/*
		 * Rain chance is scored differently and so is reported
		 * differently (sec 12.4). A percentage forecast is not wrong
		 * when it stays dry, so what is shown is the Brier score against
		 * the baseline it has to be read against, and the reliability
		 * curve underneath it: of all the times this band said forty
		 * percent, how often did it rain?
		 */
		report << "\nrain chance (Brier, lower is better):\n";

		for (bbq_band band : bands) {
			for (bbq_lead_bucket bucket : buckets) {
				const bbq_brier score = store.brier(wanted, band, bucket);
				if (score.count == 0) {
					continue;
				}

				any = true;
				report << "  " << bbq_band_name(band) << " at "
				       << bbq_lead_bucket_name(bucket) << ": n=" << score.count
				       << " Brier=" << QString::number(score.score, 'f', 3)
				       << " baseline=" << QString::number(score.baseline, 'f', 3)
				       << " skill=" << QString::number(score.skill(), 'f', 2)
				       << " (rained " << QString::number(score.base_rate * 100.0, 'f', 0)
				       << "% of the time)\n";

				const std::vector<bbq_reliability_bin> bins =
				        store.reliability(wanted, band, bucket);

				for (const bbq_reliability_bin &bin : bins) {
					if (bin.count == 0) {
						continue;
					}

					report << "      said "
					       << QString::number(bin.forecast() * 100.0, 'f', 0)
					       << "%, rained "
					       << QString::number(bin.observed() * 100.0, 'f', 0)
					       << "%  (n=" << bin.count << ")\n";
				}
			}
		}

		if (!any) {
			report << "  nothing verified yet -- a forecast is only checked "
			       << "once the hour it predicted has been observed\n";
		}

		return 0;
	}

	const QString view = option_value(arguments, QStringLiteral("--view"));
	if (!view.isEmpty()) {
		const QStringList parts = view.split(QLatin1Char(','));
		const qint64 span = parts.at(0).toLongLong();

		if (span > 0) {
			qint64 edge = QDateTime::currentSecsSinceEpoch() - span / 4;
			if (parts.size() == 2) {
				edge = parts.at(1).toLongLong();
			}

			window.graph()->set_view(edge, span);
		}
	}

	/* Wind is off by default; a shot may want it on. */
	if (arguments.contains(QStringLiteral("--wind"))) {
		window.set_show_wind(true);
	}

	const QString cursor = option_value(arguments, QStringLiteral("--cursor"));
	if (!cursor.isEmpty()) {
		window.graph()->set_cursor_column(cursor.toInt());
	}

	window.set_history_path(history_path);
	window.begin(station, geocode);

	/*
	 * The tray icon, rendered to a file. A tray cannot be screenshotted
	 * from here and the icon is now the applet's main surface, so this
	 * is the only way to look at what it says before shipping it.
	 */
	const QString tray_shot = option_value(arguments, QStringLiteral("--tray-icon"));

	const QString shot = option_value(arguments, QStringLiteral("--shot"));

	/*
	 * Render and exit. Bounded twice over: the shot is taken when the
	 * feed settles, and a wall-clock timer takes it regardless so a
	 * request that never answers cannot leave this running forever.
	 *
	 * EITHER option arms this, and that is a fix rather than a tidy-up.
	 * The condition used to be --shot alone, so --tray-icon on its own
	 * did exactly nothing: it wrote no file and never exited, because
	 * nothing was ever connected to quit. The README documents it as a
	 * diagnostic in its own right, and now it is one.
	 */
	/*
	 * Declared out here, not inside the block below, and that is a
	 * correctness matter rather than a placement preference.
	 *
	 * The lambdas that read it are invoked from timers during
	 * app.exec(), which outlives any inner scope -- so a `taken` local
	 * to the block was a dead stack slot by the time the shot fired.
	 * AddressSanitizer calls it a stack-use-after-scope and aborts on
	 * it; without the sanitizer the slot usually still held the right
	 * byte, which is why every screenshot this project reasoned about
	 * came out correct anyway. main's own frame is alive for the whole
	 * of exec(), so here it is genuinely alive whenever a timer fires.
	 */
	bool taken = false;

	if (!shot.isEmpty() || !tray_shot.isEmpty()) {
		const auto take = [&window, &tray, shot, tray_shot, want_layout, &taken]() {
			if (taken) {
				return;
			}
			taken = true;

			/*
			 * The mobile preview is given a phone's proportions here,
			 * at the last possible moment.
			 *
			 * A resize before show() does not survive, and one after it
			 * needs an event-loop turn to reach the widget -- the
			 * offscreen platform says as much when it warns that it
			 * does not propagate size hints. Doing it immediately
			 * before the grab is the only point where it is certain to
			 * have been applied.
			 *
			 * It lives here rather than in the layout because a phone
			 * does not resize its own window: the shape has to work at
			 * whatever size it is handed, and this is only how that
			 * gets looked at from a desktop.
			 */
			if (bbq_layout_resolve(want_layout) == bbq_layout::mobile) {
				window.resize(420, 860);
				QCoreApplication::processEvents();
			}

			if (!tray_shot.isEmpty()) {
				const QPixmap glyph = tray.icon().pixmap(44, 44);
				glyph.save(tray_shot);
				QTextStream(stdout) << "shot: wrote " << tray_shot << "\n";
			}

			if (!shot.isEmpty()) {
				const QPixmap picture = window.grab();
				QTextStream report(stdout);
				if (picture.save(shot)) {
					report << "shot: wrote " << shot << "\n";
				} else {
					report << "shot: could not write " << shot << "\n";
				}
			}

			QApplication::quit();
		};

		QObject::connect(window.feed(), &bbq_wu_feed::settled, &window,
		                 [take]() { QTimer::singleShot(300, take); });
		QTimer::singleShot(30000, &window, take);
	}

	return app.exec();
}
