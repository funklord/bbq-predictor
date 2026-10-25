#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <thread>

#include "serve/serve_core.h"
#include "serve/serve_loop.h"

/*
 * The daemon's poll loop over real sockets (project.md sec 15.12.2): run
 * in a thread on a loopback port, driven by a client that reads with a
 * timeout so a loop that stops answering fails a case rather than
 * hanging the suite.
 */
class test_serve_loop : public QObject {
	Q_OBJECT

private slots:
	void init();
	void cleanup();

	void a_request_is_answered_with_the_file();
	void a_held_copy_is_answered_unchanged();
	void several_requests_share_one_connection();
	void a_bad_request_is_answered_and_closed();
	void one_address_cannot_hold_every_connection();
	void a_silent_connection_is_closed();
	void a_connection_answers_only_so_many_requests();
	void a_new_manifest_is_picked_up_without_a_signal();
	void a_hangup_reloads_at_once();
	void demand_is_written_for_what_clients_ask();
	void a_full_demand_file_is_left_alone();
	void a_locked_demand_file_does_not_stall_the_loop();

private:
	void start(const bbq_serve_limits &limits, int demand_fd = -1);
	int connect_client();
	static QByteArray request(uint8_t kind, const char *station,
	                          const uint8_t *have = nullptr);
	static QByteArray file(int size, char fill);
	void publish(const QString &relative, const QByteArray &bytes);

	/* Reads exactly n bytes, or fewer if the peer closes or 2 s pass. */
	static QByteArray read_exactly(int fd, int n);

	/* The response's status, or -1 when no whole header arrived. */
	static int status_of(const QByteArray &header);
	static uint32_t length_of(const QByteArray &header);

	QTemporaryDir *m_root = nullptr;
	int m_dir_fd = -1;
	int m_listen_fd = -1;
	int m_wake[2] = {-1, -1};
	int m_port = 0;
	std::thread m_thread;
	int m_result = -2;
};

QByteArray test_serve_loop::request(uint8_t kind, const char *station,
                                    const uint8_t *have) {
	QByteArray buffer(BBQ_SERVE_REQUEST_SIZE, '\0');
	std::memcpy(buffer.data(), "BBQR", 4);
	buffer[4] = 1;
	buffer[5] = char(kind);
	std::memcpy(buffer.data() + 8, station, std::strlen(station));
	if (have) {
		std::memcpy(buffer.data() + 40, have, BBQ_SERVE_HAVE_SIZE);
	}
	return buffer;
}

QByteArray test_serve_loop::file(int size, char fill) {
	return QByteArray(size, fill);
}

void test_serve_loop::publish(const QString &relative, const QByteArray &bytes) {
	/* As the publisher will: write aside, then rename into place. */
	const QString path = m_root->path() + "/" + relative;
	QDir().mkpath(QFileInfo(path).path());
	QFile out(path + ".new");
	QVERIFY(out.open(QIODevice::WriteOnly));
	QCOMPARE(out.write(bytes), qint64(bytes.size()));
	out.close();
	QVERIFY(::rename(QFile::encodeName(path + ".new").constData(),
	                 QFile::encodeName(path).constData()) == 0);
}

QByteArray test_serve_loop::read_exactly(int fd, int n) {
	QByteArray out;
	while (out.size() < n) {
		char buffer[4096];
		const ssize_t got = ::recv(fd, buffer,
		                           size_t(std::min(n - int(out.size()), int(sizeof buffer))),
		                           0);
		if (got <= 0) {
			break;
		}
		out.append(buffer, int(got));
	}
	return out;
}

int test_serve_loop::status_of(const QByteArray &header) {
	if (header.size() != BBQ_SERVE_RESPONSE_HEADER_SIZE ||
	    !header.startsWith("BBQA")) {
		return -1;
	}
	return uint8_t(header[4]);
}

