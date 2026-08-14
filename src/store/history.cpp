#include "store/history.h"

#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

#include <cmath>

namespace {

/*
 * Rain is taken to have occurred above this rate. A threshold, and
 * labelled as one (project.md sec 12.4) rather than left as a bare
 * number in a comparison.
 */
const double rain_occurred_mm_h = 0.1;

/*
 * When a quantity's observations stop counting as measurements
 * (project.md sec 12.14).
 *
 * A sensor that reports the same number for six hours is not measuring
 * anything, and scoring a forecast against it produces a bias that
 * looks exactly like a real one. Found on the station this project was
 * written against: 288 observations in a day, tempAvg 22 for every one
 * of them, dew point also 22 and humidity 99 -- a soaked or enclosed
 * probe -- while its wind and pressure moved normally. The temperature
 * verification came out at -6.67 C, which is not a forecast error but
 * the distance from the weather to a stuck probe.
 *
 * That number does not merely sit in a table. The corrected band is
 * drawn from it, so a stuck sensor becomes a curve on the graph with
 * the authority of a measurement behind it.
 *
 * Six hours and twenty-four samples, deliberately conservative. An hour
 * of unchanging temperature is ordinary weather, especially at the
 * whole-degree quantisation this source reports; six hours of it,
 * across a sunrise or a sunset, is a fault. The test is exact equality
 * rather than a tolerance, because the fault this catches is a repeated
 * number and a tolerance would start refusing calm days.
 */
const int stuck_minimum_samples = 24;
const qint64 stuck_minimum_span_s = 6 * 3600;

/*
 * How long past its valid time a queued forecast waits before it is
 * given up on. A station that goes quiet for a day should not leave the
 * queue holding rows nothing will ever check.
 */
const qint64 give_up_after_s = 36 * 3600;

/*
 * How far apart a forecast's valid time and an observation may be and
 * still be considered the same moment.
 *
 * The bands do not share a clock: an hourly forecast lands on the hour
 * and the station reports whenever it feels like it, so demanding an
 * exact match would verify almost nothing. Half the station's nominal
 * cadence is the widest that cannot match the wrong sample.
 */
const qint64 match_within_s = 150;

QString quantity_name(int index) {
	switch (index) {
	case 0:
		return QStringLiteral("temperature");
	case 1:
		return QStringLiteral("precip_rate");
	case 2:
		return QStringLiteral("wind_kph");
	}

	return QStringLiteral("unknown");
}

} // namespace

bbq_lead_bucket bbq_lead_bucket_for(qint64 lead_s) {
	if (lead_s <= 3600) {
		return bbq_lead_bucket::hour;
	}
	if (lead_s <= 3 * 3600) {
		return bbq_lead_bucket::three_hours;
	}
	if (lead_s <= 6 * 3600) {
		return bbq_lead_bucket::six_hours;
	}
	if (lead_s <= 12 * 3600) {
		return bbq_lead_bucket::twelve_hours;
	}
	if (lead_s <= 24 * 3600) {
		return bbq_lead_bucket::day;
	}
	if (lead_s <= 2 * 24 * 3600) {
		return bbq_lead_bucket::two_days;
	}
	if (lead_s <= 4 * 24 * 3600) {
		return bbq_lead_bucket::four_days;
	}
	if (lead_s <= 7 * 24 * 3600) {
		return bbq_lead_bucket::week;
	}

	return bbq_lead_bucket::beyond;
}

const char *bbq_lead_bucket_name(bbq_lead_bucket bucket) {
	switch (bucket) {
	case bbq_lead_bucket::hour:
		return "1h";
	case bbq_lead_bucket::three_hours:
		return "3h";
	case bbq_lead_bucket::six_hours:
		return "6h";
	case bbq_lead_bucket::twelve_hours:
		return "12h";
	case bbq_lead_bucket::day:
		return "1d";
	case bbq_lead_bucket::two_days:
		return "2d";
	case bbq_lead_bucket::four_days:
		return "4d";
	case bbq_lead_bucket::week:
		return "7d";
	case bbq_lead_bucket::beyond:
		return "7d+";
	}

	return "?";
}

