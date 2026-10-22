#ifndef BBQ_ENCODE_H
#define BBQ_ENCODE_H

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

#include "model/sample.h"
#include "model/series.h"
#include "wire/format.h"

class bbq_signing_key;

/*
 * The published format's encoder (project.md sec 15.12.3). Pure: bytes
 * in, bytes out, so the suite checks every file kind without a disk or
 * a network.
 *
 * `src/wire/bbq.situ` is the specification and this follows it member
 * for member. The suite reads what this writes back through the code
 * situ generates from that schema, so the two are checked against each
 * other rather than against my reading of either.
 */

/*
 * The inflated column stream for `samples`, which must ascend by start.
 * Empty, with `error` set, for anything the format cannot carry.
 */
QByteArray bbq_wire_columns(const std::vector<bbq_sample> &samples, int rows_max,
                            QString *error);

/* A u32 length, then qCompress's own output: its BE length and zlib. */
QByteArray bbq_wire_packed(const QByteArray &inflated);

QByteArray bbq_wire_month_body(const QString &station, uint16_t month,
                               const bbq_wire_month_rows &rows, QString *error);

/* Bands that have no wire code, or no samples, are left out. */
QByteArray bbq_wire_head_body(const QString &station, qint64 generated_utc,
                              const std::vector<const bbq_series *> &bands,
                              QString *error);

/* Entries must ascend by month without repeats. */
QByteArray bbq_wire_index_body(const QString &station, qint64 generated_utc,
                               const std::vector<bbq_wire_index_entry> &entries,
                               QString *error);

QByteArray bbq_wire_stations_body(qint64 generated_utc, const QStringList &stations,
                                  QString *error);

/* Header, body, and the Ed25519 signature over both. */
QByteArray bbq_wire_file(bbq_wire_kind kind, const QByteArray &body,
                         const bbq_signing_key &key);

#endif
