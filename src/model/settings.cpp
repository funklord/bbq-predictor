#include "model/settings.h"

#include "server/transport.h"

#include <QDir>
#include <QSettings>
#include <QStringList>
#include <QVariant>
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
const char *const key_layout = "layout";
const char *const key_theme = "theme";

/* Where published files are fetched from (project.md sec 16.38.3). */
const char *const key_server_hosts = "server_hosts";
const char *const key_steadiness = "graph/scale_steadiness";

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

QByteArray bbq_settings::server_public_key() {
	const QByteArray hex = open().value(QStringLiteral("server/public_key")).toString()
	                               .trimmed().toLatin1();
	if (!hex.isEmpty()) {
		const QByteArray key = QByteArray::fromHex(hex);
		return key.size() == 32 ? key : QByteArray();
	}
	return bbq_server_built_in_key();
}

namespace {

std::optional<int> chosen(const char *key) {
	const QVariant stored = open().value(QString::fromLatin1(key));
	bool number = false;
	const int value = stored.toInt(&number);
	if (!stored.isValid() || !number) {
		return std::nullopt;
	}
	return value;
}

/* Following the default removes the key; it never writes today's value. */
void choose(const char *key, std::optional<int> value) {
	if (!value) {
		open().remove(QString::fromLatin1(key));
		return;
	}
	open().setValue(QString::fromLatin1(key), *value);
}

} // namespace

std::optional<int> bbq_settings::interpolation() {
	return chosen(key_interpolation);
}

void bbq_settings::set_interpolation(std::optional<int> method) {
	choose(key_interpolation, method);
}

std::optional<int> bbq_settings::smoothing() {
	return chosen(key_smoothing);
}

void bbq_settings::set_smoothing(std::optional<int> seconds) {
	choose(key_smoothing, seconds);
}

std::optional<int> bbq_settings::scale_steadiness() {
	return chosen(key_steadiness);
}

void bbq_settings::set_scale_steadiness(std::optional<int> percent) {
	choose(key_steadiness, percent);
}

QStringList bbq_settings::server_hosts() {
	const QVariant stored =
	        open().value(QString::fromLatin1(key_server_hosts));

	QStringList hosts;
	for (const QString &host : stored.toStringList()) {
		const QString trimmed = host.trimmed();
		if (!trimmed.isEmpty()) {
			hosts.append(trimmed);
		}
	}

	if (!hosts.isEmpty()) {
		return hosts;
	}

	/* Local first, then vibes.se (sec 16.38.3). One list, in transport. */
	return bbq_server_default_hosts();
}

void bbq_settings::set_server_hosts(const QStringList &hosts) {
	QStringList kept;
	for (const QString &host : hosts) {
		const QString trimmed = host.trimmed();
		if (!trimmed.isEmpty()) {
			kept.append(trimmed);
		}
	}

	/*
	 * An empty list removes the key rather than writing an empty one.
	 *
	 * COSMETIC, and the comment here first claimed otherwise -- that it
	 * was what stopped a caller ending up with no candidates. It is
	 * not: server_hosts() returns the default whenever what it reads is
	 * empty, so deleting this branch changes no behaviour, which is
	 * exactly what happened when it was deleted to see the test fail
	 * and the test passed.
	 *
	 * Kept because a dead key in a file people are meant to be able to
	 * open is worth not writing. The GUARANTEE lives in the reader, and
	 * belongs in one place.
	 */
	if (kept.isEmpty()) {
		open().remove(QString::fromLatin1(key_server_hosts));
		return;
	}

	open().setValue(QString::fromLatin1(key_server_hosts), kept);
}

QString bbq_settings::theme() {
	return open().value(QString::fromLatin1(key_theme),
	                    QStringLiteral("auto")).toString().trimmed();
}

void bbq_settings::set_theme(const QString &preference) {
	open().setValue(QString::fromLatin1(key_theme), preference.trimmed());
}

QString bbq_settings::layout() {
	return open().value(QString::fromLatin1(key_layout),
	                    QStringLiteral("auto")).toString().trimmed();
}

void bbq_settings::set_layout(const QString &preference) {
	open().setValue(QString::fromLatin1(key_layout), preference.trimmed());
}

QString bbq_settings::location() {
	return open().fileName();
}