qint64 bbq_lead_bucket_centre_s(bbq_lead_bucket bucket) {
	switch (bucket) {
	case bbq_lead_bucket::hour:
		return 1800;
	case bbq_lead_bucket::three_hours:
		return 2 * 3600;
	case bbq_lead_bucket::six_hours:
		return 4 * 3600 + 1800;
	case bbq_lead_bucket::twelve_hours:
		return 9 * 3600;
	case bbq_lead_bucket::day:
		return 18 * 3600;
	case bbq_lead_bucket::two_days:
		return 36 * 3600;
	case bbq_lead_bucket::four_days:
		return 3 * 24 * 3600;
	case bbq_lead_bucket::week:
		return 5 * 24 * 3600 + 12 * 3600;
	case bbq_lead_bucket::beyond:
		return 11 * 24 * 3600;
	}

	return 3600;
}

bbq_history::bbq_history() {
	/*
	 * A connection name of our own, so two of these -- or a test and the
	 * applet in one process -- do not fight over Qt's default handle.
	 */
	static int serial = 0;
	m_connection = QStringLiteral("bbq_history_%1").arg(++serial);
}

bbq_history::~bbq_history() {
	if (m_open) {
		QSqlDatabase::database(m_connection).close();
		m_open = false;
	}

	QSqlDatabase::removeDatabase(m_connection);
}

bool bbq_history::exec(const QString &statement) {
	QSqlQuery query(QSqlDatabase::database(m_connection));

	if (query.exec(statement)) {
		return true;
	}

	m_last_error = query.lastError().text();
	return false;
}

bool bbq_history::open(const QString &path) {
	m_path = path;

	if (m_path.isEmpty()) {
		/*
		 * AppDataLocation, not AppConfigLocation. The INI is a
		 * preference a person edits; this is measurement, and putting
		 * megabytes of it in a config directory would be filing it
		 * under the wrong thing (sec 12.2).
		 */
		const QString directory = QStandardPaths::writableLocation(
		        QStandardPaths::AppDataLocation);
		QDir().mkpath(directory);
		m_path = directory + QStringLiteral("/history.sqlite");
	}

	QSqlDatabase database =
	        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
	database.setDatabaseName(m_path);

	if (!database.open()) {
		m_last_error = database.lastError().text();
		return false;
	}

	m_open = true;

	/*
	 * Write-ahead logging, because two copies of the applet open at once
	 * is a real case on this machine and the default journal makes one
	 * of them block the other. Synchronous NORMAL is the usual companion:
	 * a crash can lose the last transaction, and the last transaction is
	 * five minutes of weather that will be re-fetched anyway.
	 */
	exec(QStringLiteral("PRAGMA journal_mode = WAL"));
	exec(QStringLiteral("PRAGMA synchronous = NORMAL"));

	return create_schema();
}

bool bbq_history::create_schema() {
	/*
	 * Observations. The primary key is what makes re-storing harmless:
	 * the same station reporting the same instant twice is one row, so a
	 * refresh that overlaps the last one costs nothing.
	 */
	if (!exec(QStringLiteral(
	            "CREATE TABLE IF NOT EXISTS observation ("
	            "station TEXT NOT NULL,"
	            "valid_utc INTEGER NOT NULL,"
	            "duration_s INTEGER NOT NULL,"
	            "temperature REAL,"
	            "precip_rate REAL,"
	            "wind_kph REAL,"
	            "provider TEXT NOT NULL,"
	            "PRIMARY KEY (station, valid_utc))"))) {
		return false;
	}

	/*
	 * The queue. The UNIQUE index is the bound described in sec 12.6 --
	 * one forecast per band per valid time per lead bucket, so
	 * re-forecasting the same hour every refresh stores it once per
	 * bucket rather than once per fetch.
	 */
	if (!exec(QStringLiteral(
	            "CREATE TABLE IF NOT EXISTS forecast_pending ("
	            "station TEXT NOT NULL,"
	            "band INTEGER NOT NULL,"
	            "lead_bucket INTEGER NOT NULL,"
	            "issued_utc INTEGER NOT NULL,"
	            "valid_utc INTEGER NOT NULL,"
	            "temperature REAL,"
	            "precip_rate REAL,"
	            "precip_chance REAL,"
	            "wind_kph REAL,"
	            "PRIMARY KEY (station, band, valid_utc, lead_bucket))"))) {
		return false;
	}

	if (!exec(QStringLiteral(
	            "CREATE INDEX IF NOT EXISTS forecast_pending_valid "
	            "ON forecast_pending (station, valid_utc)"))) {
		return false;
	}

	/*
	 * Sums, not samples. Everything the field's usual scores need can be
	 * recovered from these three and the count: bias from the signed
	 * sum, MAE from the absolute one, RMSE from the squared one.
	 */
	if (!exec(QStringLiteral(
	            "CREATE TABLE IF NOT EXISTS verification ("
	            "station TEXT NOT NULL,"
	            "band INTEGER NOT NULL,"
	            "quantity TEXT NOT NULL,"
	            "lead_bucket INTEGER NOT NULL,"
	            "count INTEGER NOT NULL,"
	            "sum_error REAL NOT NULL,"
	            "sum_absolute_error REAL NOT NULL,"
	            "sum_square_error REAL NOT NULL,"
	            "PRIMARY KEY (station, band, quantity, lead_bucket))"))) {
		return false;
	}

	/*
	 * Rain chance is scored differently and so is stored differently
	 * (sec 12.4). A percentage forecast is not wrong when it stays dry,
	 * so what is accumulated is occurrence against probability bin --
	 * the makings of a Brier score and a reliability curve, not a mean
	 * error.
	 */
	return exec(QStringLiteral(
	        "CREATE TABLE IF NOT EXISTS reliability ("
	        "station TEXT NOT NULL,"
	        "band INTEGER NOT NULL,"
	        "lead_bucket INTEGER NOT NULL,"
	        "probability_bin INTEGER NOT NULL,"
	        "count INTEGER NOT NULL,"
	        "rain_count INTEGER NOT NULL,"
	        "sum_square_error REAL NOT NULL,"
	        "PRIMARY KEY (station, band, lead_bucket, probability_bin))"));
}

