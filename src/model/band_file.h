#ifndef BBQ_BAND_FILE_H
#define BBQ_BAND_FILE_H

#include <QString>

class bbq_composite;

/*
 * Every band of a composite, as text, and back (project.md sec 3.18.6).
 *
 * A fetch is a moment that cannot be had again: the radar moves on in
 * five minutes and the forecasts within the hour. Saved, the same data
 * can be swept by one build and then another, which is how a change is
 * shown to have moved a flicker rather than the weather moving under
 * it. `about_utc` is the moment the bands were fetched for.
 *
 * Plain text, one sample a line, so a file can be read and edited by
 * hand into the fixture a test wants.
 */
bool bbq_write_band_file(const QString &path, const bbq_composite &composite,
                         qint64 about_utc, QString *why);

bool bbq_read_band_file(const QString &path, bbq_composite *composite,
                        qint64 *about_utc, QString *why);

#endif
