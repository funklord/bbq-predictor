#ifndef BBQ_CLI_OPTIONS_H
#define BBQ_CLI_OPTIONS_H

#include <QString>
#include <QStringList>

/*
 * The value after `name`, or empty when it is absent, last, or followed
 * by another option.
 *
 * The last of those three is the reason this is not four lines inside
 * main.cpp any more. It took the next argument unconditionally, so
 * `--station --geocode 59.3,18.0` set the station to the literal string
 * "--geocode" and the program then reported that the station was
 * unknown -- a message naming a real fault that was not the one the
 * reader had, which is the failure sec 14.1 records about somebody
 * else's API and this had at home.
 *
 * A LEADING DOUBLE DASH, not a leading dash. Every option here is
 * spelled with two, and a value may legitimately begin with one:
 * `--geocode -59.3,18.0` is the southern hemisphere, and rejecting it
 * would break a case the program is meant to serve.
 *
 * It cannot tell "option given without a value" from "option absent",
 * and does not try: the callers already handle absence by falling back
 * to the stored setting, so the value of this is that a typo stops
 * being read as data. Saying which of the two happened would want a
 * different return type and eighteen call sites agreeing about it.
 */
QString bbq_option_value(const QStringList &arguments, const QString &name);

#endif
