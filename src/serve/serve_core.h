#ifndef BBQ_SERVE_CORE_H
#define BBQ_SERVE_CORE_H

/*
 * The daemon's pure half (project.md sec 15.12.2): the request it accepts,
 * the names it will load, and the table it answers from. No sockets and
 * no globals, so the suite and a fuzzer can drive all of it.
 *
 * Plain C with no library beyond libc, because this is the part facing
 * the internet and every dependency is attack surface it would carry.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The request, exactly this long:
 *
 *    0  magic    "BBQR"
 *    4  version  1
 *    5  kind     1 stations, 2 index, 3 head, 4 month
 *    6  month    u16 LE, year*12 + (month-1); kind 4 only, else 0
 *    8  station  32 bytes, [A-Z0-9]{1,32} then NUL padding; all NUL for kind 1
 *   40  have     first 8 bytes of the signature on the client's copy, or zero
 */
#define BBQ_SERVE_REQUEST_SIZE 48
#define BBQ_SERVE_RESPONSE_HEADER_SIZE 12
#define BBQ_SERVE_STATION_MAX 32
#define BBQ_SERVE_SIGNATURE_SIZE 64
#define BBQ_SERVE_HAVE_SIZE 8

/* A published file must at least hold the common header and a signature. */
#define BBQ_SERVE_FILE_MIN (10 + BBQ_SERVE_SIGNATURE_SIZE)

/* The caps the applet enforces too (the format draft, sec 15.12). */
#define BBQ_SERVE_SMALL_FILE_MAX (1u << 20)
#define BBQ_SERVE_MONTH_FILE_MAX (4u << 20)

/* Months a station may hold: two centuries. */
#define BBQ_SERVE_MONTHS_MAX 2400

enum bbq_serve_kind {
	BBQ_SERVE_STATIONS = 1,
	BBQ_SERVE_INDEX = 2,
	BBQ_SERVE_HEAD = 3,
	BBQ_SERVE_MONTH = 4,
};

enum bbq_serve_status {
	BBQ_SERVE_OK = 0,
	BBQ_SERVE_UNCHANGED = 1,
	BBQ_SERVE_NOT_SERVED = 2,
	BBQ_SERVE_BAD_REQUEST = 3,
	BBQ_SERVE_BUSY = 4,
};

struct bbq_serve_request {
	uint8_t kind;
	uint16_t month;
	char station[BBQ_SERVE_STATION_MAX + 1];
	uint8_t have[BBQ_SERVE_HAVE_SIZE];
};

/* Returns 0 and fills `out` for a well-formed request, -1 otherwise. */
int bbq_serve_parse_request(const uint8_t *buffer, size_t length,
                            struct bbq_serve_request *out);

/* Whether `name` of `length` bytes is a station id: [A-Z0-9]{1,32}. */
int bbq_serve_station_valid(const char *name, size_t length);

/* "YYYY-MM" for a month number; returns -1 for a year outside 1970..2199. */
int bbq_serve_month_name(uint16_t month, char out[8]);

/* The inverse, accepting exactly "YYYY-MM"; returns 0 on success. */
int bbq_serve_parse_month_name(const char *name, uint16_t *month);

void bbq_serve_response_header(uint8_t out[BBQ_SERVE_RESPONSE_HEADER_SIZE],
                               enum bbq_serve_status status, uint32_t length);

/*
 * A file held in memory. Reference-counted, because a reload replaces
 * the table while connections are still writing out of the old one.
 */
struct bbq_serve_blob {
	uint32_t references;
	uint32_t size;
	uint8_t data[];
};

struct bbq_serve_blob *bbq_serve_blob_new(const uint8_t *data, uint32_t size);
struct bbq_serve_blob *bbq_serve_blob_ref(struct bbq_serve_blob *blob);
void bbq_serve_blob_unref(struct bbq_serve_blob *blob);

struct bbq_serve_station {
	char id[BBQ_SERVE_STATION_MAX + 1];
	struct bbq_serve_blob *index;
	struct bbq_serve_blob *head;

	/* Sorted ascending, parallel to month_blobs. */
	size_t month_count;
	uint16_t *months;
	struct bbq_serve_blob **month_blobs;
};

