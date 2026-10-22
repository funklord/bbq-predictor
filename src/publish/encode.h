#ifndef BBQ_ENCODE_H
#define BBQ_ENCODE_H

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

#include "model/sample.h"
#include "model/series.h"

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

enum class bbq_wire_kind : uint8_t {
	stations = 1,
	index = 2,
	head = 3,
	month = 4,
};

/* Caps from the schema. */
constexpr int bbq_wire_month_rows_max = 44640;
constexpr int bbq_wire_head_rows_max = 4096;
constexpr int bbq_wire_head_bands_max = 16;
constexpr int bbq_wire_providers_max = 16;
constexpr int bbq_wire_inflated_max = 8 * 1024 * 1024;

/*
 * The band's number on the wire. A contract with every client, so it is
 * written out here rather than taken from bbq_band's declaration order,
 * which nothing stops somebody reordering. 0 means "not published".
 */
uint8_t bbq_wire_band_code(bbq_band band);

/* [A-Z0-9]{1,32}: the only station ids that may become a file name. */
bool bbq_wire_station_valid(const QString &station);

/* year*12 + (month-1), for the UTC month holding `utc`. */
uint16_t bbq_wire_month_of(qint64 utc);

/* The first second of a month number, and of the one after it. */
qint64 bbq_wire_month_begin(uint16_t month);
qint64 bbq_wire_month_end(uint16_t month);

/* "YYYY-MM", the month file's name. */
QString bbq_wire_month_name(uint16_t month);

/*
 * The inflated column stream for `samples`, which must ascend by start.
 * Empty, with `error` set, for anything the format cannot carry.
 */
QByteArray bbq_wire_columns(const std::vector<bbq_sample> &samples, int rows_max,
                            QString *error);

/* A u32 length, then qCompress's own output: its BE length and zlib. */
QByteArray bbq_wire_packed(const QByteArray &inflated);

struct bbq_wire_month_rows {
	std::vector<bbq_sample> samples;

	/* Index into `providers` for each sample. */
	std::vector<uint8_t> provider;
	QStringList providers;
};

QByteArray bbq_wire_month_body(const QString &station, uint16_t month,
                               const bbq_wire_month_rows &rows, QString *error);

/* Bands that have no wire code, or no samples, are left out. */
QByteArray bbq_wire_head_body(const QString &station, qint64 generated_utc,
                              const std::vector<const bbq_series *> &bands,
                              QString *error);

struct bbq_wire_index_entry {
	uint16_t month = 0;
	uint32_t rows = 0;
	uint32_t bytes = 0;
	QByteArray digest;  /* 16 bytes: BLAKE2b-128 of the month file */
	qint64 first_utc = 0;
	qint64 last_utc = 0;
};

/* Entries must ascend by month without repeats. */
QByteArray bbq_wire_index_body(const QString &station, qint64 generated_utc,
                               const std::vector<bbq_wire_index_entry> &entries,
                               QString *error);

QByteArray bbq_wire_stations_body(qint64 generated_utc, const QStringList &stations,
                                  QString *error);

/* BLAKE2b-128 of a whole file, as the index records it. */
QByteArray bbq_wire_digest(const QByteArray &file);

/* Header, body, and the Ed25519 signature over both. */
QByteArray bbq_wire_file(bbq_wire_kind kind, const QByteArray &body,
                         const bbq_signing_key &key);

#endif
