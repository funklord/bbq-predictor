#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
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
	void every_scored_band_reaches_the_report();
	void discovery_refuses_an_archive_it_cannot_open();
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


/*
 * Whatever the seeding diagnostic writes, the report reads back
 * (project.md sec 16.96).
 *
 * There were two lists of "the bands that carry a score" -- one the
 * report walked and one seeding wrote -- in separate arrays kept in step
 * by somebody noticing. A band in one and not the other is invisible:
 * seeding writes a score the report never prints, or the report looks
 * for one seeding never wrote, and both read as "nothing has been
 * checked yet".
 *
 * They are one array now, so this cannot drift by construction. The test
 * is here anyway because the array is not the claim -- the claim is that
 * a score written for a band comes back out of the diagnostic, which
 * also covers bbq_band_name losing a case and the report filtering rows
 * it should print.
 *
 * corrected is the one that was missing from both, and it is the band
 * whose whole purpose is to be compared against what it corrects.
 */
void test_seed::every_scored_band_reaches_the_report() {
	QTemporaryDir home;
	QVERIFY(home.isValid());

	const QString scratch = home.filePath(QStringLiteral("report.sqlite"));

	QProcess seeding;
	QStringList seed;
	seed << QStringLiteral("--seed-verification") << QStringLiteral("0.5")
	     << QStringLiteral("--history-path") << scratch
	     << QStringLiteral("--station") << QStringLiteral("ITEST1");
	run(seeding, home, seed);
	QVERIFY2(seeding.exitCode() == 0,
	         qPrintable(QString::fromLocal8Bit(seeding.readAll())));

	QProcess reading;
	QStringList history;
	history << QStringLiteral("--history") << QStringLiteral("--history-path")
	        << scratch << QStringLiteral("--station")
	        << QStringLiteral("ITEST1");
	run(reading, home, history);

	const QString told = QString::fromLocal8Bit(reading.readAll());
	QVERIFY2(reading.exitCode() == 0, qPrintable(told));

	const QStringList expected = {
		QStringLiteral("radar"),
		QStringLiteral("nowcast"),
		QStringLiteral("hourly"),
		QStringLiteral("extended"),
		QStringLiteral("corrected"),
	};

	for (const QString &band : expected) {
		if (!told.contains(band + QStringLiteral(" at "))) {
			QFAIL(qPrintable(QStringLiteral(
			        "the report never named the %1 band, though seeding "
			        "wrote scores for it:\n%2").arg(band, told)));
		}
	}

	/*
	 * AND EVERY QUANTITY, for the same reason and by the same fault
	 * (sec 16.103). `grill` was added to the store by sec 12.20 and
	 * reached neither the seeding nor the report -- so the archive
	 * scored the verdict, which is the thing this program exists to
	 * answer, and the diagnostic showed only its three ingredients.
	 */
	const QStringList scored = {
		QStringLiteral("grill"),
		QStringLiteral("temperature"),
		QStringLiteral("precip_rate"),
		QStringLiteral("wind_kph"),
	};

	for (const QString &quantity : scored) {
		if (!told.contains(quantity + QStringLiteral(" error, by band"))) {
			QFAIL(qPrintable(QStringLiteral(
			        "the report has no section for %1, though seeding wrote "
			        "scores for it:\n%2").arg(quantity, told)));
		}
	}

	/*
	 * The control. Every name above is a word that could appear in the
	 * report's prose, so a test that only looked for them would pass
	 * against a report that printed no rows at all. A band NOT scored
	 * must be absent, which no amount of surrounding text supplies.
	 */
	QVERIFY2(!told.contains(QStringLiteral("observed at ")),
	         "a measurement was reported as though it were a forecast");

	/* And the same for a quantity nothing scores. */
	QVERIFY2(!told.contains(QStringLiteral("humidity error")),
	         "a quantity this program does not score was reported");

	/*
	 * AND THE LEAD BUCKETS, WHICH ARE THE THIRD DIMENSION OF THE SAME
	 * LOOP (sec 16.104).
	 *
	 * Asserted as the whole SET rather than as a presence check, so it
	 * fails in both the directions it can see: a bucket dropped from
	 * the shared list is reported missing, and one added to the list
	 * without being added here is reported unexpected.
	 *
	 * WHAT IT DOES NOT CATCH, tested rather than assumed: a bucket
	 * added to the enum and to bbq_lead_bucket_name but NOT to the
	 * shared list. The report then never prints it, this set never
	 * expects it, and the two agree about its absence. Grown the enum by
	 * one and left the list alone, and this test passed.
	 *
	 * That gap is the one worth knowing, because it is the direction a
	 * new lead time would actually go wrong in. What stands against it
	 * is the compiler: bbq_lead_bucket_name switches without a default,
	 * so a new value warns there and whoever silences the warning is
	 * one grep from this list.
	 *
	 * The names are bbq_lead_bucket_name's, which this binary cannot
	 * call -- it links no part of the program, only runs it -- so they
	 * are written out, and this comment is the reason a new one has to
	 * be added here too.
	 */
	QSet<QString> seen;
	static const QRegularExpression at(QStringLiteral(" at ([0-9a-z+]+):"));
	QRegularExpressionMatchIterator found = at.globalMatch(told);
	while (found.hasNext()) {
		seen.insert(found.next().captured(1));
	}

	const QSet<QString> want = {
		QStringLiteral("1h"),  QStringLiteral("3h"),
		QStringLiteral("6h"),  QStringLiteral("12h"),
		QStringLiteral("1d"),  QStringLiteral("2d"),
		QStringLiteral("4d"),  QStringLiteral("7d"),
		QStringLiteral("7d+"),
	};

	if (seen != want) {
		const QStringList missing = QStringList(
		        QList<QString>((want - seen).begin(), (want - seen).end()));
		const QStringList extra = QStringList(
		        QList<QString>((seen - want).begin(), (seen - want).end()));

		QFAIL(qPrintable(
		        QStringLiteral("the report's lead buckets do not match the "
		                       "expected set -- missing [%1], unexpected [%2]")
		                .arg(missing.join(QStringLiteral(", ")),
		                     extra.join(QStringLiteral(", ")))));
	}
}