int bbq_history::record_observations(const QString &station,
                                     const bbq_series &series) {
	if (!m_open || series.is_empty()) {
		return 0;
	}

	QSqlDatabase database = QSqlDatabase::database(m_connection);
	database.transaction();

	QSqlQuery query(database);
	query.prepare(QStringLiteral(
	        "INSERT OR REPLACE INTO observation "
	        "(station, valid_utc, duration_s, temperature, precip_rate, "
	        "wind_kph, provider) VALUES (?, ?, ?, ?, ?, ?, ?)"));

	int stored = 0;
	for (const bbq_sample &sample : series.samples()) {
		query.addBindValue(station);
		query.addBindValue(sample.start_utc);
		query.addBindValue(sample.duration_s);
		query.addBindValue(sample.temperature.has_value()
		                           ? QVariant(*sample.temperature)
		                           : QVariant());
		query.addBindValue(sample.precip_rate.has_value()
		                           ? QVariant(*sample.precip_rate)
		                           : QVariant());
		query.addBindValue(sample.wind_kph.has_value()
		                           ? QVariant(*sample.wind_kph)
		                           : QVariant());
		query.addBindValue(series.provider());

		if (query.exec()) {
			++stored;
		} else {
			m_last_error = query.lastError().text();
		}
	}

	database.commit();
	return stored;
}

int bbq_history::record_forecast(const QString &station,
                                 const bbq_series &series, qint64 issued_utc) {
	if (!m_open || series.is_empty()) {
		return 0;
	}

	QSqlDatabase database = QSqlDatabase::database(m_connection);
	database.transaction();

	/*
	 * IGNORE, not REPLACE. The first forecast seen in a bucket is the
	 * one kept, so a bucket's verification always describes a prediction
	 * made at that lead time rather than the most recent revision of it.
	 */
	QSqlQuery query(database);
	query.prepare(QStringLiteral(
	        "INSERT OR IGNORE INTO forecast_pending "
	        "(station, band, lead_bucket, issued_utc, valid_utc, temperature, "
	        "precip_rate, precip_chance, wind_kph) "
	        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));

	int stored = 0;
	for (const bbq_sample &sample : series.samples()) {
		const qint64 lead = sample.start_utc - issued_utc;
		if (lead <= 0) {
			/* Already happened when it was fetched; nothing to predict. */
			continue;
		}

		query.addBindValue(station);
		query.addBindValue(static_cast<int>(series.band()));
		query.addBindValue(static_cast<int>(bbq_lead_bucket_for(lead)));
		query.addBindValue(issued_utc);
		query.addBindValue(sample.start_utc);
		query.addBindValue(sample.temperature.has_value()
		                           ? QVariant(*sample.temperature)
		                           : QVariant());
		query.addBindValue(sample.precip_rate.has_value()
		                           ? QVariant(*sample.precip_rate)
		                           : QVariant());
		query.addBindValue(sample.precip_chance.has_value()
		                           ? QVariant(*sample.precip_chance)
		                           : QVariant());
		query.addBindValue(sample.wind_kph.has_value()
		                           ? QVariant(*sample.wind_kph)
		                           : QVariant());

		if (query.exec()) {
			stored += query.numRowsAffected() > 0 ? 1 : 0;
		} else {
			m_last_error = query.lastError().text();
		}
	}

	database.commit();
	return stored;
}

