#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "publish/demand.h"

/*
 * The publisher's main and the applet's refusal of what moved to it
 * (project.md sec 15.13), checked by running both programs.
 *
 * What is guarded here is all in main(), which no test can link: the
 * key is loaded before any round is fetched, so a bad key costs a message
 * and not somebody else's quota; a key is never overwritten; and the
 * applet refuses the publishing options by name rather than ignoring
 * them, which is what it did for an afternoon -- a command line written
 * for --publish fetched and quietly published nothing.
 *
 * Each refusal is checked by what it left on disk as well as by its exit
 * code, because "refused politely and did it anyway" passes an exit-code
 * check.
 */
class test_publisher : public QObject {
	Q_OBJECT

private slots:
	void initTestCase();
	void it_refuses_without_a_tree_and_a_key();
	void a_key_others_can_read_is_refused_before_fetching();
	void a_key_is_made_once_and_never_overwritten();
	void the_version_names_the_copyright_holder();
	void the_applet_refuses_what_moved_here();
	void demand_is_saved_before_the_round_fetches();

private:
	QString m_publisher;
	QString m_applet;
	bool m_no_applet = false;

	int run(const QString &binary, const QTemporaryDir &home,
	        const QStringList &arguments, QString *said,
	        QString *standard_output = nullptr);
};

void test_publisher::initTestCase() {
	m_publisher = QString::fromLocal8Bit(qgetenv("BBQ_PUBLISH_BINARY"));
	m_applet = QString::fromLocal8Bit(qgetenv("BBQ_APP_BINARY"));

	/*
	 * Failed, not skipped, for the reason test_seed gives -- except that
	 * a server-only build (sec 15.15) makes no applet, and says so with
	 * BBQ_NO_APPLET. Told, not inferred: an applet that is merely
	 * missing still fails here.
	 */
	m_no_applet = qgetenv("BBQ_NO_APPLET") == "1";
	QVERIFY2(!m_publisher.isEmpty() && (m_no_applet || !m_applet.isEmpty()),
	         "BBQ_PUBLISH_BINARY or BBQ_APP_BINARY is unset; run this "
	         "through `make test`");
	QVERIFY2(QFile::exists(m_publisher),
	         qPrintable(QStringLiteral("no binary at %1").arg(m_publisher)));
	QVERIFY2(m_no_applet || QFile::exists(m_applet),
	         qPrintable(QStringLiteral("no binary at %1").arg(m_applet)));
}

int test_publisher::run(const QString &binary, const QTemporaryDir &home,
                        const QStringList &arguments, QString *said,
                        QString *standard_output) {
	/*
	 * Every standard location moved into the temporary directory, so a
	 * guard that fails lands somewhere harmless and somewhere this test
	 * looks. No QT_QPA_PLATFORM: the publisher builds nothing that needs
	 * one, and the applet refuses before building anything at all, so
	 * setting it would hide the regression of either starting to.
	 */
	QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
	environment.insert(QStringLiteral("XDG_DATA_HOME"), home.path());
	environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home.path());
	environment.insert(QStringLiteral("XDG_CACHE_HOME"), home.path());
	environment.remove(QStringLiteral("DISPLAY"));
	environment.remove(QStringLiteral("WAYLAND_DISPLAY"));
	environment.remove(QStringLiteral("QT_QPA_PLATFORM"));

	/*
	 * The channels are read apart, so a check of what a program printed on
	 * stdout is not broken by what Qt says on stderr: in the C locale, as
	 * CI runs, Qt warns that it is not UTF-8 before the program prints a
	 * word, and --version's first line stopped being the first line.
	 */
	QProcess process;
	process.setProcessEnvironment(environment);
	process.start(binary, arguments);
	if (!process.waitForFinished(60000)) {
		process.kill();
		process.waitForFinished();
		*said = QStringLiteral("(did not finish)");
		return -1;
	}

	const QString out = QString::fromLocal8Bit(process.readAllStandardOutput());
	*said = out + QString::fromLocal8Bit(process.readAllStandardError());
	if (standard_output != nullptr) {
		*standard_output = out;
	}
	return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

void test_publisher::it_refuses_without_a_tree_and_a_key() {
	QTemporaryDir home;
	QVERIFY(home.isValid());
	QString said;

	QCOMPARE(run(m_publisher, home, {}, &said), 2);
	QVERIFY2(said.contains(QStringLiteral("--dir DIR and --signing-key FILE")),
	         qPrintable(said));

	const QString tree = home.filePath(QStringLiteral("published"));
	QCOMPARE(run(m_publisher, home, {QStringLiteral("--dir"), tree}, &said), 2);
	QVERIFY2(!QFileInfo::exists(tree), "a refused run created the tree");
}

