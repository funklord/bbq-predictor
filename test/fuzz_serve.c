/*
 * libFuzzer target for the daemon's request path (project.md sec
 * 15.12.2): whatever a stranger sends goes through the parser, and a
 * request it accepts is answered from a small fixed table, so both the
 * parser and the lookup see hostile input. Built and run by `make
 * fuzz-serve`, under AddressSanitizer and UndefinedBehaviorSanitizer.
 *
 * Every accepted request is also checked against the rules it must have
 * passed, so a parser that accepted something it should not is a crash
 * here rather than a quiet pass.
 */
#include "serve/serve_core.h"

#include <stdlib.h>
#include <string.h>

static struct bbq_serve_table *table;

/*
 * The demand tracker (sec 15.14) persists across inputs, as it does
 * across requests in the daemon, so the fuzzer fills its tables and
 * drives the eviction and full-table paths rather than only empty ones.
 */
static struct bbq_serve_demand *demand;

static struct bbq_serve_blob *blob_of(uint32_t size, uint8_t fill) {
	uint8_t bytes[BBQ_SERVE_FILE_MIN + 16];
	memset(bytes, fill, sizeof bytes);
	return bbq_serve_blob_new(bytes, size);
}

static void build_table(void) {
	static uint16_t months[2] = {2026 * 12 + 7, 2026 * 12 + 8};
	static struct bbq_serve_blob *month_blobs[2];
	static struct bbq_serve_station stations[2];

	table = calloc(1, sizeof *table);
	table->stations = blob_of(BBQ_SERVE_FILE_MIN, 1);

	strcpy(stations[0].id, "A1");
	stations[0].head = blob_of(BBQ_SERVE_FILE_MIN + 1, 2);
	strcpy(stations[1].id, "ISTOCK877");
	stations[1].index = blob_of(BBQ_SERVE_FILE_MIN + 2, 3);
	month_blobs[0] = blob_of(BBQ_SERVE_FILE_MIN + 3, 4);
	month_blobs[1] = blob_of(BBQ_SERVE_FILE_MIN + 4, 5);
	stations[1].months = months;
	stations[1].month_blobs = month_blobs;
	stations[1].month_count = 2;

	table->station_list = stations;
	table->station_count = 2;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (!table) {
		build_table();
	}

	struct bbq_serve_request request;
	if (bbq_serve_parse_request(data, size, &request) != 0) {
		return 0;
	}

	/* What an accepted request must satisfy. */
	if (size != BBQ_SERVE_REQUEST_SIZE || request.kind < 1 || request.kind > 4) {
		abort();
	}
	const size_t length = strlen(request.station);
	if (request.kind == BBQ_SERVE_STATIONS ? length != 0
	                                       : !bbq_serve_station_valid(request.station, length)) {
		abort();
	}

	struct bbq_serve_blob *blob = NULL;
	const enum bbq_serve_status status = bbq_serve_answer(table, &request, &blob);
	if ((status == BBQ_SERVE_OK) != (blob != NULL)) {
		abort();
	}
	if (blob) {
		/* Touch every byte, so a bad pointer is a sanitizer report. */
		volatile uint8_t sum = 0;
		for (uint32_t i = 0; i < blob->size; ++i) {
			sum = (uint8_t)(sum + blob->data[i]);
		}
		bbq_serve_blob_unref(blob);
	}

	if (!demand) {
		demand = calloc(1, sizeof *demand);
	}
	if (request.kind != BBQ_SERVE_STATIONS) {
		/*
		 * The address and the clock come from the request's own bytes,
		 * so the fuzzer steers them too: `have` as the address, IPv4 or
		 * IPv6 by its first bit, and the month as hours on the clock.
		 */
		uint8_t address[16];
		memcpy(address, request.have, 8);
		memcpy(address + 8, request.have, 8);
		uint8_t key[BBQ_SERVE_DEMAND_KEY_SIZE];
		bbq_serve_demand_key(address, (request.have[0] & 1) ? 16 : 4, key);

		char line[BBQ_SERVE_DEMAND_LINE_MAX];
		const int published = bbq_serve_table_has(table, request.station);
		const size_t written = bbq_serve_demand_note(
		        demand, key, request.station, published,
		        1792000000 + (int64_t)request.month * 3600, line);

		/* A line is the station, a space, digits and a newline, and nothing else. */
		if (written != 0) {
			const size_t id = strlen(request.station);
			if (written >= BBQ_SERVE_DEMAND_LINE_MAX || written < id + 3 ||
			    memcmp(line, request.station, id) != 0 || line[id] != ' ' ||
			    line[written - 1] != '\n') {
				abort();
			}
		}
		if (demand->station_count > BBQ_SERVE_DEMAND_STATIONS ||
		    demand->address_count > BBQ_SERVE_DEMAND_ADDRESSES) {
			abort();
		}
	}

	return 0;
}