int bbq_history::verify(const QString &station) {
	if (!m_open) {
		return 0;
	}

	QSqlDatabase database = QSqlDatabase::database(m_connection);

	/*
	 * Every queued forecast that now has an observation near its valid
	 * time, paired with it in one statement. Doing the matching in SQL
	 * rather than in a loop of queries is what keeps this cheap as the
	 * observation table grows.
	 */
	QSqlQuery find(database);
	find.prepare(QStringLiteral(
	        "SELECT f.band, f.lead_bucket, f.valid_utc, f.temperature, "
	        "f.precip_rate, f.precip_chance, f.wind_kph, "
	        "o.temperature, o.precip_rate, o.wind_kph "
	        "FROM forecast_pending f JOIN observation o "
	        "ON o.station = f.station "
	        "AND o.valid_utc BETWEEN f.valid_utc - ? AND f.valid_utc + ? "
	        "WHERE f.station = ?"));
	find.addBindValue(match_within_s);
	find.addBindValue(match_within_s);
	find.addBindValue(station);

	if (!find.exec()) {
		m_last_error = find.lastError().text();
		return 0;
	}

	struct pairing {
		int band = 0;
		int bucket = 0;
		qint64 valid_utc = 0;
		double forecast[3] = {0.0, 0.0, 0.0};
		bool have_forecast[3] = {false, false, false};
		double observed[3] = {0.0, 0.0, 0.0};
		bool have_observed[3] = {false, false, false};
		double chance = 0.0;
		bool have_chance = false;
		bool rained = false;
		bool know_rain = false;
	};

	std::vector<pairing> pairings;

	while (find.next()) {
		pairing found;
		found.band = find.value(0).toInt();
		found.bucket = find.value(1).toInt();
		found.valid_utc = find.value(2).toLongLong();

		found.have_forecast[0] = !find.value(3).isNull();
		found.forecast[0] = find.value(3).toDouble();
		found.have_forecast[1] = !find.value(4).isNull();
		found.forecast[1] = find.value(4).toDouble();
		found.have_chance = !find.value(5).isNull();
		found.chance = find.value(5).toDouble();
		found.have_forecast[2] = !find.value(6).isNull();
		found.forecast[2] = find.value(6).toDouble();

		found.have_observed[0] = !find.value(7).isNull();
		found.observed[0] = find.value(7).toDouble();
		found.have_observed[1] = !find.value(8).isNull();
		found.observed[1] = find.value(8).toDouble();
		found.have_observed[2] = !find.value(9).isNull();
		found.observed[2] = find.value(9).toDouble();

		if (found.have_observed[1]) {
			found.know_rain = true;
			found.rained = found.observed[1] > rain_occurred_mm_h;
		}

		pairings.push_back(found);
	}

	if (pairings.empty()) {
		return 0;
	}

	database.transaction();

	QSqlQuery fold(database);
	fold.prepare(QStringLiteral(
	        "INSERT INTO verification "
	        "(station, band, quantity, lead_bucket, count, sum_error, "
	        "sum_absolute_error, sum_square_error) VALUES (?, ?, ?, ?, 1, ?, ?, ?) "
	        "ON CONFLICT (station, band, quantity, lead_bucket) DO UPDATE SET "
	        "count = count + 1, "
	        "sum_error = sum_error + excluded.sum_error, "
	        "sum_absolute_error = sum_absolute_error + excluded.sum_absolute_error, "
	        "sum_square_error = sum_square_error + excluded.sum_square_error"));

	QSqlQuery reliability(database);
	reliability.prepare(QStringLiteral(
	        "INSERT INTO reliability "
	        "(station, band, lead_bucket, probability_bin, count, rain_count, "
	        "sum_square_error) VALUES (?, ?, ?, ?, 1, ?, ?) "
	        "ON CONFLICT (station, band, lead_bucket, probability_bin) DO UPDATE SET "
	        "count = count + 1, "
	        "rain_count = rain_count + excluded.rain_count, "
	        "sum_square_error = sum_square_error + excluded.sum_square_error"));

	QSqlQuery drop(database);
	drop.prepare(QStringLiteral(
	        "DELETE FROM forecast_pending WHERE station = ? AND band = ? "
	        "AND valid_utc = ? AND lead_bucket = ?"));

	int verified = 0;

	/*
	 * Which quantities are being measured at all, decided before any of
	 * them is scored. See the note on stuck_minimum_span_s.
	 */
	bool stuck[3] = {false, false, false};

	for (int q = 0; q < 3; ++q) {
		int seen = 0;
		double lowest = 0.0;
		double highest = 0.0;
		qint64 earliest = 0;
		qint64 latest = 0;

		for (const pairing &found : pairings) {
			if (!found.have_observed[q]) {
				continue;
			}

			if (seen == 0) {
				lowest = found.observed[q];
				highest = found.observed[q];
				earliest = found.valid_utc;
				latest = found.valid_utc;
			}

			lowest = std::min(lowest, found.observed[q]);
			highest = std::max(highest, found.observed[q]);
			earliest = std::min(earliest, found.valid_utc);
			latest = std::max(latest, found.valid_utc);
			++seen;
		}

		const bool enough = seen >= stuck_minimum_samples;
		const bool spans = (latest - earliest) >= stuck_minimum_span_s;
		stuck[q] = enough && spans && highest == lowest;

		if (stuck[q]) {
			/*
			 * Said out loud. A quantity quietly missing from the
			 * record is the shape of fault this whole section keeps
			 * finding, so refusing to score one announces itself.
			 */
			qWarning("bbq-predictor: %s at %s never changed across %lld "
			         "hours of %d observations; not scoring it",
			         quantity_name(q).toUtf8().constData(),
			         station.toUtf8().constData(),
			         static_cast<long long>((latest - earliest) / 3600),
			         seen);
		}
	}

	for (const pairing &found : pairings) {
		for (int q = 0; q < 3; ++q) {
			if (stuck[q]) {
				continue;
			}

			if (!found.have_forecast[q] || !found.have_observed[q]) {
				continue;
			}

			const double error = found.forecast[q] - found.observed[q];

			fold.addBindValue(station);
			fold.addBindValue(found.band);
			fold.addBindValue(quantity_name(q));
			fold.addBindValue(found.bucket);
			fold.addBindValue(error);
			fold.addBindValue(std::fabs(error));
			fold.addBindValue(error * error);

			if (!fold.exec()) {
				m_last_error = fold.lastError().text();
			}
		}

		if (found.have_chance && found.know_rain) {
			/*
			 * Deciles. The bin is the forecast probability rounded to a
			 * tenth, and what is scored against it is whether rain
			 * actually happened -- the Brier score is the mean of that
			 * squared difference.
			 */
			const double probability = found.chance / 100.0;
			const double outcome = found.rained ? 1.0 : 0.0;
			const double error = probability - outcome;

			reliability.addBindValue(station);
			reliability.addBindValue(found.band);
			reliability.addBindValue(found.bucket);
			reliability.addBindValue(static_cast<int>(probability * 10.0 + 0.5));
			reliability.addBindValue(found.rained ? 1 : 0);
			reliability.addBindValue(error * error);

			if (!reliability.exec()) {
				m_last_error = reliability.lastError().text();
			}
		}

		drop.addBindValue(station);
		drop.addBindValue(found.band);
		drop.addBindValue(found.valid_utc);
		drop.addBindValue(found.bucket);

		if (drop.exec()) {
			++verified;
		} else {
			m_last_error = drop.lastError().text();
		}
	}

	database.commit();
	return verified;
}

