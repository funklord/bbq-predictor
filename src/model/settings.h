#ifndef BBQ_SETTINGS_H
#define BBQ_SETTINGS_H

#include <QString>
#include <QStringList>

#include <optional>

/*
 * What the applet remembers between runs (project.md sec 2.6.6).
 *
 * QSettings in INI under QStandardPaths::AppConfigLocation, which sec
 * 2.6.6 settled as this project's first configuration of any kind and
 * which nothing had implemented until now -- the station arrived only
 * on the command line, so a tray applet nobody typed arguments at had
 * no station at all.
 *
 * The keys are gathered here rather than spelled out at each call site,
 * because a key is a name that escapes into a file somebody else can
 * edit, and a typo in one produces a setting that silently never loads.
 */
class bbq_settings {
public:
	/*
	 * The WATCHED station (sec 2.6.5). User-chosen, never derived, and
	 * never replaced by a nearest-station lookup.
	 *
	 * Not the same idea as bbq_station::pinned, which says a station
	 * gets the sparing backfill and nothing else. This comment said
	 * "the pinned station" until sec 13 gave that word to the other
	 * concept, and the stale wording then cost a real misdiagnosis
	 * (sec 15.7): a session read an unchecked Pin box beside a watched
	 * station as the pin having failed to persist, when the flag was
	 * intact in the store and the two were simply different things.
	 */
	static QString station();
	static void set_station(const QString &station_id);

	/*
	 * The geocode DERIVED from that station, cached (sec 2.6.7.2).
	 *
	 * A cache of a derivation rather than a second source of truth: it
	 * exists so a cold start does not have to serialise the forecast
	 * bands behind the observed one, and so an offline station leaves
	 * the forecast bands still placeable.
	 */
	static QString derived_geocode();
	static void set_derived_geocode(double latitude, double longitude);

	/*
	 * The explicit override (sec 2.6.7). Wins when set, and its being
	 * set is the only way the station and the forecast point are
	 * allowed to disagree.
	 */
	static QString geocode_override();
	static void set_geocode_override(const QString &geocode);

	/*
	 * The key vibes.se's files are checked against (project.md sec
	 * 15.12): `server/public_key` in hex when set, for somebody running
	 * their own publisher, and the built-in key otherwise. Read-only from
	 * the program: changing whose files are trusted is an edit somebody
	 * makes on purpose, not a control.
	 */
	static QByteArray server_public_key();

	/*
	 * Where published files are fetched from, nearest first (project.md
	 * sec 16.38.3), as `server_hosts` in the settings file: "host" or
	 * "host:port" for a daemon (7227 when no port is given), or
	 * "https://host/path/" for a static tree. Unset, it is this
	 * machine's daemon and then vibes.se's routes -- local first, since
	 * a machine running the packaged daemon already has the archive,
	 * and asking a remote for it tells somebody else which stations are
	 * watched.
	 *
	 * Together with `server/public_key` above, this is what lets
	 * somebody running their own publisher point the applet at it. Like
	 * the key, it is an edit made on purpose rather than a control.
	 */
	static QStringList server_hosts();
	static void set_server_hosts(const QStringList &hosts);

	/*
	 * Presentation, remembered because it is tuned by looking.
	 *
	 * Each is a CHOICE or nothing (sec 3.11.7): nullopt means the person
	 * follows the built-in default, which is stored as the key's absence
	 * and read back as nullopt, so a later build's better default reaches
	 * them. A value is a pin, kept whatever the default becomes -- even
	 * one equal to today's default, because choosing it was deliberate.
	 */
	static std::optional<int> interpolation();
	static void set_interpolation(std::optional<int> method);

	static std::optional<int> smoothing();
	static void set_smoothing(std::optional<int> seconds);

	static std::optional<int> scale_steadiness();
	static void set_scale_steadiness(std::optional<int> percent);

	static QString theme();
	static void set_theme(const QString &preference);

	/*
	 * "auto", "desktop" or "mobile" (sec 10.1). Auto means the device
	 * decides, which is right until somebody disagrees with it -- a
	 * tablet in a keyboard case, a phone on a monitor.
	 */
	static QString layout();
	static void set_layout(const QString &preference);

	/* Where the file actually is, for saying so in the interface. */
	static QString location();
};

#endif
