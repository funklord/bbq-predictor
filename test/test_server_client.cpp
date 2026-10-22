#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
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
#include "server/transport.h"

Q_DECLARE_METATYPE(std::vector<bbq_server_result>)

/*
 * The client's routes (project.md sec 15.12.2), against the real daemon
 * loop on loopback and three stand-ins for what a network can put in
 * the way: a port that answers nothing, a captive portal that answers
 * everything with a web page, and a plain web server for the HTTPS
 * route (spoken as plain HTTP here; the route is the URL it is given).
 */
class test_server_client : public QObject {
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();

	void a_refused_route_falls_through_and_the_good_one_is_remembered();
	void what_the_server_lacks_is_not_a_failure();
	void a_held_copy_comes_back_unchanged();
	void a_silent_route_is_given_up_on_its_deadline();
	void a_captive_portal_is_not_mistaken_for_the_server();
	void a_well_shaped_answer_without_our_magic_is_refused();
	void the_https_route_reads_the_same_tree();
	void an_https_body_over_its_cap_is_refused();
	void when_no_route_answers_every_request_fails();

private:
	std::vector<bbq_server_result> fetch(bbq_server_client &client,
	                                     std::vector<bbq_server_request> requests);
	static bbq_server_route daemon(quint16 port);
	static quint16 closed_port();

	QTemporaryDir m_root;
	bbq_signing_key m_key;
	QByteArray m_head;

	int m_dir_fd = -1;
	int m_listen_fd = -1;
	int m_wake[2] = {-1, -1};
	quint16 m_port = 0;
	std::thread m_thread;

	QTcpServer m_silent;
	QTcpServer m_portal;
	QTcpServer m_impostor;
	QTcpServer m_web;
	QNetworkAccessManager m_network;
};

namespace {

bbq_server_request ask(bbq_wire_kind kind, const QString &station = QString(),
                       const QByteArray &have = QByteArray()) {
	bbq_server_request request;
	request.kind = kind;
	request.station = station;
	request.have = have;
	return request;
}

void write(const QString &path, const QByteArray &bytes) {
	QDir().mkpath(QFileInfo(path).path());
	QFile out(path);
	out.open(QIODevice::WriteOnly);
	out.write(bytes);
}

} // namespace

bbq_server_route test_server_client::daemon(quint16 port) {
	bbq_server_route route;
	route.type = bbq_server_route::kind::daemon;
	route.host = QStringLiteral("127.0.0.1");
	route.port = port;
	return route;
}

quint16 test_server_client::closed_port() {
	/* Bound, read, released: nothing listens there afterwards. */
	QTcpServer probe;
	probe.listen(QHostAddress::LocalHost, 0);
	const quint16 port = probe.serverPort();
	probe.close();
	return port;
}

std::vector<bbq_server_result> test_server_client::fetch(bbq_server_client &client,
                                                         std::vector<bbq_server_request> requests) {
	QSignalSpy spy(&client, &bbq_server_client::finished);
	client.fetch(std::move(requests));
	if (!spy.wait(10000)) {
		return {};
	}
	return spy.at(0).at(0).value<std::vector<bbq_server_result>>();
}

