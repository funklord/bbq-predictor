#include "model/band_file.h"

#include <QFile>
#include <QStringList>
#include <QTextStream>

#include <optional>
#include <vector>

#include "model/composite.h"
#include "model/series.h"

namespace {

const char *const header = "bbq-bands 1";

const bbq_band every_band[] = {
	bbq_band::observed, bbq_band::current, bbq_band::nowcast_fine, bbq_band::nowcast,
	bbq_band::extended, bbq_band::corrected, bbq_band::hourly,
};

std::optional<bbq_band> band_named(const QString &name) {
	for (bbq_band band : every_band) {
		if (name == QLatin1String(bbq_band_name(band))) {
			return band;
		}
	}
	return std::nullopt;
}

/* An absent field is "-", so it cannot be mistaken for zero. */
QString field(const std::optional<double> &value) {
	return value.has_value() ? QString::number(*value, 'g', 17) : QStringLiteral("-");
}

bool parse_field(const QString &text, std::optional<double> *value) {
	if (text == QStringLiteral("-")) {
		value->reset();
		return true;
	}
	bool ok = false;
	const double parsed = text.toDouble(&ok);
	if (ok) {
		*value = parsed;
	}
	return ok;
}

} // namespace

bool bbq_write_band_file(const QString &path, const bbq_composite &composite,
                         qint64 about_utc, QString *why) {
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
		*why = file.errorString();
		return false;
	}

	QTextStream out(&file);
	out << header << "\n";
	out << "about\t" << about_utc << "\n";
	out << "zone\t" << QString::fromUtf8(composite.zone().id()) << "\n";

	for (const bbq_series &series : composite.all()) {
		out << "band\t" << bbq_band_name(series.band()) << "\t" << series.fetched_utc()
		    << "\t" << series.provider() << "\n";
		for (const bbq_sample &sample : series.samples()) {
			out << sample.start_utc << "\t" << sample.duration_s << "\t"
			    << field(sample.temperature) << "\t" << field(sample.precip_rate) << "\t"
			    << field(sample.precip_chance) << "\t" << field(sample.wind_kph) << "\n";
		}
	}

	out.flush();
	if (out.status() != QTextStream::Ok || !file.flush()) {
		*why = file.errorString();
		return false;
	}
	return true;
}

bool bbq_read_band_file(const QString &path, bbq_composite *composite,
                        qint64 *about_utc, QString *why) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		*why = file.errorString();
		return false;
	}

	QTextStream in(&file);
	if (in.readLine() != QLatin1String(header)) {
		*why = QStringLiteral("not a band file: the first line is not \"%1\"")
		               .arg(QLatin1String(header));
		return false;
	}

	QTimeZone zone;
	bool have_about = false;
	std::vector<bbq_series> bands;
	std::vector<std::vector<bbq_sample>> samples;
	int line_number = 1;

	while (!in.atEnd()) {
		const QString line = in.readLine();
		++line_number;
		if (line.isEmpty()) {
			continue;
		}

		const QStringList parts = line.split(QLatin1Char('\t'));
		const auto refuse = [&](const QString &what) {
			*why = QStringLiteral("line %1: %2").arg(line_number).arg(what);
			return false;
		};

		if (parts.at(0) == QStringLiteral("about") && parts.size() == 2) {
			bool ok = false;
			*about_utc = parts.at(1).toLongLong(&ok);
			if (!ok) {
				return refuse(QStringLiteral("the moment is not a number"));
			}
			have_about = true;
		} else if (parts.at(0) == QStringLiteral("zone") && parts.size() == 2) {
			zone = QTimeZone(parts.at(1).toUtf8());
		} else if (parts.at(0) == QStringLiteral("band") && parts.size() == 4) {
			const std::optional<bbq_band> band = band_named(parts.at(1));
			if (!band.has_value()) {
				return refuse(QStringLiteral("no band is called \"%1\"").arg(parts.at(1)));
			}
			bands.emplace_back(*band, parts.at(3));
			bands.back().set_fetched_utc(parts.at(2).toLongLong());
			samples.emplace_back();
		} else if (parts.size() == 6) {
			if (bands.empty()) {
				return refuse(QStringLiteral("a sample before any band"));
			}
			bbq_sample sample;
			bool start_ok = false;
			bool duration_ok = false;
			sample.start_utc = parts.at(0).toLongLong(&start_ok);
			sample.duration_s = parts.at(1).toInt(&duration_ok);
			if (!start_ok || !duration_ok ||
			    !parse_field(parts.at(2), &sample.temperature) ||
			    !parse_field(parts.at(3), &sample.precip_rate) ||
			    !parse_field(parts.at(4), &sample.precip_chance) ||
			    !parse_field(parts.at(5), &sample.wind_kph)) {
				return refuse(QStringLiteral("a sample that does not read"));
			}
			samples.back().push_back(sample);
		} else {
			return refuse(QStringLiteral("not a line this file has"));
		}
	}

	if (!have_about) {
		*why = QStringLiteral("no \"about\" line, so nothing says when the bands are for");
		return false;
	}

	*composite = bbq_composite();
	for (std::size_t i = 0; i < bands.size(); ++i) {
		bands[i].set_samples(std::move(samples[i]));
		if (zone.isValid()) {
			bands[i].set_zone(zone);
		}
		composite->set_series(std::move(bands[i]));
	}
	return true;
}