void test_publisher::a_key_others_can_read_is_refused_before_fetching() {
	QTemporaryDir home;
	QVERIFY(home.isValid());
	QString said;

	const QString key = home.filePath(QStringLiteral("signing.key"));
	QCOMPARE(run(m_publisher, home, {QStringLiteral("--generate-signing-key"), key}, &said),
	         0);
	QVERIFY(QFile::setPermissions(key, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
	                                           QFileDevice::ReadOther));

	const QString tree = home.filePath(QStringLiteral("published"));
	const QString archive = home.filePath(QStringLiteral("archive.sqlite"));
	QStringList arguments;
	arguments << QStringLiteral("--dir") << tree << QStringLiteral("--signing-key") << key
	          << QStringLiteral("--station") << QStringLiteral("ITEST1")
	          << QStringLiteral("--history-path") << archive;
	const int code = run(m_publisher, home, arguments, &said);

	QVERIFY2(code == 2, qPrintable(QStringLiteral("exit %1, said: %2").arg(code).arg(said)));

	/*
	 * The round never started: it would have opened the archive first
	 * and printed its banner, and a key refused after fetching would have
	 * spent the quota this ordering exists to save.
	 */
	QVERIFY2(!QFileInfo::exists(archive), "the archive was opened, so a round ran");
	QVERIFY2(!said.contains(QStringLiteral("fetching every band")), qPrintable(said));
	QVERIFY2(!QFileInfo::exists(tree), "a refused run created the tree");
}

void test_publisher::a_key_is_made_once_and_never_overwritten() {
	QTemporaryDir home;
	QVERIFY(home.isValid());
	QString said;

	const QString key = home.filePath(QStringLiteral("signing.key"));
	QCOMPARE(run(m_publisher, home, {QStringLiteral("--generate-signing-key"), key}, &said),
	         0);

	const QRegularExpression public_key(QStringLiteral("public key: ([0-9a-f]{64})\\n"));
	QVERIFY2(public_key.match(said).hasMatch(), qPrintable(said));

	QFile file(key);
	QCOMPARE(file.size(), qint64(32));
	QCOMPARE(file.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
	         QFileDevice::Permissions());
	QVERIFY(file.open(QIODevice::ReadOnly));
	const QByteArray seed = file.readAll();
	file.close();

	QCOMPARE(run(m_publisher, home, {QStringLiteral("--generate-signing-key"), key}, &said),
	         1);
	QVERIFY(file.open(QIODevice::ReadOnly));
	QCOMPARE(file.readAll(), seed);
}

void test_publisher::the_version_names_the_copyright_holder() {
	QTemporaryDir home;
	QVERIFY(home.isValid());
	QString said;
	QString out;

	QCOMPARE(run(m_publisher, home, {QStringLiteral("--version")}, &said, &out), 0);
	QVERIFY2(out.startsWith(QStringLiteral("bbq-predictor-publish ")), qPrintable(said));
	QVERIFY2(out.contains(QStringLiteral("Copyright (C) 2026 Nabeel Sowan")),
	         qPrintable(said));
}

