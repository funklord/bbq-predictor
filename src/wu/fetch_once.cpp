#include "wu/fetch_once.h"

#include <QDate>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTextStream>
#include <QTimer>

#include "wu/client.h"
#include "wu/key_source.h"

namespace {

/*
 * Describe a response without pretending to model it.
 *
 * The two shapes are genuinely different and sec 2.6.3 says so: the v3
 * endpoints are column-oriented parallel arrays, one per field, while
 * the v2 PWS endpoints are row-oriented with their values nested under
 * a unit-system key. This prints whichever it finds rather than forcing
 * one into the other's shape -- that reconciliation is the next piece
 * of work and is not being pre-empted here.
 */
QString describe(const QJsonDocument &document) {
	if (!document.isObject()) {
		return QStringLiteral("not a JSON object");
	}

	const QJsonObject root = document.object();

	/* Row-oriented: the PWS shape. */
	if (root.contains(QStringLiteral("observations"))) {
		const QJsonArray rows = root.value(QStringLiteral("observations")).toArray();
		if (rows.isEmpty()) {
			return QStringLiteral("0 observation rows");
		}

		const QJsonObject first = rows.first().toObject();
		QStringList nested;
		for (auto it = first.begin(); it != first.end(); ++it) {
			if (it.value().isObject()) {
				nested.append(it.key());
			}
		}

		QString blocks = nested.join(QStringLiteral(", "));
		if (blocks.isEmpty()) {
			blocks = QStringLiteral("none");
		}

		QString summary = QString::number(rows.size());
		summary += QStringLiteral(" rows (row-oriented), unit blocks: ");
		summary += blocks;
		return summary;
	}

	/* Column-oriented: the v3 shape. */
	int longest = 0;
	QStringList fields;
	for (auto it = root.begin(); it != root.end(); ++it) {
		if (it.value().isArray()) {
			longest = qMax(longest, it.value().toArray().size());
		}
		fields.append(it.key());
	}

	if (longest == 0) {
		return QStringLiteral("%1 fields, no arrays").arg(fields.size());
	}

	QString summary = QString::number(longest);
	summary += QStringLiteral(" points across ");
	summary += QString::number(fields.size());
	summary += QStringLiteral(" parallel arrays (column-oriented)");
	return summary;
}

/* Where the station says it is (project.md sec 2.6.7.1). */
struct station_point {
	bool known = false;
	double latitude = 0.0;
	double longitude = 0.0;
};

station_point point_from_observed(const QJsonDocument &document) {
	station_point point;

	const QJsonObject root = document.object();
	const QJsonValue rows_value = root.value(QStringLiteral("observations"));
	const QJsonArray rows = rows_value.toArray();
	if (rows.isEmpty()) {
		return point;
	}

	const QJsonObject first = rows.first().toObject();
	const bool has_lat = first.contains(QStringLiteral("lat"));
	const bool has_lon = first.contains(QStringLiteral("lon"));
	if (!has_lat || !has_lon) {
		return point;
	}

	point.known = true;
	point.latitude = first.value(QStringLiteral("lat")).toDouble();
	point.longitude = first.value(QStringLiteral("lon")).toDouble();
	return point;
}

} // namespace

int bbq_wu_fetch_once(const QString &station_id, const QString &geocode,
                      int timeout_s) {
	QTextStream out(stdout);
	QTextStream error(stderr);

	double latitude = 0.0;
	double longitude = 0.0;
	bool have_geocode = false;

	if (!geocode.isEmpty()) {
		const QStringList parts = geocode.split(QLatin1Char(','));
		if (parts.size() != 2) {
			error << "fetch-once: --geocode wants LAT,LON\n";
			return 2;
		}
		latitude = parts.at(0).toDouble();
		longitude = parts.at(1).toDouble();
		have_geocode = true;
	}

	if (station_id.isEmpty() && !have_geocode) {
		error << "fetch-once: nothing configured.\n";
		error << "fetch-once:   Give --station ID, or --geocode LAT,LON, "
		         "or both.\n";
		return 2;
	}

	QNetworkAccessManager net;
	bbq_wu_key_source keys(&net);
	bbq_wu_client client(&net, &keys);

	QEventLoop loop;
	int outstanding = 0;
	int failures = 0;

	const auto settle = [&]() {
		if (--outstanding <= 0) {
			loop.quit();
		}
	};

	QObject::connect(&client, &bbq_wu_client::ready, &loop,
	                 [&](bbq_wu_product product, const QJsonDocument &doc) {
		out << QStringLiteral("  %1  ok    %2\n")
		                .arg(QString::fromLatin1(bbq_wu_product_name(product)),
		                     -9)
		                .arg(describe(doc));
		out.flush();

		/*
		 * Derive the geocode from the station and release the two
		 * bands that were waiting on it (sec 2.6.7). The band that
		 * needs the station supplies the coordinate the others need,
		 * so there is no lookup endpoint and no second round trip.
		 */
		if (product == bbq_wu_product::observed && !have_geocode) {
			const station_point point = point_from_observed(doc);
			if (point.known) {
				have_geocode = true;
				latitude = point.latitude;
				longitude = point.longitude;
				out << "  geocode derived from station: ";
				out << latitude << "," << longitude << "\n";
				out.flush();
				outstanding += 2;
				client.fetch_nowcast(latitude, longitude);
				client.fetch_hourly(latitude, longitude);
			} else {
				error << "  the station reported no coordinates; the ";
				error << "forecast bands cannot be placed\n";
				++failures;
			}
		}

		settle();
	});

	QObject::connect(&client, &bbq_wu_client::failed, &loop,
	                 [&](bbq_wu_product product, const QString &reason) {
		error << QStringLiteral("  %1  FAIL  %2\n")
		                 .arg(QString::fromLatin1(bbq_wu_product_name(product)),
		                      -9)
		                 .arg(reason);
		error.flush();
		++failures;
		settle();
	});

	/*
	 * The termination condition, and it is not the requests.
	 *
	 * Every band settling would normally end the loop, but a connection
	 * that never answers settles nothing. This is the bound that makes
	 * the run finite whatever the network does, and it lives here
	 * rather than in whatever invokes the binary -- a wrapper only
	 * guards the way somebody did not run it.
	 */
	QTimer::singleShot(timeout_s * 1000, &loop, [&]() {
		error << QStringLiteral("fetch-once: timed out after %1s with %2 "
		                        "request(s) unanswered\n")
		                 .arg(timeout_s)
		                 .arg(outstanding);
		failures += outstanding;
		outstanding = 0;
		loop.quit();
	});

	out << "fetch-once: fetching every band once\n";

	if (!station_id.isEmpty()) {
		++outstanding;
		client.fetch_observed(station_id,
		                      QDate::currentDate().toString(
		                              QStringLiteral("yyyyMMdd")));
	} else {
		out << "  observed   skipped -- no station pinned, which is a "
		       "normal state\n";
	}

	if (have_geocode) {
		outstanding += 2;
		client.fetch_nowcast(latitude, longitude);
		client.fetch_hourly(latitude, longitude);
	}

	out.flush();
	loop.exec();

	if (failures > 0) {
		error << QStringLiteral("fetch-once: %1 band(s) failed\n").arg(failures);
		return 1;
	}

	out << "fetch-once: every band answered\n";
	return 0;
}