uint32_t test_serve_loop::length_of(const QByteArray &header) {
	const auto *p = reinterpret_cast<const uint8_t *>(header.constData()) + 8;
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
	       uint32_t(p[3]) << 24;
}

void test_serve_loop::init() {
	m_root = new QTemporaryDir;
	QVERIFY(m_root->isValid());

	publish("stations", file(BBQ_SERVE_FILE_MIN, 's'));
	publish("ISTOCK877/head", file(200, 'h'));
	publish("ISTOCK877/index", file(300, 'i'));

	m_dir_fd = ::open(QFile::encodeName(m_root->path()).constData(),
	                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	QVERIFY(m_dir_fd >= 0);

	m_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	QVERIFY(m_listen_fd >= 0);
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	QVERIFY(::bind(m_listen_fd, reinterpret_cast<sockaddr *>(&address),
	               sizeof address) == 0);
	QVERIFY(::listen(m_listen_fd, 64) == 0);
	socklen_t length = sizeof address;
	QVERIFY(::getsockname(m_listen_fd, reinterpret_cast<sockaddr *>(&address),
	                      &length) == 0);
	m_port = ntohs(address.sin_port);

	QVERIFY(::pipe2(m_wake, O_CLOEXEC | O_NONBLOCK) == 0);
}

void test_serve_loop::cleanup() {
	if (m_thread.joinable()) {
		const char quit = 'q';
		QCOMPARE(::write(m_wake[1], &quit, 1), ssize_t(1));
		m_thread.join();
		QCOMPARE(m_result, 0);
	}
	for (int fd : {m_dir_fd, m_listen_fd, m_wake[0], m_wake[1]}) {
		if (fd >= 0) {
			::close(fd);
		}
	}
	m_dir_fd = m_listen_fd = m_wake[0] = m_wake[1] = -1;
	delete m_root;
	m_root = nullptr;
}

void test_serve_loop::start(const bbq_serve_limits &limits, int demand_fd) {
	m_thread = std::thread([this, limits, demand_fd] {
		bbq_serve_loop loop = {};
		loop.dir_fd = m_dir_fd;
		loop.listen_fds = &m_listen_fd;
		loop.listen_count = 1;
		loop.wake_fd = m_wake[0];
		loop.limits = limits;
		loop.record_demand = demand_fd >= 0;
		loop.demand_fd = demand_fd;
		m_result = bbq_serve_run(&loop);
	});
}

int test_serve_loop::connect_client() {
	const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	timeval timeout = {2, 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(uint16_t(m_port));
	if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
		::close(fd);
		return -1;
	}
	return fd;
}

static bool send_all(int fd, const QByteArray &bytes) {
	return ::send(fd, bytes.constData(), size_t(bytes.size()), MSG_NOSIGNAL) ==
	       bytes.size();
}

void test_serve_loop::a_request_is_answered_with_the_file() {
	start(bbq_serve_default_limits());

	const int fd = connect_client();
	QVERIFY(fd >= 0);
	QVERIFY(send_all(fd, request(BBQ_SERVE_HEAD, "ISTOCK877")));

	const QByteArray header = read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE);
	QCOMPARE(status_of(header), int(BBQ_SERVE_OK));
	QCOMPARE(length_of(header), uint32_t(200));
	QCOMPARE(read_exactly(fd, 200), file(200, 'h'));

	QVERIFY(send_all(fd, request(BBQ_SERVE_HEAD, "ISTOCK000")));
	QCOMPARE(status_of(read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE)),
	         int(BBQ_SERVE_NOT_SERVED));
	::close(fd);
}

void test_serve_loop::a_held_copy_is_answered_unchanged() {
	start(bbq_serve_default_limits());

	uint8_t have[BBQ_SERVE_HAVE_SIZE];
	std::memset(have, 'h', sizeof have);

	const int fd = connect_client();
	QVERIFY(fd >= 0);
	QVERIFY(send_all(fd, request(BBQ_SERVE_HEAD, "ISTOCK877", have)));
	const QByteArray header = read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE);
	QCOMPARE(status_of(header), int(BBQ_SERVE_UNCHANGED));
	QCOMPARE(length_of(header), uint32_t(0));
	::close(fd);
}

