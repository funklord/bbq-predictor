#include "model/settings.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace {

/*
 * One QSettings, built the same way every time.
 *
 * The format is named explicitly rather than left to the platform
 * default: sec 2.6.6 settled on INI, and on some platforms the default
 * is a registry or a plist -- which would still work, and would mean
 * "the config file" was not a file anybody could open.
 */
QSettings open() {
	const QStandardPaths::StandardLocation where =
		QStandardPaths::AppConfigLocation;
	const QString directory = QStandardPaths::writableLocation(where);
	QDir().mkpath(directory);

	const QString path = directory + QStringLiteral("/bbq-predictor.ini");
	return QSettings(path, QSettings::IniFormat);
}

const char *const key_station = "station";
const char *const key_derived = "derived_geocode";
const char *const key_override = "geocode_override";
const char *const key_interpolation = "graph/interpolation";
const char *const key_smoothing = "graph/rounding_seconds";

} // namespace

QString bbq_settings::station() {
	return open().value(QString::fromLatin1(key_station)).toString().trimmed();
}

void bbq_settings::set_station(const QString &station_id) {
	QSettings settings = open();

	/*
	 * Changing the station invalidates the coordinate derived from the
	 * old one. Leaving it would point the forecast bands at the
	 * previous station's garden while the observed band read the new
	 * one -- two places on one axis, which is exactly what sec 2.6.7
	 * exists to prevent.
	 */
	const QString had = settings.value(QString::fromLatin1(key_station)).toString();
	if (had != station_id) {
		settings.remove(QString::fromLatin1(key_derived));
	}

	settings.setValue(QString::fromLatin1(key_station), station_id.trimmed());
}

QString bbq_settings::derived_geocode() {
	return open().value(QString::fromLatin1(key_derived)).toString();
}

void bbq_settings::set_derived_geocode(double latitude, double longitude) {
	QString value = QString::number(latitude, 'f', 4);
	value += QStringLiteral(",");
	value += QString::number(longitude, 'f', 4);

	open().setValue(QString::fromLatin1(key_derived), value);
}

QString bbq_settings::geocode_override() {
	return open().value(QString::fromLatin1(key_override)).toString().trimmed();
}

void bbq_settings::set_geocode_override(const QString &geocode) {
	open().setValue(QString::fromLatin1(key_override), geocode.trimmed());
}

int bbq_settings::interpolation(int fallback) {
	return open().value(QString::fromLatin1(key_interpolation), fallback).toInt();
}

void bbq_settings::set_interpolation(int method) {
	open().setValue(QString::fromLatin1(key_interpolation), method);
}

int bbq_settings::smoothing(int fallback) {
	return open().value(QString::fromLatin1(key_smoothing), fallback).toInt();
}

void bbq_settings::set_smoothing(int seconds) {
	open().setValue(QString::fromLatin1(key_smoothing), seconds);
}

QString bbq_settings::location() {
	return open().fileName();
}
