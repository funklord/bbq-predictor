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
	void the_version_names_the_copyright_holder();
	void the_usage_points_at_the_manual_page();

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

void test_seed::the_version_names_the_copyright_holder() {
	/*
	 * ATTRIBUTION IS A REQUIREMENT, AND NOTHING CHECKED IT
	 * (project.md sec 16.18).
	 *
	 * harmonization.md asks every private project to name the copyright
	 * holder in three places, one of which is --version. It is a
	 * statement of fact about who wrote this, and the way it goes is not
	 * somebody deleting it on purpose: it is a version string being
	 * reworked and the second line going with it, silently, in a commit
	 * about something else.
	 *
	 * Driven as a subprocess for the same reason the seed guard is:
	 * what is being checked is what the PROGRAM prints, and a unit test
	 * of a constant would pass while main.cpp printed something else.
	 *
	 * The name and address are asserted, not the year, which is that
	 * project's own and moves.
	 */
	QTemporaryDir home;
	QVERIFY(home.isValid());

	QProcess process;
	/* run() waits; waiting again would ask a finished process to
	 * finish, which answers false. */
	run(process, home, {QStringLiteral("--version")});
	QCOMPARE(process.exitStatus(), QProcess::NormalExit);

	const QString said = QString::fromLocal8Bit(process.readAll());

	QCOMPARE(process.exitCode(), 0);
	QVERIFY2(said.contains(QStringLiteral("Nabeel Sowan")),
	         qPrintable(QStringLiteral("--version does not name the "
	                                   "copyright holder: %1").arg(said)));
	QVERIFY2(said.contains(QStringLiteral("nabeel@vibes.se")),
	         qPrintable(QStringLiteral("--version has no address for the "
	                                   "holder: %1").arg(said)));

	/*
	 * And the first line keeps its shape, because a version string with
	 * a stable format is an interface: apt-emerge's own note says the
	 * first line is what scripts parse, so the attribution goes on a
	 * line of its own rather than being appended to it.
	 */
	const QStringList lines = said.split(QLatin1Char('\n'));
	QVERIFY2(lines.value(0).startsWith(QStringLiteral("bbq-predictor ")),
	         qPrintable(QStringLiteral("the first line is not the parsable "
	                                   "version: %1").arg(lines.value(0))));
	QVERIFY2(!lines.value(0).contains(QStringLiteral("Copyright")),
	         qPrintable(QStringLiteral("the attribution has been appended to "
	                                   "the line scripts parse: %1")
	                            .arg(lines.value(0))));
}

void test_seed::the_usage_points_at_the_manual_page() {
	/*
	 * --help documents eight options and the program accepts
	 * twenty-one, which is fine only while it says where the rest are
	 * (sec 15.10). tool/man_options.py keeps the PAGE honest against the
	 * program; nothing kept the usage honest about the page's
	 * existence.
	 */
	QTemporaryDir home;
	QVERIFY(home.isValid());

	QProcess process;
	/* run() waits; waiting again would ask a finished process to
	 * finish, which answers false. */
	run(process, home, {QStringLiteral("--help")});
	QCOMPARE(process.exitStatus(), QProcess::NormalExit);

	const QString said = QString::fromLocal8Bit(process.readAll());

	QCOMPARE(process.exitCode(), 0);
	QVERIFY2(said.contains(QStringLiteral("bbq-predictor(1)")),
	         qPrintable(QStringLiteral("--help does not point at the manual "
	                                   "page it is a summary of: %1")
	                            .arg(said)));
}

QTEST_GUILESS_MAIN(test_seed)
#include "test_seed.moc"
