#ifndef BBQ_DEMAND_H
#define BBQ_DEMAND_H

#include <QByteArray>
#include <QString>

#include <map>
#include <utility>
#include <vector>

/*
 * Which stations the publisher fetches, and how often (project.md sec
 * 15.14). Pure: the demand file's text and the clock in, a schedule out,
 * so every rule is checked without a network.
 *
 * The configured station is always fetched and never expires. Stations
 * clients ask for are admitted up to a ceiling, dropped after two weeks
 * nobody asked, and dropped if Weather Underground keeps answering for
 * the others while saying nothing for them -- which is what a station id
 * that does not exist looks like, and the one thing a stranger could
 * otherwise fill the ceiling with.
 *
 * Every Weather Underground request comes out of one budget an hour, from
 * one address and one key. When the stations want more than that, each
 * is fetched less often rather than the budget being exceeded: a station
 * every fifteen minutes is still a station, and an address that has been
 * refused is none.
 */

/* Holder's numbers, 2026-10-23. */
const int bbq_demand_max_stations = 50;
const qint64 bbq_demand_expiry_s = 14 * 24 * 3600;
const int bbq_demand_budget_per_hour = 2000;

/* The round the timer runs, and the most often any station is fetched. */
const qint64 bbq_demand_interval_s = 5 * 60;

/* What a round costs before one has been measured: key, four products, one spare. */
const int bbq_demand_default_cost = 6;

/* Rounds of silence, while others answered, before a station is dropped. */
const int bbq_demand_silent_rounds_max = 3;
const qint64 bbq_demand_refusal_s = 7 * 24 * 3600;

/*
 * Where a run stops starting stations, so it ends inside the timer's
 * five minutes even when every round takes its whole thirty seconds.
 */
const qint64 bbq_demand_deadline_s = 4 * 60;

/* The most of the demand file a run reads: the daemon's cap, and a little over. */
const qint64 bbq_demand_read_max = (1 << 20) + 4096;

/* A demand line further than this from now is not believed. */
const qint64 bbq_demand_skew_s = 5 * 60;

struct bbq_demand_station {
	qint64 first_asked_utc = 0;
	qint64 last_asked_utc = 0;
	qint64 last_fetched_utc = 0;

	/* WU requests its last round took; 0 until one has. */
	int cost = 0;

	/* Consecutive rounds with nothing from the station while others had data. */
	int silent_rounds = 0;

	/* Set from the command line each run, never from the file. */
	bool configured = false;
};

struct bbq_demand {
	std::map<QString, bbq_demand_station> stations;

	/* Ids dropped for silence, not admitted again until then. */
	std::map<QString, qint64> refused_until;

	/* WU requests spent, by the round they were spent in; the last hour. */
	std::vector<std::pair<qint64, int>> spent;
};

/* What one station's round came to, as bbq_round_report says it. */
struct bbq_demand_result {
	QString station;
	int wu_requests = 0;
	bool station_data = false;
};

/*
 * Mark `station` as the configured one: always fetched, never expired or
 * refused. Any other station that was marked loses the mark.
 */
void bbq_demand_configure(bbq_demand &demand, const QString &station, qint64 now_utc);

/*
 * Take the daemon's lines ("<STATION> <epoch>\n"). A malformed line, an
 * id that is not one, or a time further than bbq_demand_skew_s ahead or
 * older than the expiry is ignored. Returns how many stations were
 * admitted that were not there before.
 */
int bbq_demand_absorb(bbq_demand &demand, const QByteArray &text, qint64 now_utc);

/* Drop what nobody has asked for in two weeks, and spending older than an hour. */
void bbq_demand_expire(bbq_demand &demand, qint64 now_utc);

/* WU requests spent in the hour up to `now_utc`. */
int bbq_demand_spent(const bbq_demand &demand, qint64 now_utc);

/*
 * The interval every station is fetched at: bbq_demand_interval_s, or
 * longer when the stations together would spend more than the budget.
 */
qint64 bbq_demand_interval(const bbq_demand &demand);

/*
 * The stations to fetch now, in order: the configured one first, then by
 * who was asked for most recently. Stops where the hour's remaining budget
 * would not cover the next one.
 */
std::vector<QString> bbq_demand_due(const bbq_demand &demand, qint64 now_utc);

/*
 * Record a run's rounds. Silence counts against a station only when some
 * station in the same run had data: when none did, the fault was the key
 * page or the network, and every station would otherwise be blamed for it.
 */
void bbq_demand_record(bbq_demand &demand, const std::vector<bbq_demand_result> &results,
                       qint64 now_utc);

/* Stored in the archive's named values; an unreadable blob reads as empty. */
QByteArray bbq_demand_serialize(const bbq_demand &demand);
bbq_demand bbq_demand_parse(const QByteArray &bytes);

#endif
