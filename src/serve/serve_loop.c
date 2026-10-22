#define _GNU_SOURCE

#include "serve/serve_loop.h"
#include "serve/serve_core.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>

struct bbq_serve_limits bbq_serve_default_limits(void) {
	struct bbq_serve_limits limits;
	limits.max_connections = 512;
	limits.per_address = 8;
	limits.idle_timeout_ms = 10000;
	limits.requests_per_connection = 64;
	limits.reload_check_ms = 30000;
	return limits;
}

struct connection {
	int fd;
	struct sockaddr_storage peer;

	uint8_t request[BBQ_SERVE_REQUEST_SIZE];
	size_t got;

	/* Set while a response is being written. */
	int writing;
	uint8_t header[BBQ_SERVE_RESPONSE_HEADER_SIZE];
	size_t header_sent;
	struct bbq_serve_blob *blob;
	size_t blob_sent;

	int served;
	int close_after;
	int64_t last_progress_ms;
};

struct state {
	const struct bbq_serve_loop *loop;
	struct bbq_serve_table *table;
	struct connection *connections;
	size_t connection_count;

	/* What the manifest looked like at the last load. */
	struct stat manifest;
	int have_manifest;
};

static int64_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void say(const struct state *state, const char *format, ...)
        __attribute__((format(printf, 2, 3)));

static void say(const struct state *state, const char *format, ...) {
	if (!state->loop->log) {
		return;
	}

	char line[256];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(line, sizeof line, format, arguments);
	va_end(arguments);
	state->loop->log(line);
}

/*
 * The manifest is written last by the publisher, by rename, so a new
 * inode or a new mtime on it means a publish finished. Read without
 * following a symlink, like everything else here.
 */
static int manifest_changed(struct state *state) {
	struct stat now;
	if (fstatat(state->loop->dir_fd, "stations", &now, AT_SYMLINK_NOFOLLOW) != 0) {
		return state->have_manifest;
	}

	if (!state->have_manifest) {
		return 1;
	}

	return now.st_ino != state->manifest.st_ino ||
	       now.st_size != state->manifest.st_size ||
	       now.st_mtim.tv_sec != state->manifest.st_mtim.tv_sec ||
	       now.st_mtim.tv_nsec != state->manifest.st_mtim.tv_nsec;
}

static void reload(struct state *state) {
	struct stat manifest;
	const int have = fstatat(state->loop->dir_fd, "stations", &manifest,
	                         AT_SYMLINK_NOFOLLOW) == 0;

	struct bbq_serve_table *table = bbq_serve_table_load(state->loop->dir_fd);
	if (!table) {
		say(state, "reload failed: out of memory; still serving the old table");
		return;
	}

	/*
	 * Connections still writing hold their own references to the old
	 * table's files, so freeing it here cannot pull a file out from
	 * under a response.
	 */
	bbq_serve_table_free(state->table);
	state->table = table;
	state->manifest = manifest;
	state->have_manifest = have;

	size_t months = 0;
	for (size_t i = 0; i < table->station_count; ++i) {
		months += table->station_list[i].month_count;
	}
	say(state, "loaded %zu station(s), %zu month file(s)%s",
	    table->station_count, months,
	    table->stations ? "" : ", and no manifest");
}

static void close_connection(struct state *state, size_t index) {
	struct connection *connection = &state->connections[index];
	bbq_serve_blob_unref(connection->blob);
	close(connection->fd);

	state->connections[index] = state->connections[state->connection_count - 1];
	--state->connection_count;
}

static int same_address(const struct sockaddr_storage *a,
                        const struct sockaddr_storage *b) {
	if (a->ss_family != b->ss_family) {
		return 0;
	}

	if (a->ss_family == AF_INET) {
		const struct sockaddr_in *x = (const struct sockaddr_in *)a;
		const struct sockaddr_in *y = (const struct sockaddr_in *)b;
		return x->sin_addr.s_addr == y->sin_addr.s_addr;
	}

	if (a->ss_family == AF_INET6) {
		const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a;
		const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)b;
		return memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr) == 0;
	}

	return 0;
}

