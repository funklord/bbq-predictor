#ifndef BBQ_WIRE_FORMAT_H
#define BBQ_WIRE_FORMAT_H

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

#include "model/sample.h"
#include "model/series.h"

/*
 * What the publisher and the applet agree on about the published format
 * (project.md sec 15.12.3), apart from the bytes themselves, which
 * src/wire/bbq.situ specifies. The publisher's encoder and the applet's
 * reader both build on this, and only the reader is in the Android
 * build, so nothing here may reach for the publisher.
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

/* The reverse, for the reader; false for a code this build does not know. */
bool bbq_wire_band_from_code(uint8_t code, bbq_band *band);

/* One month's rows, as the publisher writes them and the reader returns them. */
struct bbq_wire_month_rows {
	std::vector<bbq_sample> samples;

	/* Index into `providers` for each sample. */
	std::vector<uint8_t> provider;
	QStringList providers;
};

struct bbq_wire_index_entry {
	uint16_t month = 0;
	uint32_t rows = 0;
	uint32_t bytes = 0;
	QByteArray digest;  /* 16 bytes: BLAKE2b-128 of the month file */
	qint64 first_utc = 0;
	qint64 last_utc = 0;
};

/* BLAKE2b-128 of a whole file, as the index records it. */
QByteArray bbq_wire_digest(const QByteArray &file);

/* Whether `signature` is `public_key`'s Ed25519 signature over `message`. */
bool bbq_signature_valid(const QByteArray &signature,
                         const QByteArray &public_key,
                         const QByteArray &message);

#endif
