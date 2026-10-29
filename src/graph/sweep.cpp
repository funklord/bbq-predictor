#include "graph/sweep.h"

#include <QDateTime>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <map>

#include "graph/forecast_graph.h"
#include "model/composite.h"

namespace {

/* Shown per quantity: enough to point at a place, not a table. */
const std::size_t moments_shown = 5;

bbq_sweep_quantity summarise(const QString &name,
                             const std::map<qint64, std::vector<double>> &values) {
	bbq_sweep_quantity quantity;
	quantity.name = name;

	for (const auto &[at, seen] : values) {
		if (seen.size() < 2) {
			continue;
		}

		/*
		 * Everything the value travelled, less the net change from the
		 * first frame to the last. A value that rises steadily as the
		 * zoom narrows travels exactly its net change and scores zero;
		 * one that goes back and forth scores what it wasted.
		 */
		double travelled = 0.0;
		for (std::size_t i = 1; i < seen.size(); ++i) {
			travelled += std::fabs(seen[i] - seen[i - 1]);
		}

		bbq_sweep_moment moment;
		moment.at_utc = at;
		moment.jitter = travelled - std::fabs(seen.back() - seen.front());
		moment.low = *std::min_element(seen.begin(), seen.end());
		moment.high = *std::max_element(seen.begin(), seen.end());
		quantity.total += moment.jitter;
		quantity.worst.push_back(moment);
	}

	std::sort(quantity.worst.begin(), quantity.worst.end(),
	          [](const bbq_sweep_moment &a, const bbq_sweep_moment &b) {
		          return a.jitter > b.jitter;
	          });
	if (quantity.worst.size() > moments_shown) {
		quantity.worst.resize(moments_shown);
	}
	return quantity;
}

} // namespace

bbq_sweep_result bbq_sweep(const bbq_composite &composite, qint64 about_utc, int width,
                           bbq_interpolation method, int smoothing_s) {
	bbq_sweep_result result;
	if (width <= 0) {
		return result;
	}

	std::map<qint64, std::vector<double>> temperature;
	std::map<qint64, std::vector<double>> rain;
	std::map<qint64, std::vector<double>> chance;

	/* Two percent a frame: about the step of one pinch update. */
	for (double span = 36 * 3600.0; span > 2 * 3600.0; span *= 0.98) {
		const double per = span / width;
		const qint64 from = static_cast<qint64>(about_utc - width * per / 3.0);
		const qint64 to = from + static_cast<qint64>(span);
		const std::vector<bbq_graph_column> columns =
		        bbq_graph_columns(composite, from, to, width, method, smoothing_s);
		const double seconds_per_column = static_cast<double>(to - from) / width;
		++result.frames;

		for (qint64 at = about_utc - 3600; at <= about_utc + 3 * 3600; at += 60) {
			/*
			 * The drawn line AT this moment: the graph joins one point
			 * per column with straight lines, each at its column's left
			 * edge, so the moment falls between two of them. Reading
			 * one column instead made every step in the data jitter by
			 * its whole height, as the column holding it began just
			 * before the step or just on it.
			 */
			const double position = (at - from) / seconds_per_column;
			const int x = static_cast<int>(std::floor(position));
			if (x < 0 || x + 1 >= width) {
				continue;
			}

			const bbq_graph_column &a = columns[static_cast<std::size_t>(x)];
			const bbq_graph_column &b = columns[static_cast<std::size_t>(x + 1)];
			const double t = position - x;
			const auto between = [t](double left, double right) {
				return left + (right - left) * t;
			};
			if (a.has_temperature && b.has_temperature) {
				temperature[at].push_back(between(a.temperature, b.temperature));
			}
			rain[at].push_back(between(a.has_rain ? a.rain : 0.0, b.has_rain ? b.rain : 0.0));
			chance[at].push_back(
			        between(a.has_chance ? a.chance : 0.0, b.has_chance ? b.chance : 0.0));
		}
	}

	result.quantities.push_back(summarise(QStringLiteral("temperature, C"), temperature));
	result.quantities.push_back(summarise(QStringLiteral("rain, mm/h"), rain));
	result.quantities.push_back(summarise(QStringLiteral("rain chance, %"), chance));
	return result;
}

QString bbq_sweep_report(const bbq_sweep_result &result, qint64 about_utc) {
	QString text;
	QTextStream out(&text);

	out << "sweep: " << result.frames << " frames, 36 h to 2 h about "
	    << QDateTime::fromSecsSinceEpoch(about_utc, bbq_utc())
	               .toString(QStringLiteral("MM-dd HH:mm'Z'"))
	    << "\n";

	for (const bbq_sweep_quantity &quantity : result.quantities) {
		out << "  " << quantity.name << ": jitter "
		    << QString::number(quantity.total, 'f', 2) << "\n";

		for (const bbq_sweep_moment &moment : quantity.worst) {
			if (moment.jitter <= 0.0) {
				break;
			}
			const qint64 minutes = (moment.at_utc - about_utc) / 60;
			out << "    " << (minutes < 0 ? "" : "+") << minutes << " min: "
			    << QString::number(moment.jitter, 'f', 2) << ", between "
			    << QString::number(moment.low, 'f', 2) << " and "
			    << QString::number(moment.high, 'f', 2) << "\n";
		}
	}

	return text;
}
