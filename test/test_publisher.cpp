#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

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

private:
	QString m_publisher;
	QString m_applet;

	int run(const QString &binary, const QTemporaryDir &home,
	        const QStringList &arguments, QString *said);
};

void test_publisher::initTestCase() {
	m_publisher = QString::fromLocal8Bit(qgetenv("BBQ_PUBLISH_BINARY"));
	m_applet = QString::fromLocal8Bit(qgetenv("BBQ_APP_BINARY"));

	/* Failed, not skipped, for the reason test_seed gives. */
	QVERIFY2(!m_publisher.isEmpty() && !m_applet.isEmpty(),
	         "BBQ_PUBLISH_BINARY or BBQ_APP_BINARY is unset; run this "
	         "through `make test`");
	QVERIFY2(QFile::exists(m_publisher),
	         qPrintable(QStringLiteral("no binary at %1").arg(m_publisher)));
	QVERIFY2(QFile::exists(m_applet),
	         qPrintable(QStringLiteral("no binary at %1").arg(m_applet)));
}

int test_publisher::run(const QString &binary, const QTemporaryDir &home,
                        const QStringList &arguments, QString *said) {
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

	QProcess process;
	process.setProcessChannelMode(QProcess::MergedChannels);
	process.setProcessEnvironment(environment);
	process.start(binary, arguments);
	if (!process.waitForFinished(60000)) {
		process.kill();
		process.waitForFinished();
		*said = QStringLiteral("(did not finish)");
		return -1;
	}

	*said = QString::fromLocal8Bit(process.readAll());
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

	QCOMPARE(run(m_publisher, home, {QStringLiteral("--version")}, &said), 0);
	QVERIFY2(said.startsWith(QStringLiteral("bbq-predictor-publish ")), qPrintable(said));
	QVERIFY2(said.contains(QStringLiteral("Copyright (C) 2026 Nabeel Sowan")),
	         qPrintable(said));
}

void test_publisher::the_applet_refuses_what_moved_here() {
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

QTEST_GUILESS_MAIN(test_publisher)
#include "test_publisher.moc"
