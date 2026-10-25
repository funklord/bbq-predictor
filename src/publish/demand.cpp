#include "publish/demand.h"

#include "wire/format.h"

#include <QDataStream>
#include <QIODevice>
#include <QSet>

#include <algorithm>

namespace {

int admitted_count(const bbq_demand &demand) {
	int count = 0;
	for (const auto &entry : demand.stations) {
		count += entry.second.configured ? 0 : 1;
	}
	return count;
}

int cost_of(const bbq_demand_station &station) {
	return station.cost > 0 ? station.cost : bbq_demand_default_cost;
}

/* A version, so a change of layout reads as empty rather than as garbage. */
const quint32 serialized_version = 1;

} // namespace

void bbq_demand_configure(bbq_demand &demand, const QString &station, qint64 now_utc) {
	for (auto &entry : demand.stations) {
		entry.second.configured = false;
	}
	if (station.isEmpty()) {
		return;
	}

	bbq_demand_station &configured = demand.stations[station];
	configured.configured = true;
	if (configured.first_asked_utc == 0) {
		configured.first_asked_utc = now_utc;
	}
	configured.last_asked_utc = now_utc;
	configured.silent_rounds = 0;
	demand.refused_until.erase(station);
}

int bbq_demand_absorb(bbq_demand &demand, const QByteArray &text, qint64 now_utc,
                      int *turned_away) {
	int admitted = 0;
	QSet<QString> full;

	for (const QByteArray &line : text.split('\n')) {
		const QList<QByteArray> fields = line.split(' ');
		if (fields.size() != 2) {
			continue;
		}

		const QString station = QString::fromLatin1(fields.at(0));
		bool number = false;
		const qint64 asked = fields.at(1).toLongLong(&number);
		if (!number || !bbq_wire_station_valid(station) ||
		    asked > now_utc + bbq_demand_skew_s || asked < now_utc - bbq_demand_expiry_s) {
			continue;
		}

		const auto known = demand.stations.find(station);
		if (known != demand.stations.end()) {
			known->second.last_asked_utc = std::max(known->second.last_asked_utc, asked);
			continue;
		}

		const auto refused = demand.refused_until.find(station);
		if (refused != demand.refused_until.end() && refused->second > now_utc) {
			continue;
		}
		if (admitted_count(demand) >= bbq_demand_max_stations) {
			full.insert(station);
			continue;
		}

		bbq_demand_station &added = demand.stations[station];
		added.first_asked_utc = asked;
		added.last_asked_utc = asked;
		++admitted;
	}

	if (turned_away != nullptr) {
		*turned_away = int(full.size());
	}
	return admitted;
}

void bbq_demand_expire(bbq_demand &demand, qint64 now_utc) {
	for (auto it = demand.stations.begin(); it != demand.stations.end();) {
		if (!it->second.configured &&
		    now_utc - it->second.last_asked_utc > bbq_demand_expiry_s) {
			it = demand.stations.erase(it);
		} else {
			++it;
		}
	}

	for (auto it = demand.refused_until.begin(); it != demand.refused_until.end();) {
		if (it->second <= now_utc) {
			it = demand.refused_until.erase(it);
		} else {
			++it;
		}
	}

	demand.spent.erase(std::remove_if(demand.spent.begin(), demand.spent.end(),
	                                  [now_utc](const std::pair<qint64, int> &round) {
		                                  return now_utc - round.first >= 3600 ||
		                                         round.first > now_utc;
	                                  }),
	                   demand.spent.end());
}

int bbq_demand_spent(const bbq_demand &demand, qint64 now_utc) {
	int spent = 0;
	for (const auto &round : demand.spent) {
		if (now_utc - round.first < 3600 && round.first <= now_utc) {
			spent += round.second;
		}
	}
	return spent;
}

qint64 bbq_demand_interval(const bbq_demand &demand) {
	const qint64 rounds_an_hour = 3600 / bbq_demand_interval_s;
	qint64 wanted = 0;
	for (const auto &entry : demand.stations) {
		wanted += cost_of(entry.second) * rounds_an_hour;
	}

	if (wanted <= bbq_demand_budget_per_hour) {
		return bbq_demand_interval_s;
	}

	/* Stretched in proportion, rounded up, so the stations together fit. */
	return (bbq_demand_interval_s * wanted + bbq_demand_budget_per_hour - 1) /
	       bbq_demand_budget_per_hour;
}

