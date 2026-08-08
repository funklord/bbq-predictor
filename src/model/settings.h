#ifndef BBQ_SETTINGS_H
#define BBQ_SETTINGS_H

#include <QString>

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
	 * The pinned station (sec 2.6.5). User-chosen, never derived, and
	 * never replaced by a nearest-station lookup.
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

	/* Presentation, remembered because it is tuned by looking. */
	static int interpolation(int fallback);
	static void set_interpolation(int method);

	static int smoothing(int fallback);
	static void set_smoothing(int seconds);

	/* Where the file actually is, for saying so in the interface. */
	static QString location();
};

#endif
