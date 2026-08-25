#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

/*
 * --seed-verification writes invented statistics, and it must never
 * write them into the real archive (project.md sec 12.15).
 *
 * That guard is four lines in main() and it is exactly the kind that
 * stops working without anyone noticing: it produces no output when it
 * is doing its job, and the thing it prevents is silent too -- fabricated
 * bias rows sitting in the store looking like measurements, feeding the
 * corrected band. The APK signature check in this project stopped
 * matching when a tool changed its output format and reported nothing
 * wrong for months; this is the same shape.
 *
 * So it is tested by running the program. Both directions are checked:
 * that it refuses without --history-path, and that it DOES seed with
 * one. Without the second half the first would pass just as loudly if
 * the binary were broken, missing, or refusing everything.
 */
class test_seed : public QObject {
	Q_OBJECT

private slots:
	void initTestCase();
	void seeding_the_real_archive_is_refused();
	void seeding_a_scratch_file_works();

private:
	QString m_binary;

	QProcess *run(QProcess &process, const QTemporaryDir &home,
	              const QStringList &arguments);
};

void test_seed::initTestCase() {
	m_binary = QString::fromLocal8Bit(qgetenv("BBQ_APP_BINARY"));

	/*
	 * Failed, not skipped. `make test` sets this, so its absence means
	 * the suite is being run in a way that cannot check the guard --
	 * and a guard that was not checked must not report as one that was.
	 */
	QVERIFY2(!m_binary.isEmpty(),
	         "BBQ_APP_BINARY is unset; run this through `make test`");
	QVERIFY2(QFile::exists(m_binary),
	         qPrintable(QStringLiteral("no binary at %1").arg(m_binary)));
}

QProcess *test_seed::run(QProcess &process, const QTemporaryDir &home,
                         const QStringList &arguments) {
	/*
	 * Every standard location moved into a temporary directory, so that
	 * a failure of the guard lands somewhere harmless AND somewhere this
	 * test can look. Asserting on the exit code alone would not notice a
	 * program that refused politely and wrote the rows anyway.
	 */
	QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
	environment.insert(QStringLiteral("XDG_DATA_HOME"), home.path());
	environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home.path());
	environment.insert(QStringLiteral("XDG_CACHE_HOME"), home.path());
	environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
	                   QStringLiteral("offscreen"));

	/*
	 * Merged, so that a failure here reports what the program said on
	 * either channel. Reading only stdout left a failing test saying
	 * "said: " and nothing else, which is a test that cannot explain
	 * itself.
	 */
	process.setProcessChannelMode(QProcess::MergedChannels);
	process.setProcessEnvironment(environment);
	process.start(m_binary, arguments);
	process.waitForFinished(60000);
	return &process;
}

/* Every .sqlite anywhere beneath a directory, however deep. */
static QStringList archives_under(const QString &path) {
	QStringList found;
	QDirIterator walk(path, QStringList() << QStringLiteral("*.sqlite"),
	                  QDir::Files, QDirIterator::Subdirectories);

	while (walk.hasNext()) {
		found << walk.next();
	}

	return found;
}

void test_seed::seeding_the_real_archive_is_refused() {
	QTemporaryDir home;
	QVERIFY(home.isValid());

	QProcess process;
	QStringList arguments;
	arguments << QStringLiteral("--seed-verification")
	          << QStringLiteral("0.5") << QStringLiteral("--station")
	          << QStringLiteral("ITEST1");

	run(process, home, arguments);

	QCOMPARE(process.exitStatus(), QProcess::NormalExit);
	QVERIFY2(process.exitCode() != 0, "refusing must not report success");

	const QString said = QString::fromLocal8Bit(process.readAll());
	QVERIFY2(said.contains(QStringLiteral("refusing")),
	         qPrintable(QStringLiteral("said: %1").arg(said)));

	const QStringList written = archives_under(home.path());
	QVERIFY2(written.isEmpty(),
	         qPrintable(QStringLiteral("it wrote %1").arg(written.join(u' '))));
}

void test_seed::seeding_a_scratch_file_works() {
	QTemporaryDir home;
	QVERIFY(home.isValid());

	const QString scratch = home.filePath(QStringLiteral("scratch.sqlite"));

	QProcess process;
	QStringList arguments;
	arguments << QStringLiteral("--seed-verification")
	          << QStringLiteral("0.5") << QStringLiteral("--history-path")
	          << scratch << QStringLiteral("--station")
	          << QStringLiteral("ITEST1");

	run(process, home, arguments);

	const QString told = QString::fromLocal8Bit(process.readAll());
	QCOMPARE(process.exitStatus(), QProcess::NormalExit);
	QVERIFY2(process.exitCode() == 0, qPrintable(told));
	QVERIFY2(QFile::exists(scratch), "the scratch archive was not written");
	QVERIFY(QFileInfo(scratch).size() > 0);
}

QTEST_GUILESS_MAIN(test_seed)
#include "test_seed.moc"
