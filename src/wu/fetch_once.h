#ifndef BBQ_WU_FETCH_ONCE_H
#define BBQ_WU_FETCH_ONCE_H

#include <QString>

/*
 * Fetch every band once, report what arrived, and exit.
 *
 * A diagnostic, not a feature. It exists because "get the data from WU"
 * has to be verifiable before anything is designed on top of it: this
 * runs the real key extraction against the real endpoints and says what
 * came back, so the shape of the data is observed rather than assumed
 * (which is the same standard sec 2.6 was held to).
 *
 * Terminates on its own. Every request settles or the whole run is cut
 * off by a wall-clock timeout, so this cannot sit waiting forever on a
 * connection that never answers.
 *
 * An empty station skips the observed band, which is the normal state
 * rather than an error (sec 2.6.6). Passing a station with no geocode
 * derives one from the station's own coordinates (sec 2.6.7).
 */
/*
 * Exits 0 when every band answered, 3 when some band failed but the
 * composite still covers now, 1 when the run stored nothing usable, and
 * 2 for a configuration error. 3 exists because a station going quiet is
 * ordinary and the other bands still land in the archive -- the systemd
 * timer forgives it by name (sec 15.6), which it could not do if a quiet
 * station and a total outage returned the same thing.
 *
 * `history_path` is the store to archive into. Empty takes the real one,
 * which is deliberate: this fetches genuine measurements, and throwing
 * away real observations because they arrived through a diagnostic
 * would be the wrong way round (project.md sec 12.11).
 */
int bbq_wu_fetch_once(const QString &station_id, const QString &geocode,
                      int timeout_s, const QString &history_path = QString());

#endif