/* Best effort, never blocking: a refused client is not worth waiting on. */
static void refuse(int fd, enum bbq_serve_status status) {
	uint8_t header[BBQ_SERVE_RESPONSE_HEADER_SIZE];
	bbq_serve_response_header(header, status, 0);
	(void)send(fd, header, sizeof header, MSG_DONTWAIT | MSG_NOSIGNAL);
	close(fd);
}

static void accept_all(struct state *state, int listen_fd) {
	const struct bbq_serve_limits *limits = &state->loop->limits;

	for (;;) {
		struct sockaddr_storage peer;
		socklen_t length = sizeof peer;
		const int fd = accept4(listen_fd, (struct sockaddr *)&peer, &length,
		                       SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0) {
			/* EAGAIN is the normal end; anything else waits for the next poll. */
			return;
		}

		if (state->connection_count >= (size_t)limits->max_connections) {
			refuse(fd, BBQ_SERVE_BUSY);
			continue;
		}

		int from_peer = 0;
		for (size_t i = 0; i < state->connection_count; ++i) {
			from_peer += same_address(&state->connections[i].peer, &peer);
		}
		if (from_peer >= limits->per_address) {
			refuse(fd, BBQ_SERVE_BUSY);
			continue;
		}

		struct connection *connection =
		        &state->connections[state->connection_count++];
		memset(connection, 0, sizeof *connection);
		connection->fd = fd;
		connection->peer = peer;
		connection->last_progress_ms = now_ms();
	}
}

static void answer(struct state *state, struct connection *connection) {
	struct bbq_serve_request request;
	enum bbq_serve_status status;
	struct bbq_serve_blob *blob = NULL;

	if (bbq_serve_parse_request(connection->request, sizeof connection->request,
	                            &request) != 0) {
		status = BBQ_SERVE_BAD_REQUEST;
		connection->close_after = 1;
	} else {
		status = bbq_serve_answer(state->table, &request, &blob);
	}

	bbq_serve_response_header(connection->header, status, blob ? blob->size : 0);
	connection->header_sent = 0;
	connection->blob = blob;
	connection->blob_sent = 0;
	connection->writing = 1;
	connection->got = 0;

	if (++connection->served >= state->loop->limits.requests_per_connection) {
		connection->close_after = 1;
	}
}

/* Returns 0 while the connection lives, -1 when it should be closed. */
static int read_some(struct state *state, struct connection *connection) {
	const ssize_t n = recv(connection->fd, connection->request + connection->got,
	                       sizeof connection->request - connection->got, 0);
	if (n == 0) {
		return -1;
	}
	if (n < 0) {
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
	}

	connection->got += (size_t)n;
	connection->last_progress_ms = now_ms();

	if (connection->got == sizeof connection->request) {
		answer(state, connection);
	}
	return 0;
}

/* Returns 0 while the connection lives, -1 when it should be closed. */
static int write_some(struct connection *connection) {
	while (connection->header_sent < sizeof connection->header) {
		const ssize_t n = send(connection->fd,
		                       connection->header + connection->header_sent,
		                       sizeof connection->header - connection->header_sent,
		                       MSG_DONTWAIT | MSG_NOSIGNAL);
		if (n < 0) {
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
		}
		connection->header_sent += (size_t)n;
		connection->last_progress_ms = now_ms();
	}

	while (connection->blob && connection->blob_sent < connection->blob->size) {
		const ssize_t n = send(connection->fd,
		                       connection->blob->data + connection->blob_sent,
		                       connection->blob->size - connection->blob_sent,
		                       MSG_DONTWAIT | MSG_NOSIGNAL);
		if (n < 0) {
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
		}
		connection->blob_sent += (size_t)n;
		connection->last_progress_ms = now_ms();
	}

	bbq_serve_blob_unref(connection->blob);
	connection->blob = NULL;
	connection->writing = 0;
	return connection->close_after ? -1 : 0;
}