void test_serve_loop::several_requests_share_one_connection() {
	start(bbq_serve_default_limits());

	const int fd = connect_client();
	QVERIFY(fd >= 0);

	/* Sent back to back before reading anything: pipelined. */
	QVERIFY(send_all(fd, request(BBQ_SERVE_INDEX, "ISTOCK877") +
	                             request(BBQ_SERVE_HEAD, "ISTOCK877") +
	                             request(BBQ_SERVE_STATIONS, "")));

	QCOMPARE(length_of(read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE)), uint32_t(300));
	QCOMPARE(read_exactly(fd, 300), file(300, 'i'));
	QCOMPARE(length_of(read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE)), uint32_t(200));
	QCOMPARE(read_exactly(fd, 200), file(200, 'h'));
	QCOMPARE(length_of(read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE)),
	         uint32_t(BBQ_SERVE_FILE_MIN));
	QCOMPARE(read_exactly(fd, BBQ_SERVE_FILE_MIN), file(BBQ_SERVE_FILE_MIN, 's'));
	::close(fd);
}

void test_serve_loop::a_bad_request_is_answered_and_closed() {
	start(bbq_serve_default_limits());

	const int fd = connect_client();
	QVERIFY(fd >= 0);
	QByteArray bad = request(BBQ_SERVE_HEAD, "ISTOCK877");
	bad[0] = 'X';
	QVERIFY(send_all(fd, bad + request(BBQ_SERVE_HEAD, "ISTOCK877")));

	QCOMPARE(status_of(read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE)),
	         int(BBQ_SERVE_BAD_REQUEST));

	/* The good request after it is never answered: the connection is gone. */
	QCOMPARE(read_exactly(fd, 1), QByteArray());
	::close(fd);
}

void test_serve_loop::one_address_cannot_hold_every_connection() {
	bbq_serve_limits limits = bbq_serve_default_limits();
	limits.per_address = 2;
	start(limits);

	const int first = connect_client();
	const int second = connect_client();
	QVERIFY(first >= 0 && second >= 0);

	/* Make sure both are held before the third arrives. */
	for (int fd : {first, second}) {
		QVERIFY(send_all(fd, request(BBQ_SERVE_HEAD, "ISTOCK877")));
		QCOMPARE(status_of(read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE)),
		         int(BBQ_SERVE_OK));
		QCOMPARE(read_exactly(fd, 200).size(), 200);
	}

	const int third = connect_client();
	QVERIFY(third >= 0);
	QCOMPARE(status_of(read_exactly(third, BBQ_SERVE_RESPONSE_HEADER_SIZE)),
	         int(BBQ_SERVE_BUSY));
	QCOMPARE(read_exactly(third, 1), QByteArray());

	for (int fd : {first, second, third}) {
		::close(fd);
	}
}

void test_serve_loop::a_silent_connection_is_closed() {
	bbq_serve_limits limits = bbq_serve_default_limits();
	limits.idle_timeout_ms = 200;
	start(limits);

	const int fd = connect_client();
	QVERIFY(fd >= 0);

	/* Half a request, then nothing: the loop must not wait for the rest. */
	QVERIFY(send_all(fd, request(BBQ_SERVE_HEAD, "ISTOCK877").left(20)));
	QElapsedTimer timer;
	timer.start();
	QCOMPARE(read_exactly(fd, 1), QByteArray());
	QVERIFY2(timer.elapsed() < 1500, "closed by the idle timeout, not the read timeout");
	::close(fd);
}