void test_server_client::initTestCase() {
	qRegisterMetaType<std::vector<bbq_server_result>>();

	uint8_t seed[32] = {7, 7, 7};
	m_key.set_seed(seed);

	/* A published tree: a manifest and one station's head. */
	QString why;
	const QString dir = m_root.path();
	write(dir + "/stations",
	      bbq_wire_file(bbq_wire_kind::stations,
	                    bbq_wire_stations_body(1, {QStringLiteral("ISTOCK877")}, &why), m_key));
	bbq_series hourly(bbq_band::hourly, QStringLiteral("wunderground"));
	bbq_sample sample;
	sample.start_utc = 1790000000;
	sample.duration_s = 3600;
	sample.temperature = 12.5;
	hourly.set_samples({sample});
	m_head = bbq_wire_file(bbq_wire_kind::head,
	                       bbq_wire_head_body(QStringLiteral("ISTOCK877"), 1, {&hourly}, &why),
	                       m_key);
	write(dir + "/ISTOCK877/head", m_head);

	/* The real daemon loop, on a loopback port of its own. */
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

	/* Accepts, and never says a word. */
	QVERIFY(m_silent.listen(QHostAddress::LocalHost, 0));

	/* Answers everything with a web page, the way a hotel network does. */
	QVERIFY(m_portal.listen(QHostAddress::LocalHost, 0));
	connect(&m_portal, &QTcpServer::newConnection, this, [this]() {
		QTcpSocket *socket = m_portal.nextPendingConnection();
		socket->write("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nlogin");
		socket->disconnectFromHost();
	});

	/*
	 * Answers in exactly the daemon's shape -- status 0, a plausible
	 * length -- with someone else's magic. The portal above is refused
	 * for its status byte before the magic is ever looked at, so without
	 * this the magic check is untested: a sabotage removing it passed.
	 */
	QVERIFY(m_impostor.listen(QHostAddress::LocalHost, 0));
	connect(&m_impostor, &QTcpServer::newConnection, this, [this]() {
		QTcpSocket *socket = m_impostor.nextPendingConnection();
		socket->write(QByteArray("JUNK\0\0\0\0\x05\0\0\0hello", 17));
		socket->disconnectFromHost();
	});

	/* A plain web server over the tree, standing in for one on 443. */
	QVERIFY(m_web.listen(QHostAddress::LocalHost, 0));
	connect(&m_web, &QTcpServer::newConnection, this, [this, dir]() {
		QTcpSocket *socket = m_web.nextPendingConnection();
		connect(socket, &QTcpSocket::readyRead, socket, [socket, dir]() {
			const QByteArray line = socket->readLine();
			const QByteArray path = line.split(' ').value(1);
			QByteArray body;
			int code = 404;
			if (path.startsWith("/bbq-predictor/")) {
				QFile file(dir + QLatin1Char('/') + QString::fromLatin1(path.mid(15)));
				if (path.contains("/huge/")) {
					code = 200;
					body = QByteArray(1024 * 1024 + 10, 'x');
				} else if (file.open(QIODevice::ReadOnly)) {
					code = 200;
					body = file.readAll();
				}
			}
			socket->readAll();
			socket->write(QByteArray("HTTP/1.1 ") + QByteArray::number(code) +
			              (code == 200 ? " OK" : " Not Found") +
			              "\r\nContent-Length: " + QByteArray::number(body.size()) +
			              "\r\nConnection: close\r\n\r\n" + body);
			socket->disconnectFromHost();
		});
	});
}

