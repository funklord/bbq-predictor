#include <QTest>

#include "publish/demand.h"

/*
 * The publisher's schedule of stations (project.md sec 15.14): what is
 * admitted, what expires, what is dropped, and how the hour's budget is
 * shared. Each limit is asserted at its edge, the case allowed and the
 * one after it refused, because a limit tested from one side only is a
 * limit nobody has seen hold.
 */
class test_demand : public QObject {
	Q_OBJECT

private slots:
	void a_line_must_be_a_station_and_a_believable_time();
	void the_ceiling_admits_fifty_and_no_more();
	void two_weeks_without_a_request_expires_a_station();
	void the_configured_station_never_expires_and_comes_first();
	void silence_drops_a_station_only_while_others_answer();
	void the_budget_stretches_the_interval_rather_than_being_spent();
	void a_round_stops_where_the_hour_runs_out();
	void the_state_survives_the_store_and_damage_reads_as_empty();
};

namespace {

const qint64 now = 1792000000;

QByteArray line(const QString &station, qint64 asked) {
	return station.toLatin1() + ' ' + QByteArray::number(asked) + '\n';
}

} // namespace

void test_demand::a_line_must_be_a_station_and_a_believable_time() {
	bbq_demand demand;
	const QByteArray text = line("IGOOD1", now) + "IBAD2\n" + "ibad3 " +
	                        QByteArray::number(now) + "\n" + line("IFUTURE4", now + 301) +
	                        line("IOLD5", now - bbq_demand_expiry_s - 1) +
	                        "ISPACE6  " + QByteArray::number(now) + "\n" +
	                        line("INEAR7", now + 300) + "IEND8 12x\n";

	QCOMPARE(bbq_demand_absorb(demand, text, now), 2);
	QCOMPARE(demand.stations.size(), size_t(2));
	QVERIFY(demand.stations.count("IGOOD1"));
	QVERIFY(demand.stations.count("INEAR7"));

	/* A later line moves a known station's time forward, never back. */
	QCOMPARE(bbq_demand_absorb(demand, line("IGOOD1", now - 100), now), 0);
	QCOMPARE(demand.stations["IGOOD1"].last_asked_utc, now);
	QCOMPARE(bbq_demand_absorb(demand, line("IGOOD1", now + 100), now + 100), 0);
	QCOMPARE(demand.stations["IGOOD1"].last_asked_utc, now + 100);
}

void test_demand::the_ceiling_admits_fifty_and_no_more() {
	bbq_demand demand;
	bbq_demand_configure(demand, QStringLiteral("ICONF1"), now);

	QByteArray text;
	for (int i = 0; i < bbq_demand_max_stations + 5; ++i) {
		text += line(QStringLiteral("IX%1").arg(i), now);
	}
	int turned_away = -1;
	QCOMPARE(bbq_demand_absorb(demand, text, now, &turned_away), bbq_demand_max_stations);
	QCOMPARE(turned_away, 5);

	/* Counted, not repeated: IX50 asking twice is one station turned away. */
	QCOMPARE(bbq_demand_absorb(demand, line("IX50", now) + line("IX50", now), now, &turned_away),
	         0);
	QCOMPARE(turned_away, 1);

	/* The configured station is not one of the fifty. */
	QCOMPARE(demand.stations.size(), size_t(bbq_demand_max_stations + 1));

	/* One expiring makes room for one more. */
	demand.stations["IX0"].last_asked_utc = now - bbq_demand_expiry_s - 1;
	bbq_demand_expire(demand, now);
	QCOMPARE(bbq_demand_absorb(demand, line("INEW1", now) + line("INEW2", now), now), 1);
}