void test_serve_loop::a_connection_answers_only_so_many_requests() {
	bbq_serve_limits limits = bbq_serve_default_limits();
	limits.requests_per_connection = 2;
	start(limits);

	const int fd = connect_client();
	QVERIFY(fd >= 0);
	for (int i = 0; i < 2; ++i) {
		QVERIFY(send_all(fd, request(BBQ_SERVE_INDEX, "ISTOCK877")));
		QCOMPARE(status_of(read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE)),
		         int(BBQ_SERVE_OK));
		QCOMPARE(read_exactly(fd, 300).size(), 300);
	}
	QCOMPARE(read_exactly(fd, 1), QByteArray());
	::close(fd);
}

void test_serve_loop::a_new_manifest_is_picked_up_without_a_signal() {
	bbq_serve_limits limits = bbq_serve_default_limits();
	limits.reload_check_ms = 50;
	start(limits);

	/* A new head, then the manifest last, as the publisher orders it. */
	publish("ISTOCK877/head", file(250, 'H'));
	publish("stations", file(BBQ_SERVE_FILE_MIN + 1, 's'));

	QByteArray body;
	for (int attempt = 0; attempt < 40 && body != file(250, 'H'); ++attempt) {
		QTest::qWait(25);
		const int fd = connect_client();
		QVERIFY(fd >= 0);
		QVERIFY(send_all(fd, request(BBQ_SERVE_HEAD, "ISTOCK877")));
		const QByteArray header = read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE);
		QCOMPARE(status_of(header), int(BBQ_SERVE_OK));
		body = read_exactly(fd, int(length_of(header)));
		::close(fd);
	}
	QCOMPARE(body, file(250, 'H'));
}

void test_serve_loop::a_hangup_reloads_at_once() {
	/* A check interval far longer than the case, so only the byte can do it. */
	bbq_serve_limits limits = bbq_serve_default_limits();
	limits.reload_check_ms = 600000;
	start(limits);

	publish("ISTOCK877/head", file(260, 'J'));
	const char hangup = 'h';
	QCOMPARE(::write(m_wake[1], &hangup, 1), ssize_t(1));

	QByteArray body;
	for (int attempt = 0; attempt < 40 && body != file(260, 'J'); ++attempt) {
		QTest::qWait(25);
		const int fd = connect_client();
		QVERIFY(fd >= 0);
		QVERIFY(send_all(fd, request(BBQ_SERVE_HEAD, "ISTOCK877")));
		const QByteArray header = read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE);
		QCOMPARE(status_of(header), int(BBQ_SERVE_OK));
		body = read_exactly(fd, int(length_of(header)));
		::close(fd);
	}
	QCOMPARE(body, file(260, 'J'));
}

/*
 * Demand through the real loop (sec 15.14): a published station, four
 * unpublished ones from one address, and the manifest, which names no
 * station. The demand is written before the answer is, so by the time
 * the last header has arrived every line that will be written has been.
 */
void test_serve_loop::demand_is_written_for_what_clients_ask() {
	const QString path = m_root->filePath(QStringLiteral("demand"));
	const int demand_fd = ::open(QFile::encodeName(path).constData(),
	                             O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	QVERIFY(demand_fd >= 0);
	start(bbq_serve_default_limits(), demand_fd);

	const int fd = connect_client();
	QVERIFY(fd >= 0);
	for (const char *station : {"ISTOCK877", "INEWA1", "INEWB2", "INEWC3", "INEWD4"}) {
		QVERIFY(send_all(fd, request(BBQ_SERVE_HEAD, station)));
		const QByteArray header = read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE);
		QVERIFY(status_of(header) >= 0);
		QCOMPARE(read_exactly(fd, int(length_of(header))).size(), int(length_of(header)));
	}
	QVERIFY(send_all(fd, request(BBQ_SERVE_STATIONS, "")));
	QCOMPARE(status_of(read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE)), int(BBQ_SERVE_OK));
	::close(fd);

	QFile written(path);
	QVERIFY(written.open(QIODevice::ReadOnly));
	QStringList stations;
	for (const QByteArray &line : written.readAll().split('\n')) {
		if (!line.isEmpty()) {
			stations << QString::fromLatin1(line.split(' ').value(0));
		}
	}
	QCOMPARE(stations, QStringList({QStringLiteral("ISTOCK877"), QStringLiteral("INEWA1"),
	                                QStringLiteral("INEWB2"), QStringLiteral("INEWC3")}));
	::close(demand_fd);
}

