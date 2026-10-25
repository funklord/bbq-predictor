#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QNetworkProxyFactory>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

#include "publish/encode.h"
#include "publish/signing_key.h"
#include "serve/serve_loop.h"
#include "wu/feed.h"

/*
 * The feed reading from vibes.se (project.md sec 15.12), end to end
 * against the real daemon loop on loopback: what a round applies, what
 * it stores, what it remembers, and what it does when the server is
 * gone or does not publish the station.
 *
 * Hermetic: loopback goes direct and everything else to a proxy that
 * answers nothing, so no case reaches a weather provider -- the
 * network probe included.
 */
class test_feed_server : public QObject {
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();

	void a_round_applies_the_head_and_stores_the_months();
	void a_second_round_fetches_no_month_it_already_has();
	void a_month_the_index_does_not_name_is_not_applied();
	void a_month_the_archive_refuses_is_not_held();
	void an_offline_launch_shows_the_last_head();
	void a_station_the_server_does_not_publish_is_not_an_outage();
	void a_station_the_server_has_since_added_is_taken_up();
	void a_server_that_does_not_answer_is_counted_not_abandoned();
	void a_file_signed_by_another_key_is_not_applied();
	void no_key_means_no_server();
	void the_keyless_bands_settle_quietly_when_unreachable();

private:
	bool settle(bbq_wu_feed &feed);
	static bbq_server_route daemon(quint16 port);

	QTemporaryDir m_root;
	bbq_signing_key m_key;
	int m_dir_fd = -1;
	int m_listen_fd = -1;
	int m_wake[2] = {-1, -1};
	quint16 m_port = 0;
	std::thread m_thread;
	int m_september_rows = 0;
};

namespace {

/* Loopback direct, everything else into a proxy that answers nothing. */
class hermetic_proxies : public QNetworkProxyFactory {
public:
	QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override {
		if (query.peerHostName() == QLatin1String("127.0.0.1")) {
			return {QNetworkProxy(QNetworkProxy::NoProxy)};
		}
		return {QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 9)};
	}
};

void write(const QString &path, const QByteArray &bytes) {
	QDir().mkpath(QFileInfo(path).path());
	QFile out(path);
	out.open(QIODevice::WriteOnly);
	out.write(bytes);
}

const QString station = QStringLiteral("ISTOCK877");
const QString swapped = QStringLiteral("ISTOCK999");

} // namespace

bbq_server_route test_feed_server::daemon(quint16 port) {
	bbq_server_route route;
	route.type = bbq_server_route::kind::daemon;
	route.host = QStringLiteral("127.0.0.1");
	route.port = port;
	return route;
}

bool test_feed_server::settle(bbq_wu_feed &feed) {
	QSignalSpy settled(&feed, &bbq_wu_feed::settled);
	feed.refresh();
	return settled.count() > 0 || settled.wait(10000);
}

