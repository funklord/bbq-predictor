#define _POSIX_C_SOURCE 200809L

#include "serve/serve_core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t request_magic[4] = {'B', 'B', 'Q', 'R'};
static const uint8_t response_magic[4] = {'B', 'B', 'Q', 'A'};

static uint16_t get_le16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

static void put_le32(uint8_t *p, uint32_t value) {
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

int bbq_serve_station_valid(const char *name, size_t length) {
	if (length == 0 || length > BBQ_SERVE_STATION_MAX) {
		return 0;
	}

	for (size_t i = 0; i < length; ++i) {
		const char c = name[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
			return 0;
		}
	}

	return 1;
}

int bbq_serve_month_name(uint16_t month, char out[8]) {
	const unsigned year = month / 12u;
	const unsigned number = month % 12u + 1u;

	if (year < 1970 || year > 2199) {
		return -1;
	}

	out[0] = (char)('0' + year / 1000);
	out[1] = (char)('0' + year / 100 % 10);
	out[2] = (char)('0' + year / 10 % 10);
	out[3] = (char)('0' + year % 10);
	out[4] = '-';
	out[5] = (char)('0' + number / 10);
	out[6] = (char)('0' + number % 10);
	out[7] = '\0';
	return 0;
}

int bbq_serve_parse_month_name(const char *name, uint16_t *month) {
	unsigned year = 0;
	unsigned number = 0;

	for (int i = 0; i < 7; ++i) {
		const char c = name[i];
		if (i == 4) {
			if (c != '-') {
				return -1;
			}
			continue;
		}
		if (c < '0' || c > '9') {
			return -1;
		}
		if (i < 4) {
			year = year * 10 + (unsigned)(c - '0');
		} else {
			number = number * 10 + (unsigned)(c - '0');
		}
	}

	if (name[7] != '\0' || year < 1970 || year > 2199 || number < 1 ||
	    number > 12) {
		return -1;
	}

	*month = (uint16_t)(year * 12 + number - 1);
	return 0;
}

int bbq_serve_parse_request(const uint8_t *buffer, size_t length,
                            struct bbq_serve_request *out) {
	if (length != BBQ_SERVE_REQUEST_SIZE) {
		return -1;
	}

	if (memcmp(buffer, request_magic, sizeof request_magic) != 0 ||
	    buffer[4] != 1) {
		return -1;
	}

	const uint8_t kind = buffer[5];
	if (kind < BBQ_SERVE_STATIONS || kind > BBQ_SERVE_MONTH) {
		return -1;
	}

	const uint16_t month = get_le16(buffer + 6);
	char name[8];
	if (kind == BBQ_SERVE_MONTH ? bbq_serve_month_name(month, name) != 0
	                            : month != 0) {
		return -1;
	}

	/*
	 * The id runs to the first NUL, and everything after it must be NUL
	 * too, so one request has exactly one spelling and no bytes ride
	 * along unchecked.
	 */
	const uint8_t *station = buffer + 8;
	size_t station_length = 0;
	while (station_length < BBQ_SERVE_STATION_MAX &&
	       station[station_length] != 0) {
		++station_length;
	}
	for (size_t i = station_length; i < BBQ_SERVE_STATION_MAX; ++i) {
		if (station[i] != 0) {
			return -1;
		}
	}

	if (kind == BBQ_SERVE_STATIONS) {
		if (station_length != 0) {
			return -1;
		}
	} else if (!bbq_serve_station_valid((const char *)station,
	                                    station_length)) {
		return -1;
	}

	out->kind = kind;
	out->month = month;
	memcpy(out->station, station, station_length);
	out->station[station_length] = '\0';
	memcpy(out->have, buffer + 40, BBQ_SERVE_HAVE_SIZE);
	return 0;
}

void bbq_serve_response_header(uint8_t out[BBQ_SERVE_RESPONSE_HEADER_SIZE],
                               enum bbq_serve_status status, uint32_t length) {
	memcpy(out, response_magic, sizeof response_magic);
	out[4] = (uint8_t)status;
	out[5] = 0;
	out[6] = 0;
	out[7] = 0;
	put_le32(out + 8, length);
}

struct bbq_serve_blob *bbq_serve_blob_new(const uint8_t *data, uint32_t size) {
	struct bbq_serve_blob *blob = malloc(sizeof *blob + size);
	if (!blob) {
		return NULL;
	}

	blob->references = 1;
	blob->size = size;
	memcpy(blob->data, data, size);
	return blob;
}

struct bbq_serve_blob *bbq_serve_blob_ref(struct bbq_serve_blob *blob) {
	if (blob) {
		++blob->references;
	}
	return blob;
}

void bbq_serve_blob_unref(struct bbq_serve_blob *blob) {
	if (blob && --blob->references == 0) {
		free(blob);
	}
}

/*
 * Read one regular file, refusing symlinks, anything not a regular file,
 * and anything over `cap` or under the smallest file the format allows.
 * NULL for all of those alike: the caller skips what it cannot serve.
 */
static struct bbq_serve_blob *load_file(int dir_fd, const char *name,
                                        uint32_t cap) {
	const int fd = openat(dir_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}

	struct stat info;
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
	    info.st_size < BBQ_SERVE_FILE_MIN || (uint64_t)info.st_size > cap) {
		close(fd);
		return NULL;
	}

	const uint32_t size = (uint32_t)info.st_size;
	struct bbq_serve_blob *blob = malloc(sizeof *blob + size);
	if (!blob) {
		close(fd);
		return NULL;
	}

	uint32_t got = 0;
	while (got < size) {
		const ssize_t n = read(fd, blob->data + got, size - got);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		got += (uint32_t)n;
	}
	close(fd);

	/* A file that changed size under the read is not served half-read. */
	if (got != size) {
		free(blob);
		return NULL;
	}

	blob->references = 1;
	blob->size = size;
	return blob;
}