void test_demand::two_weeks_without_a_request_expires_a_station() {
	bbq_demand demand;
	QCOMPARE(bbq_demand_absorb(demand, line("IONE1", now), now), 1);

	bbq_demand_expire(demand, now + bbq_demand_expiry_s);
	QVERIFY(demand.stations.count("IONE1"));

	bbq_demand_expire(demand, now + bbq_demand_expiry_s + 1);
	QVERIFY(!demand.stations.count("IONE1"));

	/* Its published months are the publisher's to keep; only fetching stops. */
}

void test_demand::the_configured_station_never_expires_and_comes_first() {
	bbq_demand demand;
	bbq_demand_absorb(demand, line("IASKED1", now) + line("IASKED2", now - 60), now);
	bbq_demand_configure(demand, QStringLiteral("ICONF1"), now - 2 * bbq_demand_expiry_s);

	bbq_demand_expire(demand, now);
	QVERIFY(demand.stations.count("ICONF1"));

	const std::vector<QString> due = bbq_demand_due(demand, now);
	QCOMPARE(due.size(), size_t(3));
	QCOMPARE(due[0], QStringLiteral("ICONF1"));
	QCOMPARE(due[1], QStringLiteral("IASKED1"));
	QCOMPARE(due[2], QStringLiteral("IASKED2"));

	/* Configuring another moves the mark; the old one can expire now. */
	bbq_demand_configure(demand, QStringLiteral("IASKED2"), now);
	QVERIFY(!demand.stations["ICONF1"].configured);
	bbq_demand_expire(demand, now + 1);
	QVERIFY(!demand.stations.count("ICONF1"));
}

void test_demand::silence_drops_a_station_only_while_others_answer() {
	bbq_demand demand;
	bbq_demand_configure(demand, QStringLiteral("ICONF1"), now);
	bbq_demand_absorb(demand, line("IGHOST1", now), now);

	/* Nobody answered: the key page or the network, not the station. */
	for (int round = 0; round < 5; ++round) {
		bbq_demand_record(demand, {{"ICONF1", 2, false}, {"IGHOST1", 1, false}},
		                  now + round * 300);
	}
	QVERIFY(demand.stations.count("IGHOST1"));
	QCOMPARE(demand.stations["IGHOST1"].silent_rounds, 0);

	/* The configured station answers and this one does not, three times running. */
	for (int round = 0; round < bbq_demand_silent_rounds_max - 1; ++round) {
		bbq_demand_record(demand, {{"ICONF1", 5, true}, {"IGHOST1", 2, false}},
		                  now + 3000 + round * 300);
		QVERIFY(demand.stations.count("IGHOST1"));
	}
	bbq_demand_record(demand, {{"ICONF1", 5, true}, {"IGHOST1", 2, false}}, now + 4000);
	QVERIFY(!demand.stations.count("IGHOST1"));

	/* And it is not admitted again for a week, however often it is asked. */
	QCOMPARE(bbq_demand_absorb(demand, line("IGHOST1", now + 4100), now + 4100), 0);
	bbq_demand_expire(demand, now + 4000 + bbq_demand_refusal_s);
	QCOMPARE(bbq_demand_absorb(demand, line("IGHOST1", now + 4000 + bbq_demand_refusal_s),
	                           now + 4000 + bbq_demand_refusal_s),
	         1);

	/* A station that answers once is forgiven its silence so far. */
	bbq_demand_record(demand, {{"ICONF1", 5, true}, {"IGHOST1", 2, false}}, now + 9000000);
	bbq_demand_record(demand, {{"ICONF1", 5, true}, {"IGHOST1", 2, true}}, now + 9000300);
	QCOMPARE(demand.stations["IGHOST1"].silent_rounds, 0);

	/* The configured station is never dropped. */
	for (int round = 0; round < 10; ++round) {
		bbq_demand_record(demand, {{"ICONF1", 5, false}, {"IGHOST1", 2, true}},
		                  now + 9001000 + round * 300);
	}
	QVERIFY(demand.stations.count("ICONF1"));
}

