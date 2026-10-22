#ifndef BBQ_WIRE_READ_H
#define BBQ_WIRE_READ_H

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

#include "model/series.h"
#include "wire/format.h"

/*
 * The applet's reader for the published format (project.md sec 15.12).
 *
 * Every byte here came from the network, so the order is the defence:
 *
 *   1. a size cap, before anything is looked at;
 *   2. the Ed25519 signature against the key the applet is built with,
 *      before anything is PARSED -- so only the publisher's files reach
 *      the parser at all, and a hostile one needs the key first;
 *   3. situ's generated validate() and check(), from src/wire/bbq.situ,
 *      using only the calls the vendored README says are safe;
 *   4. every rule situ cannot state, checked here: the station's
 *      character class and identity, ascending samples inside their
 *      month, provider runs that sum, counts that match their bitmaps,
 *      zero padding, and no trailing bytes.
 *
 * Anything failing any step is refused whole, never applied in part.
 * Each function returns false and says why; `why` is for a log line,
 * never for a user.
 */

/* Steps 1 and 2: the body of a signed file of `kind`. */
bool bbq_wire_open(const QByteArray &file, bbq_wire_kind kind,
                   const QByteArray &public_key, QByteArray *body, QString *why);

struct bbq_wire_head {
	qint64 generated_utc = 0;
	std::vector<bbq_series> bands;
};

bool bbq_wire_read_stations(const QByteArray &file, const QByteArray &public_key,
                            QStringList *stations, QString *why);

bool bbq_wire_read_index(const QByteArray &file, const QByteArray &public_key,
                         const QString &station,
                         std::vector<bbq_wire_index_entry> *entries, QString *why);

bool bbq_wire_read_month(const QByteArray &file, const QByteArray &public_key,
                         const QString &station, uint16_t month,
                         bbq_wire_month_rows *rows, QString *why);

bool bbq_wire_read_head(const QByteArray &file, const QByteArray &public_key,
                        const QString &station, bbq_wire_head *head, QString *why);

/*
 * Steps 3 and 4 alone, over a body that has already been opened. Public
 * so the fuzzer can reach the parser: through the functions above it
 * would need to forge a signature first, and would test nothing else.
 */
bool bbq_wire_parse_stations(const QByteArray &body, QStringList *stations, QString *why);
bool bbq_wire_parse_index(const QByteArray &body, const QString &station,
                          std::vector<bbq_wire_index_entry> *entries, QString *why);
bool bbq_wire_parse_month(const QByteArray &body, const QString &station, uint16_t month,
                          bbq_wire_month_rows *rows, QString *why);
bool bbq_wire_parse_head(const QByteArray &body, const QString &station,
                         bbq_wire_head *head, QString *why);

#endif