void test_serve_loop::a_full_demand_file_is_left_alone() {
	const QString path = m_root->filePath(QStringLiteral("demand"));
	const int demand_fd = ::open(QFile::encodeName(path).constData(),
	                             O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	QVERIFY(demand_fd >= 0);
	QVERIFY(::ftruncate(demand_fd, off_t(BBQ_SERVE_DEMAND_FILE_MAX)) == 0);
	start(bbq_serve_default_limits(), demand_fd);

	const int fd = connect_client();
	QVERIFY(fd >= 0);
	QVERIFY(send_all(fd, request(BBQ_SERVE_HEAD, "ISTOCK877")));
	QCOMPARE(status_of(read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE)), int(BBQ_SERVE_OK));
	::close(fd);

	QCOMPARE(QFileInfo(path).size(), qint64(BBQ_SERVE_DEMAND_FILE_MAX));

	/* Emptied, as the publisher would: the dropped line is due again. */
	QVERIFY(::ftruncate(demand_fd, 0) == 0);
	const int again = connect_client();
	QVERIFY(again >= 0);
	QVERIFY(send_all(again, request(BBQ_SERVE_HEAD, "ISTOCK877")));
	QCOMPARE(status_of(read_exactly(again, BBQ_SERVE_RESPONSE_HEADER_SIZE)), int(BBQ_SERVE_OK));
	::close(again);
	QVERIFY(QFileInfo(path).size() > 0);
	::close(demand_fd);
}

/*
 * The publisher holds the file's lock from its read to its truncation. The
 * loop must answer while it is held -- the client's read times out at two
 * seconds otherwise -- write nothing, and write the line on the next
 * request once the lock is free rather than an hour later.
 */
void test_serve_loop::a_locked_demand_file_does_not_stall_the_loop() {
	const QString path = m_root->filePath(QStringLiteral("demand"));
	const QByteArray name = QFile::encodeName(path);
	const int demand_fd = ::open(name.constData(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	QVERIFY(demand_fd >= 0);
	const int publisher_fd = ::open(name.constData(), O_RDWR | O_CLOEXEC);
	QVERIFY(publisher_fd >= 0);
	/* Closed on any exit: a failed case left holding the lock would hold a
	 * loop that waits for it, and cleanup's join with it, until the
	 * suite's timeout. */
	const std::unique_ptr<const int, void (*)(const int *)> unlock(
	        &publisher_fd, [](const int *held) { ::close(*held); });
	QVERIFY(::flock(publisher_fd, LOCK_EX) == 0);
	start(bbq_serve_default_limits(), demand_fd);

	const int fd = connect_client();
	QVERIFY(fd >= 0);
	QVERIFY(send_all(fd, request(BBQ_SERVE_HEAD, "ISTOCK877")));
	QCOMPARE(status_of(read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE)), int(BBQ_SERVE_OK));
	QCOMPARE(read_exactly(fd, 200).size(), 200);
	QCOMPARE(QFileInfo(path).size(), qint64(0));

	QVERIFY(::flock(publisher_fd, LOCK_UN) == 0);
	QVERIFY(send_all(fd, request(BBQ_SERVE_HEAD, "ISTOCK877")));
	QCOMPARE(status_of(read_exactly(fd, BBQ_SERVE_RESPONSE_HEADER_SIZE)), int(BBQ_SERVE_OK));
	::close(fd);

	QFile written(path);
	QVERIFY(written.open(QIODevice::ReadOnly));
	QVERIFY(written.readAll().startsWith("ISTOCK877 "));
	::close(demand_fd);
}

QTEST_GUILESS_MAIN(test_serve_loop)
#include "test_serve_loop.moc"