void test_publisher::the_applet_refuses_what_moved_here() {
	if (m_no_applet) {
		QSKIP("a server-only build makes no applet (BBQ_NO_APPLET=1)");
	}

	QTemporaryDir home;
	QVERIFY(home.isValid());
	QString said;

	const QString tree = home.filePath(QStringLiteral("published"));
	const QString key = home.filePath(QStringLiteral("signing.key"));

	const QList<QStringList> lines = {
		{QStringLiteral("--fetch-once"), QStringLiteral("--publish"), tree},
		{QStringLiteral("--fetch-once"), QStringLiteral("--signing-key"), key},
		{QStringLiteral("--generate-signing-key"), key},
	};

	for (const QStringList &line : lines) {
		const int code = run(m_applet, home, line, &said);
		QVERIFY2(code == 2, qPrintable(QStringLiteral("%1: exit %2, said: %3")
		                                       .arg(line.join(QLatin1Char(' ')))
		                                       .arg(code)
		                                       .arg(said)));
		QVERIFY2(said.contains(QStringLiteral("has moved to bbq-predictor-publish(1)")),
		         qPrintable(said));
	}

	QVERIFY2(!QFileInfo::exists(key), "the applet made a key");
	QVERIFY2(!QFileInfo::exists(tree), "the applet published");
	QVERIFY2(QDir(home.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
	         "a refused run wrote something, so it did more than refuse");
}


void test_publisher::demand_is_saved_before_the_round_fetches() {
	QTemporaryDir home;
	QVERIFY(home.isValid());
	QString said;

	const QString key = home.filePath(QStringLiteral("signing.key"));
	QCOMPARE(run(m_publisher, home, {QStringLiteral("--generate-signing-key"), key}, &said),
	         0);

	const QString demand = home.filePath(QStringLiteral("demand"));
	{
		QFile file(demand);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write(QStringLiteral("IDEMAND1 %1\n")
		                   .arg(QDateTime::currentSecsSinceEpoch())
		                   .toLatin1());
	}

	/*
	 * A proxy that takes the connection and never answers, so the round
	 * is still in its first fetch when the archive is read -- what a
	 * crash or a shutdown would interrupt. Without it the run ends in a
	 * second and saving at the end could not be told from saving at the
	 * start; with it, nothing reaches a weather provider.
	 */
	QTcpServer silent;
	QVERIFY(silent.listen(QHostAddress::LocalHost));
	const QString proxy = QStringLiteral("http://127.0.0.1:%1").arg(silent.serverPort());

	QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
	environment.insert(QStringLiteral("XDG_DATA_HOME"), home.path());
	environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home.path());
	environment.insert(QStringLiteral("XDG_CACHE_HOME"), home.path());
	for (const char *name : {"http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY"}) {
		environment.insert(QString::fromLatin1(name), proxy);
	}
	environment.remove(QStringLiteral("no_proxy"));
	environment.remove(QStringLiteral("NO_PROXY"));

	/*
	 * The daemon's side of the lock, held before the run starts: the
	 * publisher must wait for it rather than read and empty the file
	 * under an append. A second is ample -- unlocked, the run has
	 * emptied the file well inside it.
	 */
	const int daemon_fd = ::open(QFile::encodeName(demand).constData(),
	                             O_WRONLY | O_APPEND | O_CLOEXEC);
	QVERIFY(daemon_fd >= 0);
	QVERIFY(::flock(daemon_fd, LOCK_EX) == 0);
	const qint64 asked = QFileInfo(demand).size();

	const QString archive = home.filePath(QStringLiteral("archive.sqlite"));
	QProcess process;
	process.setProcessEnvironment(environment);
	process.start(m_publisher,
	              {QStringLiteral("--dir"), home.filePath(QStringLiteral("published")),
	               QStringLiteral("--signing-key"), key, QStringLiteral("--station"),
	               QStringLiteral("ITEST1"), QStringLiteral("--history-path"), archive,
	               QStringLiteral("--demand"), demand});
	QVERIFY(process.waitForStarted());

	QTest::qWait(1000);
	const bool waited = process.state() == QProcess::Running &&
	                    QFileInfo(demand).size() == asked;
	::close(daemon_fd);
	if (!waited) {
		process.kill();
		process.waitForFinished();
	}
	QVERIFY2(waited, "the publisher read the demand file while its lock was held");

	/* Bounded: fifteen seconds, half the round's own thirty-second fetch. */
	bool saved = false;
	bool running = true;
	for (int i = 0; i < 150 && !saved; ++i) {
		QTest::qWait(100);
		running = process.state() == QProcess::Running;
		if (!running) {
			break;
		}
		{
			QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
			                                            QStringLiteral("probe"));
			db.setDatabaseName(archive);
			if (QFileInfo::exists(archive) && db.open()) {
				QSqlQuery query(db);
				if (query.exec(QStringLiteral(
				            "SELECT value FROM server_state WHERE name = 'demand'")) &&
				    query.next()) {
					const bbq_demand held = bbq_demand_parse(query.value(0).toByteArray());
					saved = held.stations.count(QStringLiteral("IDEMAND1")) == 1;
				}
			}
		}
		QSqlDatabase::removeDatabase(QStringLiteral("probe"));
	}

	process.kill();
	process.waitForFinished();

	QVERIFY2(running, qPrintable(QStringLiteral("the round ended before the archive was "
	                                            "read, so the proxy held nothing: %1")
	                                     .arg(QString::fromLocal8Bit(process.readAll()))));
	QVERIFY2(saved, "the demand was not in the archive while the round was fetching");

	/* And the file it came from was emptied, so the archive was the only copy. */
	QCOMPARE(QFileInfo(demand).size(), qint64(0));
}

QTEST_GUILESS_MAIN(test_publisher)
#include "test_publisher.moc"