std::vector<QString> bbq_demand_due(const bbq_demand &demand, qint64 now_utc) {
	const qint64 interval = bbq_demand_interval(demand);

	/*
	 * A little slack, so a timer that fires a few seconds early does not
	 * skip a whole round: the timer's own jitter is thirty seconds.
	 */
	const qint64 slack = 30;

	std::vector<std::pair<QString, const bbq_demand_station *>> due;
	for (const auto &entry : demand.stations) {
		const bbq_demand_station &station = entry.second;
		if (station.last_fetched_utc == 0 ||
		    now_utc - station.last_fetched_utc >= interval - slack ||
		    station.last_fetched_utc > now_utc) {
			due.emplace_back(entry.first, &station);
		}
	}

	std::stable_sort(due.begin(), due.end(), [](const auto &a, const auto &b) {
		if (a.second->configured != b.second->configured) {
			return a.second->configured;
		}
		return a.second->last_asked_utc > b.second->last_asked_utc;
	});

	int remaining = bbq_demand_budget_per_hour - bbq_demand_spent(demand, now_utc);
	std::vector<QString> chosen;
	for (const auto &entry : due) {
		const int cost = cost_of(*entry.second);
		if (cost > remaining) {
			break;
		}
		remaining -= cost;
		chosen.push_back(entry.first);
	}
	return chosen;
}

void bbq_demand_record(bbq_demand &demand, const std::vector<bbq_demand_result> &results,
                       qint64 now_utc) {
	bool anybody_answered = false;
	int spent = 0;
	for (const bbq_demand_result &result : results) {
		anybody_answered = anybody_answered || result.station_data;
		spent += result.wu_requests;
	}
	if (spent > 0) {
		demand.spent.emplace_back(now_utc, spent);
	}

	for (const bbq_demand_result &result : results) {
		const auto found = demand.stations.find(result.station);
		if (found == demand.stations.end()) {
			continue;
		}
		bbq_demand_station &station = found->second;
		station.last_fetched_utc = now_utc;
		if (result.wu_requests > 0) {
			station.cost = result.wu_requests;
		}

		if (result.station_data || !anybody_answered) {
			station.silent_rounds = result.station_data ? 0 : station.silent_rounds;
			continue;
		}
		if (station.configured) {
			continue;
		}
		if (++station.silent_rounds >= bbq_demand_silent_rounds_max) {
			demand.refused_until[result.station] = now_utc + bbq_demand_refusal_s;
			demand.stations.erase(found);
		}
	}
}

QByteArray bbq_demand_serialize(const bbq_demand &demand) {
	QByteArray bytes;
	QDataStream out(&bytes, QIODevice::WriteOnly);
	out.setVersion(QDataStream::Qt_6_0);

	out << serialized_version << quint32(demand.stations.size());
	for (const auto &entry : demand.stations) {
		out << entry.first << entry.second.first_asked_utc << entry.second.last_asked_utc
		    << entry.second.last_fetched_utc << qint32(entry.second.cost)
		    << qint32(entry.second.silent_rounds);
	}
	out << quint32(demand.refused_until.size());
	for (const auto &entry : demand.refused_until) {
		out << entry.first << entry.second;
	}
	out << quint32(demand.spent.size());
	for (const auto &round : demand.spent) {
		out << round.first << qint32(round.second);
	}
	return bytes;
}

bbq_demand bbq_demand_parse(const QByteArray &bytes) {
	bbq_demand demand;
	QDataStream in(bytes);
	in.setVersion(QDataStream::Qt_6_0);

	/*
	 * Counts are bounded before anything is read for them: the blob is
	 * this program's own, but a damaged one must not become a loop of
	 * four billion or an allocation to match.
	 */
	const quint32 bound = 100000;

	quint32 version = 0;
	quint32 count = 0;
	in >> version >> count;
	if (in.status() != QDataStream::Ok || version != serialized_version || count > bound) {
		return bbq_demand();
	}
	for (quint32 i = 0; i < count; ++i) {
		QString id;
		bbq_demand_station station;
		qint32 cost = 0;
		qint32 silent = 0;
		in >> id >> station.first_asked_utc >> station.last_asked_utc >>
		        station.last_fetched_utc >> cost >> silent;
		station.cost = cost;
		station.silent_rounds = silent;
		if (in.status() != QDataStream::Ok || !bbq_wire_station_valid(id)) {
			return bbq_demand();
		}
		demand.stations[id] = station;
	}

	in >> count;
	if (in.status() != QDataStream::Ok || count > bound) {
		return bbq_demand();
	}
	for (quint32 i = 0; i < count; ++i) {
		QString id;
		qint64 until = 0;
		in >> id >> until;
		if (in.status() != QDataStream::Ok) {
			return bbq_demand();
		}
		demand.refused_until[id] = until;
	}

	in >> count;
	if (in.status() != QDataStream::Ok || count > bound) {
		return bbq_demand();
	}
	for (quint32 i = 0; i < count; ++i) {
		qint64 when = 0;
		qint32 requests = 0;
		in >> when >> requests;
		if (in.status() != QDataStream::Ok) {
			return bbq_demand();
		}
		demand.spent.emplace_back(when, requests);
	}
	return demand;
}
