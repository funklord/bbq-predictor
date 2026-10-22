#ifndef BBQ_PUBLISH_H
#define BBQ_PUBLISH_H

#include <QString>
#include <QTextStream>

class bbq_composite;
class bbq_history;
class bbq_signing_key;

/*
 * Write one station's signed files into the published tree (project.md
 * sec 15.12):
 *
 *   <dir>/<STATION>/YYYY-MM   every month the archive holds
 *   <dir>/<STATION>/index     which months exist, and their digests
 *   <dir>/<STATION>/head      the bands fetched this round
 *   <dir>/stations            every station published here, LAST
 *
 * Each file is written aside and renamed into place, so a reader -- the
 * daemon, or a web server on 443 -- never sees half of one. The manifest
 * goes last because the daemon reloads when it changes: by then every
 * file it lists is whole.
 *
 * A month whose row count and first and last readings match the existing
 * index is left alone, so a decade costs one grouped query per run. A
 * file whose bytes would not change is not rewritten, because encoding
 * and signing are deterministic and an unchanged file is what lets a
 * client be told "unchanged".
 *
 * `composite` may be null to republish the months alone. Returns 0, or
 * 1 having said why on `error`.
 */
int bbq_publish(const QString &dir, const QString &station,
                const bbq_composite *composite, const bbq_history &history,
                const bbq_signing_key &key, qint64 now_utc, QTextStream &out,
                QTextStream &error);

#endif