int bbq_history::expire(const QString &station, qint64 now_utc) {
	if (!m_open) {
		return 0;
	}

	QSqlQuery query(QSqlDatabase::database(m_connection));
	query.prepare(QStringLiteral(
	        "DELETE FROM forecast_pending WHERE station = ? AND valid_utc < ?"));
	query.addBindValue(station);
	query.addBindValue(now_utc - give_up_after_s);

	if (!query.exec()) {
		m_last_error = query.lastError().text();
		return 0;
	}

	return query.numRowsAffected();
}

bool bbq_history::set_verification(const QString &station, bbq_band band,
                                   const QString &quantity,
                                   bbq_lead_bucket bucket, int count,
                                   double bias, double mean_absolute_error,
                                   double root_mean_square_error) {
	if (!m_open || count <= 0) {
		return false;
	}

	/*
	 * Stored as the sums the table actually holds, so a seeded row is
	 * indistinguishable from an accumulated one and verify() can keep
	 * adding to it afterwards.
	 */
	QSqlQuery query(QSqlDatabase::database(m_connection));
	query.prepare(QStringLiteral(
	        "INSERT OR REPLACE INTO verification "
	        "(station, band, quantity, lead_bucket, count, sum_error, "
	        "sum_absolute_error, sum_square_error) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
	query.addBindValue(station);
	query.addBindValue(static_cast<int>(band));
	query.addBindValue(quantity);
	query.addBindValue(static_cast<int>(bucket));
	query.addBindValue(count);
	query.addBindValue(bias * count);
	query.addBindValue(mean_absolute_error * count);
	query.addBindValue(root_mean_square_error * root_mean_square_error * count);

	if (!query.exec()) {
		m_last_error = query.lastError().text();
		return false;
	}

	return true;
}

bool bbq_history::set_reliability(const QString &station, bbq_band band,
                                  bbq_lead_bucket bucket, int probability_bin,
                                  int count, int rain_count,
                                  double sum_square_error) {
	if (!m_open || count <= 0) {
		return false;
	}

	QSqlQuery query(QSqlDatabase::database(m_connection));
	query.prepare(QStringLiteral(
	        "INSERT OR REPLACE INTO reliability "
	        "(station, band, lead_bucket, probability_bin, count, rain_count, "
	        "sum_square_error) VALUES (?, ?, ?, ?, ?, ?, ?)"));
	query.addBindValue(station);
	query.addBindValue(static_cast<int>(band));
	query.addBindValue(static_cast<int>(bucket));
	query.addBindValue(probability_bin);
	query.addBindValue(count);
	query.addBindValue(rain_count);
	query.addBindValue(sum_square_error);

	if (!query.exec()) {
		m_last_error = query.lastError().text();
		return false;
	}

	return true;
}

bbq_brier bbq_history::brier(const QString &station, bbq_band band,
                             bbq_lead_bucket bucket) const {
	bbq_brier result;

	if (!m_open) {
		return result;
	}

	QSqlQuery query(QSqlDatabase::database(m_connection));
	query.prepare(QStringLiteral(
	        "SELECT SUM(count), SUM(rain_count), SUM(sum_square_error) "
	        "FROM reliability WHERE station = ? AND band = ? AND lead_bucket = ?"));
	query.addBindValue(station);
	query.addBindValue(static_cast<int>(band));
	query.addBindValue(static_cast<int>(bucket));

	if (!query.exec() || !query.next() || query.value(0).isNull()) {
		return result;
	}

	result.count = query.value(0).toInt();
	if (result.count <= 0) {
		result.count = 0;
		return result;
	}

	const double n = result.count;
	const double rained = query.value(1).toDouble();

	result.score = query.value(2).toDouble() / n;
	result.base_rate = rained / n;

	/*
	 * The score of ignoring the weather entirely and always predicting
	 * the observed base rate. It is what "good" has to be measured
	 * against: a Brier of 0.1 is excellent in a dry climate and poor in
	 * a changeable one, and only the comparison says which this is.
	 */
	result.baseline = result.base_rate * (1.0 - result.base_rate);

	return result;
}

std::vector<bbq_reliability_bin> bbq_history::reliability(
        const QString &station, bbq_band band, bbq_lead_bucket bucket) const {
	std::vector<bbq_reliability_bin> bins;

	if (!m_open) {
		return bins;
	}

	QSqlQuery query(QSqlDatabase::database(m_connection));
	query.prepare(QStringLiteral(
	        "SELECT probability_bin, count, rain_count FROM reliability "
	        "WHERE station = ? AND band = ? AND lead_bucket = ? "
	        "ORDER BY probability_bin"));
	query.addBindValue(station);
	query.addBindValue(static_cast<int>(band));
	query.addBindValue(static_cast<int>(bucket));

	if (!query.exec()) {
		m_last_error = query.lastError().text();
		return bins;
	}

	while (query.next()) {
		bbq_reliability_bin bin;
		bin.probability_bin = query.value(0).toInt();
		bin.count = query.value(1).toInt();
		bin.rain_count = query.value(2).toInt();
		bins.push_back(bin);
	}

	return bins;
}

bbq_verification bbq_history::verification(const QString &station,
                                           bbq_band band,
                                           const QString &quantity,
                                           bbq_lead_bucket bucket) const {
	bbq_verification result;

	if (!m_open) {
		return result;
	}

	QSqlQuery query(QSqlDatabase::database(m_connection));
	query.prepare(QStringLiteral(
	        "SELECT count, sum_error, sum_absolute_error, sum_square_error "
	        "FROM verification WHERE station = ? AND band = ? AND quantity = ? "
	        "AND lead_bucket = ?"));
	query.addBindValue(station);
	query.addBindValue(static_cast<int>(band));
	query.addBindValue(quantity);
	query.addBindValue(static_cast<int>(bucket));

	if (!query.exec() || !query.next()) {
		return result;
	}

	result.count = query.value(0).toInt();
	if (result.count <= 0) {
		result.count = 0;
		return result;
	}

	const double n = result.count;
	result.bias = query.value(1).toDouble() / n;
	result.mean_absolute_error = query.value(2).toDouble() / n;
	result.root_mean_square_error = std::sqrt(query.value(3).toDouble() / n);

	return result;
}

bbq_series bbq_history::observations(const QString &station, qint64 from,
                                     qint64 to) const {
	bbq_series series(bbq_band::observed, QStringLiteral("history"));

	if (!m_open) {
		return series;
	}

	QSqlQuery query(QSqlDatabase::database(m_connection));
	query.prepare(QStringLiteral(
	        "SELECT valid_utc, duration_s, temperature, precip_rate, wind_kph "
	        "FROM observation WHERE station = ? AND valid_utc >= ? "
	        "AND valid_utc < ? ORDER BY valid_utc"));
	query.addBindValue(station);
	query.addBindValue(from);
	query.addBindValue(to);

	if (!query.exec()) {
		m_last_error = query.lastError().text();
		return series;
	}

	std::vector<bbq_sample> samples;
	while (query.next()) {
		bbq_sample sample;
		sample.start_utc = query.value(0).toLongLong();
		sample.duration_s = query.value(1).toInt();

		if (!query.value(2).isNull()) {
			sample.temperature = query.value(2).toDouble();
		}
		if (!query.value(3).isNull()) {
			sample.precip_rate = query.value(3).toDouble();
		}
		if (!query.value(4).isNull()) {
			sample.wind_kph = query.value(4).toDouble();
		}

		samples.push_back(sample);
	}

	series.set_samples(std::move(samples));
	return series;
}

qint64 bbq_history::earliest_observation(const QString &station) const {
	if (!m_open) {
		return 0;
	}

	QSqlQuery query(QSqlDatabase::database(m_connection));
	query.prepare(QStringLiteral(
	        "SELECT MIN(valid_utc) FROM observation WHERE station = ?"));
	query.addBindValue(station);

	if (!query.exec() || !query.next() || query.value(0).isNull()) {
		return 0;
	}

	return query.value(0).toLongLong();
}

int bbq_history::observation_count(const QString &station) const {
	if (!m_open) {
		return 0;
	}

	QSqlQuery query(QSqlDatabase::database(m_connection));
	query.prepare(QStringLiteral(
	        "SELECT COUNT(*) FROM observation WHERE station = ?"));
	query.addBindValue(station);

	if (!query.exec() || !query.next()) {
		return 0;
	}

	return query.value(0).toInt();
}

int bbq_history::pending_count(const QString &station) const {
	if (!m_open) {
		return 0;
	}

	QSqlQuery query(QSqlDatabase::database(m_connection));
	query.prepare(QStringLiteral(
	        "SELECT COUNT(*) FROM forecast_pending WHERE station = ?"));
	query.addBindValue(station);

	if (!query.exec() || !query.next()) {
		return 0;
	}

	return query.value(0).toInt();
}