struct bbq_serve_table {
	struct bbq_serve_blob *stations;

	/* Sorted by id, so a lookup is a binary search. */
	size_t station_count;
	struct bbq_serve_station *station_list;
};

/*
 * Load the published tree under the directory `dir_fd`. Only names the
 * format defines are read, only regular files, never through a symlink,
 * and none over its cap; anything else is skipped. Returns NULL only
 * when memory runs out.
 */
struct bbq_serve_table *bbq_serve_table_load(int dir_fd);
void bbq_serve_table_free(struct bbq_serve_table *table);

/*
 * Answer a parsed request. Returns the status, and for BBQ_SERVE_OK the
 * blob to send, with a reference the caller owns.
 */
enum bbq_serve_status bbq_serve_answer(const struct bbq_serve_table *table,
                                       const struct bbq_serve_request *request,
                                       struct bbq_serve_blob **out);

/* Whether the table publishes `station`, a NUL-terminated valid id. */
int bbq_serve_table_has(const struct bbq_serve_table *table, const char *station);

/*
 * DEMAND (project.md sec 15.14): which stations clients ask for, so the
 * publisher can fetch the ones it does not yet publish and stop fetching
 * the ones nobody asks for any more.
 *
 * A request is the signal; nothing is added to the protocol. What this
 * decides is which requests become a line in the demand file, and it is
 * the part that faces the internet, so every table is bounded and a full
 * one refuses rather than grows:
 *
 *   - a station is written at most once an hour, whoever asks;
 *   - a station the table does not publish is written only for an
 *     address that has not already introduced three others today, so a
 *     stranger cannot hand the publisher a list of thousands. An IPv6
 *     address counts by its /64, which is what one subscriber is given;
 *   - when the address table is full of addresses still inside their day,
 *     an unknown address introduces nothing.
 *
 * A line is "<STATION> <epoch seconds>
". No address is written: the
 * limit is enforced here, so the file has no need of one.
 */
#define BBQ_SERVE_DEMAND_STATIONS 1024
#define BBQ_SERVE_DEMAND_ADDRESSES 4096
#define BBQ_SERVE_DEMAND_NEW_PER_DAY 3
#define BBQ_SERVE_DEMAND_EVERY_S 3600
#define BBQ_SERVE_DEMAND_DAY_S 86400
#define BBQ_SERVE_DEMAND_LINE_MAX 64
#define BBQ_SERVE_DEMAND_KEY_SIZE 16

/* The file stops growing here, until the publisher has read and emptied it. */
#define BBQ_SERVE_DEMAND_FILE_MAX (1u << 20)

struct bbq_serve_demand_station {
	char id[BBQ_SERVE_STATION_MAX + 1];
	int64_t written_s;
};

struct bbq_serve_demand_address {
	uint8_t key[BBQ_SERVE_DEMAND_KEY_SIZE];
	int64_t since_s;
	uint8_t count;
	char introduced[BBQ_SERVE_DEMAND_NEW_PER_DAY][BBQ_SERVE_STATION_MAX + 1];
};

struct bbq_serve_demand {
	size_t station_count;
	struct bbq_serve_demand_station stations[BBQ_SERVE_DEMAND_STATIONS];
	size_t address_count;
	struct bbq_serve_demand_address addresses[BBQ_SERVE_DEMAND_ADDRESSES];
};

/*
 * The key an address is limited by: an IPv4 address whole (4 bytes), an
 * IPv6 one by its first 8. Any other length gives the all-zero key, which
 * every such address then shares.
 */
void bbq_serve_demand_key(const uint8_t *address, size_t length,
                          uint8_t key[BBQ_SERVE_DEMAND_KEY_SIZE]);

/*
 * Note a request for `station` from `key` at `now_s`. Returns the length
 * of the line to write into `line`, or 0 when nothing is to be written.
 */
size_t bbq_serve_demand_note(struct bbq_serve_demand *demand,
                             const uint8_t key[BBQ_SERVE_DEMAND_KEY_SIZE],
                             const char *station, int published, int64_t now_s,
                             char line[BBQ_SERVE_DEMAND_LINE_MAX]);

#ifdef __cplusplus
}
#endif

#endif