/*
 * A directory stream of its own over `dir_fd`, starting at the first
 * entry.
 *
 * NOT dup(): a duplicated descriptor shares its file offset, and a
 * directory's read position IS that offset. The first load read the
 * shared one to the end, so every reload after it listed nothing and
 * the daemon served an empty tree from its first republish on. The
 * loop's reload case found it. openat(".") opens the same directory
 * with an offset of its own, and the rewind is for any platform where
 * fdopendir does not start at the top.
 */
static DIR *open_listing(int dir_fd) {
	const int fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}

	DIR *listing = fdopendir(fd);
	if (!listing) {
		close(fd);
		return NULL;
	}

	rewinddir(listing);
	return listing;
}

static int compare_station(const void *a, const void *b) {
	return strcmp(((const struct bbq_serve_station *)a)->id,
	              ((const struct bbq_serve_station *)b)->id);
}

static int compare_month(const void *a, const void *b) {
	const uint16_t x = *(const uint16_t *)a;
	const uint16_t y = *(const uint16_t *)b;
	return (x > y) - (x < y);
}

static void station_free(struct bbq_serve_station *station) {
	bbq_serve_blob_unref(station->index);
	bbq_serve_blob_unref(station->head);
	for (size_t i = 0; i < station->month_count; ++i) {
		bbq_serve_blob_unref(station->month_blobs[i]);
	}
	free(station->months);
	free(station->month_blobs);
}

/* Returns -1 only when memory runs out. */
static int station_load(int root_fd, const char *id,
                        struct bbq_serve_station *station) {
	memset(station, 0, sizeof *station);
	memcpy(station->id, id, strlen(id) + 1);

	const int fd = openat(root_fd, id,
	                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		return 0;
	}

	station->index = load_file(fd, "index", BBQ_SERVE_SMALL_FILE_MAX);
	station->head = load_file(fd, "head", BBQ_SERVE_SMALL_FILE_MAX);

	DIR *listing = open_listing(fd);
	if (!listing) {
		close(fd);
		return 0;
	}

	size_t capacity = 0;
	int result = 0;
	struct dirent *entry;
	while ((entry = readdir(listing)) != NULL) {
		uint16_t month;
		if (strlen(entry->d_name) != 7 ||
		    bbq_serve_parse_month_name(entry->d_name, &month) != 0) {
			continue;
		}
		if (station->month_count == BBQ_SERVE_MONTHS_MAX) {
			break;
		}

		struct bbq_serve_blob *blob =
		        load_file(fd, entry->d_name, BBQ_SERVE_MONTH_FILE_MAX);
		if (!blob) {
			continue;
		}

		if (station->month_count == capacity) {
			const size_t grown = capacity ? capacity * 2 : 16;
			uint16_t *months = realloc(station->months,
			                           grown * sizeof *months);
			if (months) {
				station->months = months;
			}
			struct bbq_serve_blob **blobs = realloc(
			        station->month_blobs, grown * sizeof *blobs);
			if (blobs) {
				station->month_blobs = blobs;
			}
			if (!months || !blobs) {
				bbq_serve_blob_unref(blob);
				result = -1;
				break;
			}
			capacity = grown;
		}

		station->months[station->month_count] = month;
		station->month_blobs[station->month_count] = blob;
		++station->month_count;
	}
	closedir(listing);
	close(fd);

	/*
	 * Sort the two parallel arrays together, by month. An insertion sort,
	 * because there are at most a couple of thousand and a directory
	 * listing mostly hands them over in order already.
	 */
	for (size_t i = 1; i < station->month_count; ++i) {
		const uint16_t month = station->months[i];
		struct bbq_serve_blob *blob = station->month_blobs[i];
		size_t j = i;
		while (j > 0 && station->months[j - 1] > month) {
			station->months[j] = station->months[j - 1];
			station->month_blobs[j] = station->month_blobs[j - 1];
			--j;
		}
		station->months[j] = month;
		station->month_blobs[j] = blob;
	}

	return result;
}

