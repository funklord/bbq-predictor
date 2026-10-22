#ifndef BBQ_SERVE_LOOP_H
#define BBQ_SERVE_LOOP_H

/*
 * The daemon's network half (project.md sec 15.12.2): one thread, one
 * poll loop, every connection bounded in number, in time and in what it
 * may ask. Kept apart from main() so the suite can run it in a thread
 * against a socket it bound itself, which is the only way the loop is
 * tested at all.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bbq_serve_limits {
	/* Connections held at once, in total and from one address. */
	int max_connections;
	int per_address;

	/* A connection that makes no progress for this long is closed. */
	int idle_timeout_ms;

	/* Requests answered on one connection before it is closed. */
	int requests_per_connection;

	/* How often the published tree is checked for a new manifest. */
	int reload_check_ms;
};

/* What the daemon runs with unless told otherwise. */
struct bbq_serve_limits bbq_serve_default_limits(void);

struct bbq_serve_loop {
	/* The published tree; the table is loaded from it and reloaded. */
	int dir_fd;

	/* Bound, listening, non-blocking sockets. Not closed by the loop. */
	const int *listen_fds;
	size_t listen_count;

	/*
	 * The read end of a pipe: a byte 'q' stops the loop and 'h'
	 * reloads the table now. A signal handler writes to the other end.
	 */
	int wake_fd;

	struct bbq_serve_limits limits;

	/* Where a line of news goes; NULL for nowhere. */
	void (*log)(const char *line);
};

/* Runs until 'q' arrives. Returns 0 then, -1 if it could not start. */
int bbq_serve_run(const struct bbq_serve_loop *loop);

#ifdef __cplusplus
}
#endif

#endif
