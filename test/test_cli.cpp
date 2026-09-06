#include <QtTest>

#include "cli/options.h"

/*
 * The command line, which until now was the largest piece of this
 * program no test linked at all (sec 16.58).
 */
class test_cli : public QObject {
	Q_OBJECT

private slots:
	void a_value_is_the_argument_after_its_option();
	void an_option_followed_by_another_option_has_no_value();
	void a_negative_number_is_a_value_and_not_an_option();
};

void test_cli::a_value_is_the_argument_after_its_option() {
	const QStringList arguments = {QStringLiteral("bbq-predictor"),
	                               QStringLiteral("--station"),
	                               QStringLiteral("ISTOCK877"),
	                               QStringLiteral("--shot"),
	                               QStringLiteral("out.png")};

	QCOMPARE(bbq_option_value(arguments, QStringLiteral("--station")),
	         QStringLiteral("ISTOCK877"));
	QCOMPARE(bbq_option_value(arguments, QStringLiteral("--shot")),
	         QStringLiteral("out.png"));

	/* Absent, and present-but-last, are both empty. */
	QVERIFY(bbq_option_value(arguments, QStringLiteral("--view")).isEmpty());
	QVERIFY(bbq_option_value({QStringLiteral("--shot")},
	                         QStringLiteral("--shot"))
	                .isEmpty());
}

/*
 * The defect this was extracted for.
 *
 * `--station --geocode 59.3,18.0` set the station to the literal
 * "--geocode" and the program reported that the station was unknown --
 * true of the string it was given, and nothing to do with the reader's
 * actual mistake.
 */
void test_cli::an_option_followed_by_another_option_has_no_value() {
	const QStringList arguments = {QStringLiteral("bbq-predictor"),
	                               QStringLiteral("--station"),
	                               QStringLiteral("--geocode"),
	                               QStringLiteral("59.3,18.0")};

	const QString station = bbq_option_value(arguments,
	                                         QStringLiteral("--station"));

	QVERIFY2(station.isEmpty(),
	         qPrintable(QStringLiteral("--station took \"%1\" as its value")
	                            .arg(station)));

	/* And the option that followed is unharmed. */
	QCOMPARE(bbq_option_value(arguments, QStringLiteral("--geocode")),
	         QStringLiteral("59.3,18.0"));
}

/*
 * A LEADING DOUBLE DASH, not a leading dash, and this is the case that
 * decides it: the southern hemisphere is a legal geocode and rejecting
 * it would break a case the program exists to serve.
 */
void test_cli::a_negative_number_is_a_value_and_not_an_option() {
	const QStringList arguments = {QStringLiteral("bbq-predictor"),
	                               QStringLiteral("--geocode"),
	                               QStringLiteral("-33.9,18.4"),
	                               QStringLiteral("--cursor"),
	                               QStringLiteral("-1")};

	QCOMPARE(bbq_option_value(arguments, QStringLiteral("--geocode")),
	         QStringLiteral("-33.9,18.4"));
	QCOMPARE(bbq_option_value(arguments, QStringLiteral("--cursor")),
	         QStringLiteral("-1"));
}

QTEST_MAIN(test_cli)
#include "test_cli.moc"