int bbq_serve_run(const struct bbq_serve_loop *loop) {
	const struct bbq_serve_limits *limits = &loop->limits;
	if (limits->max_connections < 1 || limits->per_address < 1 ||
	    limits->idle_timeout_ms < 1 || limits->requests_per_connection < 1 ||
	    limits->reload_check_ms < 1) {
		return -1;
	}

	struct state state;
	memset(&state, 0, sizeof state);
	state.loop = loop;
	state.connections = calloc((size_t)limits->max_connections,
	                           sizeof *state.connections);
	const size_t poll_capacity =
	        1 + loop->listen_count + (size_t)limits->max_connections;
	struct pollfd *polls = calloc(poll_capacity, sizeof *polls);
	if (!state.connections || !polls) {
		free(state.connections);
		free(polls);
		return -1;
	}

	reload(&state);
	if (!state.table) {
		free(state.connections);
		free(polls);
		return -1;
	}

	int64_t next_check_ms = now_ms() + limits->reload_check_ms;
	int running = 1;

	while (running) {
		size_t count = 0;
		polls[count].fd = loop->wake_fd;
		polls[count++].events = POLLIN;
		for (size_t i = 0; i < loop->listen_count; ++i) {
			polls[count].fd = loop->listen_fds[i];
			polls[count++].events = POLLIN;
		}
		const size_t first_connection = count;
		for (size_t i = 0; i < state.connection_count; ++i) {
			polls[count].fd = state.connections[i].fd;
			polls[count++].events =
			        state.connections[i].writing ? POLLOUT : POLLIN;
		}

		/* Wake for the nearest idle deadline or reload check. */
		const int64_t now = now_ms();
		int64_t wake_at = next_check_ms;
		for (size_t i = 0; i < state.connection_count; ++i) {
			const int64_t deadline =
			        state.connections[i].last_progress_ms + limits->idle_timeout_ms;
			if (deadline < wake_at) {
				wake_at = deadline;
			}
		}
		const int64_t wait = wake_at > now ? wake_at - now : 0;

		if (poll(polls, count, (int)(wait > 60000 ? 60000 : wait)) < 0 &&
		    errno != EINTR) {
			say(&state, "poll failed: %s", strerror(errno));
			break;
		}

		if (polls[0].revents & POLLIN) {
			char bytes[16];
			const ssize_t n = read(loop->wake_fd, bytes, sizeof bytes);
			for (ssize_t i = 0; i < n; ++i) {
				if (bytes[i] == 'q') {
					running = 0;
				} else if (bytes[i] == 'h') {
					reload(&state);
				}
			}
		}
		if (!running) {
			break;
		}

		/*
		 * Connections first, walking down so that closing one (which
		 * moves the last into its slot) never skips an unvisited one.
		 * Accepting comes after, so a new connection is not looked at
		 * against a poll result that was never about it.
		 */
		for (size_t i = state.connection_count; i-- > 0;) {
			const short revents = polls[first_connection + i].revents;
			struct connection *connection = &state.connections[i];
			int keep = 0;

			if (revents & (POLLERR | POLLNVAL)) {
				keep = -1;
			} else if (connection->writing && (revents & (POLLOUT | POLLHUP))) {
				keep = write_some(connection);
			} else if (!connection->writing && (revents & (POLLIN | POLLHUP))) {
				keep = read_some(&state, connection);
				/* Answer at once rather than waiting a round for POLLOUT. */
				if (keep == 0 && connection->writing) {
					keep = write_some(connection);
				}
			}

			if (keep == 0 && now_ms() - connection->last_progress_ms >=
			                         limits->idle_timeout_ms) {
				keep = -1;
			}
			if (keep != 0) {
				close_connection(&state, i);
			}
		}

		for (size_t i = 0; i < loop->listen_count; ++i) {
			if (polls[1 + i].revents & POLLIN) {
				accept_all(&state, loop->listen_fds[i]);
			}
		}

		if (now_ms() >= next_check_ms) {
			if (manifest_changed(&state)) {
				reload(&state);
			}
			next_check_ms = now_ms() + limits->reload_check_ms;
		}
	}

	while (state.connection_count > 0) {
		close_connection(&state, state.connection_count - 1);
	}
	bbq_serve_table_free(state.table);
	free(state.connections);
	free(polls);
	return 0;
}