void test_demand::the_budget_stretches_the_interval_rather_than_being_spent() {
	bbq_demand demand;

	/* Twenty stations at six requests, twelve rounds an hour: 1440, inside 2000. */
	QByteArray text;
	for (int i = 0; i < 20; ++i) {
		text += line(QStringLiteral("IX%1").arg(i), now);
	}
	bbq_demand_absorb(demand, text, now);
	QCOMPARE(bbq_demand_interval(demand), bbq_demand_interval_s);

	/* Fifty: 3600 an hour wanted, so every station waits 3600/2000 as long. */
	text.clear();
	for (int i = 20; i < 50; ++i) {
		text += line(QStringLiteral("IX%1").arg(i), now);
	}
	bbq_demand_absorb(demand, text, now);
	QCOMPARE(bbq_demand_interval(demand), qint64(540));

	/* Fetched four minutes ago is not due at a nine-minute interval. */
	for (auto &entry : demand.stations) {
		entry.second.last_fetched_utc = now - 240;
	}
	QVERIFY(bbq_demand_due(demand, now).empty());
	QCOMPARE(bbq_demand_due(demand, now + 300 - 30).size(), size_t(50));
}

void test_demand::a_round_stops_where_the_hour_runs_out() {
	bbq_demand demand;
	bbq_demand_configure(demand, QStringLiteral("ICONF1"), now);
	bbq_demand_absorb(demand, line("IONE1", now) + line("ITWO2", now - 1), now);

	/* 1990 already spent this hour: room for the configured station, at six, alone. */
	demand.spent.emplace_back(now - 1800, 1990);
	const std::vector<QString> due = bbq_demand_due(demand, now);
	QCOMPARE(due, std::vector<QString>({QStringLiteral("ICONF1")}));

	/* An hour on, that spending no longer counts. */
	bbq_demand_expire(demand, now + 1800);
	QCOMPARE(bbq_demand_spent(demand, now + 1800), 0);
	QCOMPARE(bbq_demand_due(demand, now + 1800).size(), size_t(3));

	/* A measured cost replaces the guess. */
	bbq_demand_record(demand, {{"IONE1", 40, true}}, now + 1800);
	QCOMPARE(demand.stations["IONE1"].cost, 40);
	QCOMPARE(bbq_demand_spent(demand, now + 1800), 40);
}

void test_demand::the_state_survives_the_store_and_damage_reads_as_empty() {
	bbq_demand demand;
	bbq_demand_configure(demand, QStringLiteral("ICONF1"), now);
	bbq_demand_absorb(demand, line("IONE1", now - 5), now);
	bbq_demand_record(demand, {{"IONE1", 7, true}}, now);
	demand.refused_until["IGONE1"] = now + 100;

	const QByteArray bytes = bbq_demand_serialize(demand);
	const bbq_demand back = bbq_demand_parse(bytes);
	QCOMPARE(back.stations.size(), size_t(2));
	QCOMPARE(back.stations.at("IONE1").cost, 7);
	QCOMPARE(back.stations.at("IONE1").last_asked_utc, now - 5);
	QCOMPARE(back.stations.at("IONE1").last_fetched_utc, now);
	QCOMPARE(back.refused_until.at("IGONE1"), now + 100);
	QCOMPARE(back.spent.size(), size_t(1));

	/* The configured mark is the command line's, not the store's. */
	QVERIFY(!back.stations.at("ICONF1").configured);

	/* Truncated anywhere, or another version: empty, never half. */
	for (int cut = 0; cut < bytes.size(); cut += 7) {
		QVERIFY2(bbq_demand_parse(bytes.left(cut)).stations.empty(),
		         qPrintable(QStringLiteral("cut at %1").arg(cut)));
	}
	QByteArray other = bytes;
	other[3] = char(2);
	QVERIFY(bbq_demand_parse(other).stations.empty());
}

QTEST_GUILESS_MAIN(test_demand)
#include "test_demand.moc"