void test_server_client::cleanupTestCase() {
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

void test_server_client::a_refused_route_falls_through_and_the_good_one_is_remembered() {
	bbq_server_client client(&m_network);
	client.set_routes({daemon(closed_port()), daemon(m_port)});

	const auto results = fetch(client, {ask(bbq_wire_kind::stations),
	                                    ask(bbq_wire_kind::head, "ISTOCK877")});
	QCOMPARE(results.size(), size_t(2));
	QCOMPARE(results[0].status, bbq_server_status::ok);
	QCOMPARE(results[1].status, bbq_server_status::ok);
	QCOMPARE(results[1].file, m_head);
	QCOMPARE(client.preferred_route(), 1);
}

void test_server_client::what_the_server_lacks_is_not_a_failure() {
	bbq_server_client client(&m_network);
	client.set_routes({daemon(m_port)});

	const auto results = fetch(client, {ask(bbq_wire_kind::head, "ISTOCK000"),
	                                    ask(bbq_wire_kind::index, "ISTOCK877")});
	QCOMPARE(results.size(), size_t(2));
	QCOMPARE(results[0].status, bbq_server_status::not_served);
	QCOMPARE(results[1].status, bbq_server_status::not_served);
	QCOMPARE(client.preferred_route(), 0);
}

void test_server_client::a_held_copy_comes_back_unchanged() {
	bbq_server_client client(&m_network);
	client.set_routes({daemon(m_port)});

	const auto results = fetch(client, {ask(bbq_wire_kind::head, "ISTOCK877", m_head)});
	QCOMPARE(results.size(), size_t(1));
	QCOMPARE(results[0].status, bbq_server_status::unchanged);
	QVERIFY(results[0].file.isEmpty());
}

void test_server_client::a_silent_route_is_given_up_on_its_deadline() {
	bbq_server_client client(&m_network);
	client.set_timeout_ms(300);
	client.set_routes({daemon(m_silent.serverPort()), daemon(m_port)});

	QElapsedTimer timer;
	timer.start();
	auto results = fetch(client, {ask(bbq_wire_kind::head, "ISTOCK877")});
	QCOMPARE(results.size(), size_t(1));
	QCOMPARE(results[0].status, bbq_server_status::ok);
	/*
	 * It waited for the deadline rather than failing at once. Not >= 300:
	 * a QTimer is coarse by default and may fire up to 5% early, and an
	 * earlier threshold of 290 failed about one run in two for exactly
	 * that reason.
	 */
	QVERIFY2(timer.elapsed() >= 250, qPrintable(QString::number(timer.elapsed())));

	/* And the next round starts where it worked, without waiting again. */
	QCOMPARE(client.preferred_route(), 1);
	timer.restart();
	results = fetch(client, {ask(bbq_wire_kind::head, "ISTOCK877")});
	QCOMPARE(results[0].status, bbq_server_status::ok);
	QVERIFY2(timer.elapsed() < 250, qPrintable(QString::number(timer.elapsed())));
}

void test_server_client::a_captive_portal_is_not_mistaken_for_the_server() {
	bbq_server_client client(&m_network);
	client.set_routes({daemon(m_portal.serverPort()), daemon(m_port)});

	const auto results = fetch(client, {ask(bbq_wire_kind::head, "ISTOCK877")});
	QCOMPARE(results[0].status, bbq_server_status::ok);
	QCOMPARE(results[0].file, m_head);
	QCOMPARE(client.preferred_route(), 1);
}

void test_server_client::a_well_shaped_answer_without_our_magic_is_refused() {
	bbq_server_client client(&m_network);
	client.set_routes({daemon(m_impostor.serverPort()), daemon(m_port)});

	const auto results = fetch(client, {ask(bbq_wire_kind::head, "ISTOCK877")});
	QCOMPARE(results[0].status, bbq_server_status::ok);
	QCOMPARE(results[0].file, m_head);
	QCOMPARE(client.preferred_route(), 1);
}

void test_server_client::the_https_route_reads_the_same_tree() {
	bbq_server_client client(&m_network);
	bbq_server_route web;
	web.type = bbq_server_route::kind::https;
	web.base = QUrl(QStringLiteral("http://127.0.0.1:%1/bbq-predictor/").arg(m_web.serverPort()));
	client.set_routes({daemon(closed_port()), web});

	const auto results = fetch(client, {ask(bbq_wire_kind::head, "ISTOCK877"),
	                                    ask(bbq_wire_kind::head, "ISTOCK000"),
	                                    ask(bbq_wire_kind::head, "ISTOCK877", m_head)});
	QCOMPARE(results.size(), size_t(3));
	QCOMPARE(results[0].status, bbq_server_status::ok);
	QCOMPARE(results[0].file, m_head);
	QCOMPARE(results[1].status, bbq_server_status::not_served);
	QCOMPARE(results[2].status, bbq_server_status::unchanged);
	QCOMPARE(client.preferred_route(), 1);
}

void test_server_client::an_https_body_over_its_cap_is_refused() {
	bbq_server_client client(&m_network);
	bbq_server_route web;
	web.type = bbq_server_route::kind::https;
	web.base = QUrl(QStringLiteral("http://127.0.0.1:%1/bbq-predictor/").arg(m_web.serverPort()));
	client.set_routes({web});

	/* A head is capped at 1 MiB; this one is 1 MiB and ten bytes. */
	const auto results = fetch(client, {ask(bbq_wire_kind::head, "huge")});
	QCOMPARE(results.size(), size_t(1));
	QCOMPARE(results[0].status, bbq_server_status::failed);
}

void test_server_client::when_no_route_answers_every_request_fails() {
	bbq_server_client client(&m_network);
	client.set_timeout_ms(300);
	client.set_routes({daemon(closed_port()), daemon(m_silent.serverPort())});

	const auto results = fetch(client, {ask(bbq_wire_kind::stations),
	                                    ask(bbq_wire_kind::head, "ISTOCK877")});
	QCOMPARE(results.size(), size_t(2));
	QCOMPARE(results[0].status, bbq_server_status::failed);
	QCOMPARE(results[1].status, bbq_server_status::failed);
	QVERIFY(!client.is_busy());
}

QTEST_GUILESS_MAIN(test_server_client)
#include "test_server_client.moc"