QTEST_GUILESS_MAIN(test_seed)
#include "test_seed.moc"

/*
 * Discovery exists to REMEMBER what it finds, so an archive that will
 * not open is a failure rather than a footnote (project.md sec 16.102).
 *
 * It used to discard the result of opening the store, look the stations
 * up, throw them away and report "remembered 0 station(s)" with an exit
 * of 0 -- the same sentence a genuinely empty answer gives, and the
 * opposite of what happened.
 *
 * No network is needed to check this and that is by construction: the
 * archive is opened before anything is looked up, so the refusal
 * happens first. A test that needed the lookup could not live in this
 * suite at all.
 */
void test_seed::discovery_refuses_an_archive_it_cannot_open() {
	QTemporaryDir home;
	QVERIFY(home.isValid());

	QProcess process;
	QStringList arguments;
	arguments << QStringLiteral("--discover") << QStringLiteral("--geocode")
	          << QStringLiteral("59.33,18.07")
	          << QStringLiteral("--history-path")
	          << QStringLiteral("/proc/nope/x.sqlite");

	run(process, home, arguments);

	const QString told = QString::fromLocal8Bit(process.readAll());

	QCOMPARE(process.exitStatus(), QProcess::NormalExit);
	QVERIFY2(process.exitCode() != 0,
	         qPrintable(QStringLiteral("an unusable archive exited 0: %1")
	                            .arg(told)));

	QVERIFY2(told.contains(QStringLiteral("history unavailable")),
	         qPrintable(QStringLiteral("the archive was not blamed: %1")
	                            .arg(told)));

	/*
	 * The control, and the reason this test is worth having: the old
	 * behaviour must not be able to pass it. "remembered 0 station(s)"
	 * is what a working program says when the sky is empty, so a test
	 * that only looked for an error string would still pass against a
	 * program that printed both.
	 *
	 * Matched on the count's own phrasing rather than on the word
	 * "remembered", which the refusal itself uses -- the first version
	 * forbade the word and failed against the fix, which is the control
	 * being stricter than the claim.
	 */
	QVERIFY2(!told.contains(QStringLiteral("station(s) near")),
	         qPrintable(QStringLiteral("it still reported a count of stations "
	                                   "it had remembered: %1").arg(told)));
}