void test_feed_server::initTestCase() {
	QNetworkProxyFactory::setApplicationProxyFactory(new hermetic_proxies);

	uint8_t seed[32] = {3, 1, 4, 1, 5};
	m_key.set_seed(seed);

	/* A published tree: two months, an index, a head and a manifest. */
	QString why;
	const QString dir = m_root.filePath("published");
	std::vector<bbq_wire_index_entry> entries;
	for (uint16_t month : {uint16_t(2026 * 12 + 7), uint16_t(2026 * 12 + 8)}) {
		bbq_wire_month_rows rows;
		rows.providers = {QStringLiteral("wunderground")};
		for (int i = 0; i < 200; ++i) {
			bbq_sample sample;
			sample.start_utc = bbq_wire_month_begin(month) + i * 300;
			sample.duration_s = 300;
			sample.temperature = 10.0 + i * 0.1;
			rows.samples.push_back(sample);
			rows.provider.push_back(0);
		}
		const QByteArray file = bbq_wire_file(
		        bbq_wire_kind::month, bbq_wire_month_body(station, month, rows, &why), m_key);
		write(dir + "/" + station + "/" + bbq_wire_month_name(month), file);

		bbq_wire_index_entry entry;
		entry.month = month;
		entry.rows = 200;
		entry.bytes = uint32_t(file.size());
		entry.digest = bbq_wire_digest(file);
		entry.first_utc = rows.samples.front().start_utc;
		entry.last_utc = rows.samples.back().start_utc;
		entries.push_back(entry);
		m_september_rows = 200;
	}
	write(dir + "/" + station + "/index",
	      bbq_wire_file(bbq_wire_kind::index, bbq_wire_index_body(station, 1, entries, &why),
	                    m_key));

	bbq_series hourly(bbq_band::hourly, QStringLiteral("wunderground"));
	std::vector<bbq_sample> hours(24);
	for (int i = 0; i < 24; ++i) {
		hours[size_t(i)].start_utc = 1790000000 + i * 3600;
		hours[size_t(i)].duration_s = 3600;
		hours[size_t(i)].temperature = 20.0;
	}
	hourly.set_samples(hours);
	hourly.set_fetched_utc(1789999000);
	write(dir + "/" + station + "/head",
	      bbq_wire_file(bbq_wire_kind::head,
	                    bbq_wire_head_body(station, 1789999000, {&hourly}, &why), m_key));
	/*
	 * A second station whose month file is validly signed but is NOT the
	 * one its index names: the index was made from one version, and a
	 * later (or older) signed file was put in its place. Only the digest
	 * can tell -- every signature here verifies.
	 */
	{
		const uint16_t month = uint16_t(2026 * 12 + 8);
		bbq_wire_month_rows named;
		named.providers = {QStringLiteral("wunderground")};
		bbq_sample sample;
		sample.start_utc = bbq_wire_month_begin(month);
		sample.duration_s = 300;
		sample.temperature = 1.0;
		named.samples = {sample};
		named.provider = {0};
		const QByteArray indexed = bbq_wire_file(
		        bbq_wire_kind::month, bbq_wire_month_body(swapped, month, named, &why), m_key);
		bbq_wire_month_rows served = named;
		served.samples[0].temperature = 2.0;
		write(dir + "/" + swapped + "/" + bbq_wire_month_name(month),
		      bbq_wire_file(bbq_wire_kind::month,
		                    bbq_wire_month_body(swapped, month, served, &why), m_key));

		bbq_wire_index_entry entry;
		entry.month = month;
		entry.rows = 1;
		entry.bytes = uint32_t(indexed.size());
		entry.digest = bbq_wire_digest(indexed);
		entry.first_utc = entry.last_utc = sample.start_utc;
		write(dir + "/" + swapped + "/index",
		      bbq_wire_file(bbq_wire_kind::index,
		                    bbq_wire_index_body(swapped, 1, {entry}, &why), m_key));
		write(dir + "/" + swapped + "/head",
		      bbq_wire_file(bbq_wire_kind::head,
		                    bbq_wire_head_body(swapped, 1789999000, {&hourly}, &why), m_key));
	}

	write(dir + "/stations",
	      bbq_wire_file(bbq_wire_kind::stations,
	                    bbq_wire_stations_body(1, {station, swapped}, &why), m_key));

	m_dir_fd = ::open(QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	m_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	QVERIFY(::bind(m_listen_fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0);
	QVERIFY(::listen(m_listen_fd, 64) == 0);
	socklen_t length = sizeof address;
	::getsockname(m_listen_fd, reinterpret_cast<sockaddr *>(&address), &length);
	m_port = ntohs(address.sin_port);
	QVERIFY(::pipe2(m_wake, O_CLOEXEC | O_NONBLOCK) == 0);
	m_thread = std::thread([this] {
		bbq_serve_loop loop = {};
		loop.dir_fd = m_dir_fd;
		loop.listen_fds = &m_listen_fd;
		loop.listen_count = 1;
		loop.wake_fd = m_wake[0];
		loop.limits = bbq_serve_default_limits();
		bbq_serve_run(&loop);
	});
}

void test_feed_server::cleanupTestCase() {
	const char quit = 'q';
	(void)!::write(m_wake[1], &quit, 1);
	if (m_thread.joinable()) {
		m_thread.join();
	}
	for (int fd : {m_dir_fd, m_listen_fd, m_wake[0], m_wake[1]}) {
		if (fd >= 0) {
			::close(fd);
		}
	}
}

void test_feed_server::a_round_applies_the_head_and_stores_the_months() {
	QTemporaryDir scratch;
	bbq_wu_feed feed;
	QVERIFY(feed.open_history(scratch.filePath("history.sqlite")));
	feed.set_server(m_key.public_key(), {daemon(m_port)});
	feed.set_station(station);
	QVERIFY(feed.server_active());
	QSignalSpy refused(&feed, &bbq_wu_feed::months_refused);

	QVERIFY(settle(feed));
	QCOMPARE(refused.size(), 0);

	const bbq_series *hourly = feed.composite().band(bbq_band::hourly);
	QVERIFY(hourly != nullptr);
	QCOMPARE(hourly->fetched_utc(), qint64(1789999000));

	/* Both months, every row, under the provider the file named. */
	const auto months = feed.history().observation_months(station);
	QCOMPARE(months.size(), size_t(2));
	QCOMPARE(months[1].rows, m_september_rows);
	QCOMPARE(feed.history().server_months(station).size(), size_t(2));

	QCOMPARE(feed.server_state().failures, 0);
	QVERIFY(feed.server_state().last_success_utc > 0);
	QVERIFY(!feed.scraping());
}

void test_feed_server::a_second_round_fetches_no_month_it_already_has() {
	QTemporaryDir scratch;
	const QString path = scratch.filePath("history.sqlite");
	{
		bbq_wu_feed feed;
		QVERIFY(feed.open_history(path));
		feed.set_server(m_key.public_key(), {daemon(m_port)});
		feed.set_station(station);
		QVERIFY(settle(feed));
	}

	/*
	 * Mark one held month with a digest nobody published: the next round
	 * must fetch that month and only that month, and put the digest right.
	 */
	{
		bbq_history history;
		QVERIFY(history.open(path));
		QVERIFY(history.set_server_month(station, 2026 * 12 + 7, QByteArray(16, 'x')));
	}

	bbq_wu_feed feed;
	QVERIFY(feed.open_history(path));
	feed.set_server(m_key.public_key(), {daemon(m_port)});
	feed.set_station(station);
	QVERIFY(settle(feed));

	const auto held = feed.history().server_months(station);
	QCOMPARE(held.size(), size_t(2));
	QVERIFY(held.at(2026 * 12 + 7) != QByteArray(16, 'x'));

	/* One month asked for, not both: the other was already held as published. */
	QCOMPARE(feed.m_months_requested, 1);
}

void test_feed_server::a_month_the_index_does_not_name_is_not_applied() {
	QTemporaryDir scratch;
	bbq_wu_feed feed;
	QVERIFY(feed.open_history(scratch.filePath("history.sqlite")));
	feed.set_server(m_key.public_key(), {daemon(m_port)});
	feed.set_station(swapped);
	QSignalSpy refused(&feed, &bbq_wu_feed::months_refused);
	QSignalSpy failed(&feed, &bbq_wu_feed::band_failed);
	QVERIFY(settle(feed));

	QCOMPARE(feed.m_months_requested, 1);
	QCOMPARE(feed.history().server_months(swapped).size(), size_t(0));
	QCOMPARE(feed.history().observation_months(swapped).size(), size_t(0));

	/*
	 * Refused out loud, naming the month and why, and not as a server
	 * failure: the head applied and the server is as healthy as before.
	 */
	QCOMPARE(refused.size(), 1);
	const QStringList months = refused.at(0).at(0).toStringList();
	QCOMPARE(months, QStringList{QStringLiteral("2026-09: not the copy the index names")});
	QCOMPARE(failed.size(), 0);
	QCOMPARE(feed.server_state().failures, 0);
}

/*
 * The archive refusing every row, by a trigger added from a second
 * connection. A month marked held is never asked for again, and a past
 * month never changes on the server, so held here would be its rows lost
 * for good. Not held, said as the scrape says a short store, and taken
 * whole by the next round once the archive accepts it.
 */
void test_feed_server::a_month_the_archive_refuses_is_not_held() {
	QTemporaryDir scratch;
	const QString path = scratch.filePath("history.sqlite");
	const QString connection = QStringLiteral("refuse");
	{
		bbq_wu_feed feed;
		QVERIFY(feed.open_history(path));
		{
			QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
			db.setDatabaseName(path);
			QVERIFY(db.open());
			QSqlQuery query(db);
			QVERIFY(query.exec(QStringLiteral(
			        "CREATE TRIGGER refuse BEFORE INSERT ON observation "
			        "BEGIN SELECT RAISE(ABORT, 'refused for the test'); END")));
		}
		QSqlDatabase::removeDatabase(connection);

		feed.set_server(m_key.public_key(), {daemon(m_port)});
		feed.set_station(station);
		QSignalSpy failed(&feed, &bbq_wu_feed::band_failed);
		QVERIFY(settle(feed));

		QCOMPARE(feed.history().server_months(station).size(), size_t(0));
		bool said = false;
		for (const QList<QVariant> &arguments : failed) {
			said |= arguments.at(0).toString() == QLatin1String("observed") &&
			        arguments.at(1).toString().contains(QLatin1String("refused for the test"));
		}
		QVERIFY2(said, "a month the archive refused was not said");
	}

	{
		QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
		db.setDatabaseName(path);
		QVERIFY(db.open());
		QSqlQuery query(db);
		QVERIFY(query.exec(QStringLiteral("DROP TRIGGER refuse")));
	}
	QSqlDatabase::removeDatabase(connection);

	bbq_wu_feed feed;
	QVERIFY(feed.open_history(path));
	feed.set_server(m_key.public_key(), {daemon(m_port)});
	feed.set_station(station);
	QVERIFY(settle(feed));
	QCOMPARE(feed.m_months_requested, 2);
	QCOMPARE(feed.history().server_months(station).size(), size_t(2));
	const auto months = feed.history().observation_months(station);
	QCOMPARE(months.size(), size_t(2));
	QCOMPARE(months[1].rows, m_september_rows);
}

void test_feed_server::an_offline_launch_shows_the_last_head() {
	QTemporaryDir scratch;
	const QString path = scratch.filePath("history.sqlite");
	{
		bbq_wu_feed feed;
		QVERIFY(feed.open_history(path));
		feed.set_server(m_key.public_key(), {daemon(m_port)});
		feed.set_station(station);
		QVERIFY(settle(feed));
	}

	/* No route at all, and no round run: the head is there from the store. */
	bbq_wu_feed feed;
	QVERIFY(feed.open_history(path));
	feed.set_server(m_key.public_key(), {daemon(9)});
	feed.set_station(station);
	const bbq_series *hourly = feed.composite().band(bbq_band::hourly);
	QVERIFY(hourly != nullptr);
	QCOMPARE(hourly->fetched_utc(), qint64(1789999000));
}

void test_feed_server::a_station_the_server_does_not_publish_is_not_an_outage() {
	QTemporaryDir scratch;
	bbq_wu_feed feed;
	QVERIFY(feed.open_history(scratch.filePath("history.sqlite")));
	feed.set_server(m_key.public_key(), {daemon(m_port)});
	feed.set_station(QStringLiteral("ISTOCK822"));
	QVERIFY(feed.server_active());

	QVERIFY(settle(feed));

	/* The server answered, so no failure; and this station now goes to the providers. */
	QCOMPARE(feed.server_state().failures, 0);
	QVERIFY(!feed.server_active());

	/*
	 * But not for ever (sec 15.14): the server adds stations clients ask
	 * for, so an hour on it is asked again, and a client told no once
	 * would otherwise never notice the station being added.
	 */
	feed.m_station_not_served_utc -= bbq_unserved_recheck_s;
	QVERIFY(feed.server_active());
	QVERIFY(settle(feed));
	QVERIFY2(!feed.server_active(), "still unserved, so told no again");
	QVERIFY(feed.m_station_not_served_utc > QDateTime::currentSecsSinceEpoch() - 60);
}

/*
 * The other half of sec 15.14: a station the server did not publish an
 * hour ago, and does now. The recheck must take it up -- the head
 * applied and the not-served mark cleared -- or the server's adding it
 * would reach nobody who had asked.
 */
void test_feed_server::a_station_the_server_has_since_added_is_taken_up() {
	QTemporaryDir scratch;
	bbq_wu_feed feed;
	QVERIFY(feed.open_history(scratch.filePath("history.sqlite")));
	feed.set_server(m_key.public_key(), {daemon(m_port)});
	feed.set_station(station);

	feed.m_station_not_served_utc =
	        QDateTime::currentSecsSinceEpoch() - bbq_unserved_recheck_s - 1;
	QVERIFY(feed.server_active());
	QVERIFY(settle(feed));

	QCOMPARE(feed.m_station_not_served_utc, qint64(0));
	QVERIFY2(feed.composite().band(bbq_band::hourly) != nullptr, "the head was not applied");
}

void test_feed_server::a_server_that_does_not_answer_is_counted_not_abandoned() {
	QTemporaryDir scratch;
	bbq_wu_feed feed;
	QVERIFY(feed.open_history(scratch.filePath("history.sqlite")));
	feed.set_server(m_key.public_key(), {daemon(9)});
	feed.set_station(station);

	QSignalSpy failed(&feed, &bbq_wu_feed::band_failed);
	QVERIFY(settle(feed));

	QCOMPARE(feed.server_state().failures, 1);
	bool named = false;
	for (const auto &call : failed) {
		named = named || call.at(0).toString() == QLatin1String("server");
	}
	QVERIFY(named);

	/* One failure is not an outage: still the server's, still not scraping. */
	QVERIFY(!feed.scraping());
	QVERIFY(feed.server_active());
}

void test_feed_server::a_file_signed_by_another_key_is_not_applied() {
	QTemporaryDir scratch;
	bbq_wu_feed feed;
	QVERIFY(feed.open_history(scratch.filePath("history.sqlite")));

	bbq_signing_key other;
	uint8_t seed[32] = {9, 9};
	other.set_seed(seed);
	feed.set_server(other.public_key(), {daemon(m_port)});
	feed.set_station(station);

	QVERIFY(settle(feed));

	/* The manifest does not verify, so it is a failure and nothing is applied. */
	QVERIFY(feed.composite().band(bbq_band::hourly) == nullptr);
	QCOMPARE(feed.history().observation_months(station).size(), size_t(0));
	QCOMPARE(feed.server_state().failures, 1);
}

void test_feed_server::no_key_means_no_server() {
	bbq_wu_feed feed;
	feed.set_server(QByteArray(), {daemon(m_port)});
	feed.set_station(station);
	QVERIFY(!feed.server_active());
}

/*
 * Radar and extended, unreachable through the hermetic proxy, settle
 * through band_quiet and never band_failed (sec 19). The display hears
 * only the second, where a permanent error for everybody outside MET's
 * coverage would be wrong; the diagnostic hears the first, without which
 * a timeout named them as bands that had not answered yet.
 */
void test_feed_server::the_keyless_bands_settle_quietly_when_unreachable() {
	QTemporaryDir scratch;
	bbq_wu_feed feed;
	QVERIFY(feed.open_history(scratch.filePath("history.sqlite")));
	feed.set_station(station);
	feed.set_geocode(59.33, 18.07);
	QVERIFY(!feed.server_active());

	QSignalSpy quiet(&feed, &bbq_wu_feed::band_quiet);
	QSignalSpy failed(&feed, &bbq_wu_feed::band_failed);
	QVERIFY(settle(feed));

	QStringList quietly;
	for (const auto &call : quiet) {
		quietly << call.at(0).toString();
		QVERIFY2(!call.at(1).toString().isEmpty(), "a quiet band with no reason");
	}
	quietly.sort();
	QCOMPARE(quietly, QStringList({QStringLiteral("extended"), QStringLiteral("radar")}));

	for (const auto &call : failed) {
		const QString band = call.at(0).toString();
		QVERIFY2(band != QLatin1String("radar") && band != QLatin1String("extended"),
		         qPrintable(band + QStringLiteral(" was announced as failed")));
	}
}

QTEST_GUILESS_MAIN(test_feed_server)
#include "test_feed_server.moc"