struct bbq_serve_table *bbq_serve_table_load(int dir_fd) {
	struct bbq_serve_table *table = calloc(1, sizeof *table);
	if (!table) {
		return NULL;
	}

	table->stations = load_file(dir_fd, "stations", BBQ_SERVE_SMALL_FILE_MAX);

	DIR *listing = open_listing(dir_fd);
	if (!listing) {
		return table;
	}

	size_t capacity = 0;
	struct dirent *entry;
	while ((entry = readdir(listing)) != NULL) {
		const size_t length = strlen(entry->d_name);
		if (!bbq_serve_station_valid(entry->d_name, length)) {
			continue;
		}

		if (table->station_count == capacity) {
			const size_t grown = capacity ? capacity * 2 : 16;
			struct bbq_serve_station *list = realloc(
			        table->station_list, grown * sizeof *list);
			if (!list) {
				closedir(listing);
				bbq_serve_table_free(table);
				return NULL;
			}
			table->station_list = list;
			capacity = grown;
		}

		struct bbq_serve_station *station =
		        &table->station_list[table->station_count];
		const int loaded = station_load(dir_fd, entry->d_name, station);
		if (loaded != 0) {
			station_free(station);
			closedir(listing);
			bbq_serve_table_free(table);
			return NULL;
		}

		/* A directory holding nothing servable is not a station. */
		if (!station->index && !station->head && station->month_count == 0) {
			station_free(station);
			continue;
		}
		++table->station_count;
	}
	closedir(listing);

	qsort(table->station_list, table->station_count,
	      sizeof *table->station_list, compare_station);
	return table;
}

void bbq_serve_table_free(struct bbq_serve_table *table) {
	if (!table) {
		return;
	}

	bbq_serve_blob_unref(table->stations);
	for (size_t i = 0; i < table->station_count; ++i) {
		station_free(&table->station_list[i]);
	}
	free(table->station_list);
	free(table);
}

static int unchanged(const struct bbq_serve_blob *blob,
                     const uint8_t have[BBQ_SERVE_HAVE_SIZE]) {
	static const uint8_t none[BBQ_SERVE_HAVE_SIZE];
	if (memcmp(have, none, sizeof none) == 0) {
		return 0;
	}

	const uint8_t *signature = blob->data + blob->size - BBQ_SERVE_SIGNATURE_SIZE;
	return memcmp(signature, have, BBQ_SERVE_HAVE_SIZE) == 0;
}

enum bbq_serve_status bbq_serve_answer(const struct bbq_serve_table *table,
                                       const struct bbq_serve_request *request,
                                       struct bbq_serve_blob **out) {
	struct bbq_serve_blob *blob = NULL;
	*out = NULL;

	if (request->kind == BBQ_SERVE_STATIONS) {
		blob = table->stations;
	} else {
		/*
		 * Every bsearch is guarded by its count. An empty array is
		 * NULL here, and passing NULL to bsearch is undefined even
		 * with a count of zero -- the fuzzer found it in seconds, with
		 * a month asked of a station that has none.
		 */
		struct bbq_serve_station key;
		memcpy(key.id, request->station, sizeof key.id);
		const struct bbq_serve_station *station =
		        table->station_count == 0
		                ? NULL
		                : bsearch(&key, table->station_list,
		                          table->station_count,
		                          sizeof *table->station_list, compare_station);
		if (station) {
			if (request->kind == BBQ_SERVE_INDEX) {
				blob = station->index;
			} else if (request->kind == BBQ_SERVE_HEAD) {
				blob = station->head;
			} else if (station->month_count != 0) {
				const uint16_t *found = bsearch(
				        &request->month, station->months,
				        station->month_count, sizeof *station->months,
				        compare_month);
				if (found) {
					blob = station->month_blobs[found - station->months];
				}
			}
		}
	}

	if (!blob) {
		return BBQ_SERVE_NOT_SERVED;
	}

	if (unchanged(blob, request->have)) {
		return BBQ_SERVE_UNCHANGED;
	}

	*out = bbq_serve_blob_ref(blob);
	return BBQ_SERVE_OK;
}
