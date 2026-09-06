#ifndef BBQ_SETTINGS_H
#define BBQ_SETTINGS_H

#include <QString>
#include <QStringList>

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
	 * ============================================================
	 * NOT WIRED TO ANYTHING. THERE IS NO SERVER AND NO PROTOCOL.
	 * ============================================================
	 *
	 * Where an archive server would be, nearest first (project.md
	 * sec 16.38). A placeholder, added on the copyright holder's
	 * instruction so the shape is visible, and deliberately marked
	 * rather than left to look finished:
	 *
	 *   - nothing in this tree opens a socket. `grep` for QTcpServer,
	 *     QHttpServer, listen( or bind( across src/ finds nothing.
	 *   - there is no wire format. sec 15.5's first open question is
	 *     that the internal series has no wire form at all, and
	 *     build-and-commit.md requires `situ` to be evaluated before
	 *     any byte-exact one is hand-rolled.
	 *   - so these names are an address for a port nobody serves,
	 *     speaking a protocol nobody has written down.
	 *
	 * LOCAL FIRST, and that ordering is the part that is a decision
	 * rather than a placeholder. A machine running the packaged timer
	 * already has the archive on disk; asking somebody else's host for
	 * what is under your own hand is slower, needs a network, and tells
	 * a third party which stations you watch. The remote is the
	 * fallback, never the first choice.
	 *
	 * The default remote is one person's machine. That is a hosting and
	 * a privacy commitment rather than a convenience, and it is the
	 * holder's to make; it is recorded here so that whoever wires this
	 * up meets the sentence before the address.
	 */
	static QStringList server_hosts();
	static void set_server_hosts(const QStringList &hosts);

	/* Presentation, remembered because it is tuned by looking. */
	static int interpolation(int fallback);
	static void set_interpolation(int method);

	static int smoothing(int fallback);
	static void set_smoothing(int seconds);

	/*
	 * "auto", "desktop" or "mobile" (sec 10.1). Auto means the device
	 * decides, which is right until somebody disagrees with it -- a
	 * tablet in a keyboard case, a phone on a monitor.
	 */
	static int scale_steadiness(int fallback);
	static void set_scale_steadiness(int percent);

	static QString theme();
	static void set_theme(const QString &preference);

	static QString layout();
	static void set_layout(const QString &preference);

	/* Where the file actually is, for saying so in the interface. */
	static QString location();
};

#endif
