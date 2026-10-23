/*
 * bbq-predictor-serve -- answer published weather files over TCP
 * (project.md sec 15.12.2).
 *
 * Binds its ports, then confines itself: when started as root it
 * chroots into the published tree and becomes an unprivileged user
 * before it reads a byte from anybody. It refuses to serve as root at
 * all, since nothing it does after binding needs root and a parser bug
 * is cheaper in an empty chroot as nobody.
 *
 * Runs in the foreground and logs to stderr. Backgrounding, pidfiles and
 * log files are the init system's job (packaging/sysvinit, packaging/openrc).
 */
#define _GNU_SOURCE

#include "serve/serve_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <netinet/in.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#define PROGRAM "bbq-predictor-serve"
#define MAX_PORTS 4

static int wake_write_fd = -1;

static void on_signal(int number) {
	const int saved = errno;
	const char byte = number == SIGHUP ? 'h' : 'q';
	(void)!write(wake_write_fd, &byte, 1);
	errno = saved;
}

static void log_line(const char *line) {
	fprintf(stderr, PROGRAM ": %s\n", line);
}

static void usage(FILE *out) {
	fprintf(out,
	        "usage: " PROGRAM " --dir DIR [--port N]... [--user NAME]\n"
	        "                           [--demand FILE]\n"
	        "                           [--max-connections N] [--per-address N]\n"
	        "\n"
	        "Serve the files bbq-predictor-publish wrote under DIR.\n"
	        "Ports default to 7227 and 443. Started as root it needs --user,\n"
	        "and chroots into DIR as that user once the ports are bound.\n"
	        "--demand appends the stations clients ask for to FILE, which must\n"
	        "exist, for the publisher to read.\n");
}

/* A whole decimal number in [low, high], or -1. */
static long number(const char *text, long low, long high) {
	char *end = NULL;
	errno = 0;
	const long value = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value < low || value > high) {
		return -1;
	}
	return value;
}

static int bind_port(int port) {
	/*
	 * One dual-stack IPv6 socket where the machine has IPv6, so a port
	 * is one socket rather than two; IPv4 alone where it does not.
	 */
	int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	const int one = 1;
	const int zero = 0;

	if (fd >= 0) {
		struct sockaddr_in6 address;
		memset(&address, 0, sizeof address);
		address.sin6_family = AF_INET6;
		address.sin6_addr = in6addr_any;
		address.sin6_port = htons((uint16_t)port);

		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
		if (bind(fd, (struct sockaddr *)&address, sizeof address) == 0 &&
		    listen(fd, 128) == 0) {
			return fd;
		}
		if (errno != EAFNOSUPPORT && errno != EADDRNOTAVAIL) {
			fprintf(stderr, PROGRAM ": port %d: %s\n", port, strerror(errno));
			close(fd);
			return -1;
		}
		close(fd);
	}

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, PROGRAM ": socket: %s\n", strerror(errno));
		return -1;
	}

	struct sockaddr_in address;
	memset(&address, 0, sizeof address);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons((uint16_t)port);

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
	    listen(fd, 128) != 0) {
		fprintf(stderr, PROGRAM ": port %d: %s\n", port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * chroot into the tree and become `user`, or fail. Returns the directory
 * descriptor to serve from, now "/" inside the chroot.
 */
static int confine(const char *dir, const char *user) {
	errno = 0;
	const struct passwd *account = getpwnam(user);
	if (!account) {
		fprintf(stderr, PROGRAM ": no user %s\n", user);
		return -1;
	}
	if (account->pw_uid == 0 || account->pw_gid == 0) {
		fprintf(stderr, PROGRAM ": refusing to serve as %s, which is root\n", user);
		return -1;
	}
	const uid_t uid = account->pw_uid;
	const gid_t gid = account->pw_gid;

	if (chroot(dir) != 0 || chdir("/") != 0) {
		fprintf(stderr, PROGRAM ": chroot %s: %s\n", dir, strerror(errno));
		return -1;
	}

	if (setgroups(0, NULL) != 0 || setgid(gid) != 0 || setuid(uid) != 0) {
		fprintf(stderr, PROGRAM ": dropping to %s: %s\n", user, strerror(errno));
		return -1;
	}

	/* Proved rather than assumed: root must be gone for good. */
	if (setuid(0) == 0 || geteuid() == 0) {
		fprintf(stderr, PROGRAM ": could still regain root after dropping\n");
		return -1;
	}

	return open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

int main(int argc, char **argv) {
	const char *dir = NULL;
	const char *user = NULL;
	const char *demand = NULL;
	int ports[MAX_PORTS];
	int port_count = 0;
	struct bbq_serve_limits limits = bbq_serve_default_limits();

	for (int i = 1; i < argc; ++i) {
		const char *option = argv[i];
		const char *value = i + 1 < argc ? argv[i + 1] : NULL;

		if (strcmp(option, "--help") == 0) {
			usage(stdout);
			return 0;
		}
		if (!value) {
			usage(stderr);
			return 2;
		}

		long parsed = 0;
		if (strcmp(option, "--dir") == 0) {
			dir = value;
		} else if (strcmp(option, "--user") == 0) {
			user = value;
		} else if (strcmp(option, "--demand") == 0) {
			demand = value;
		} else if (strcmp(option, "--port") == 0 && port_count < MAX_PORTS &&
		           (parsed = number(value, 1, 65535)) > 0) {
			ports[port_count++] = (int)parsed;
		} else if (strcmp(option, "--max-connections") == 0 &&
		           (parsed = number(value, 1, 65536)) > 0) {
			limits.max_connections = (int)parsed;
		} else if (strcmp(option, "--per-address") == 0 &&
		           (parsed = number(value, 1, 65536)) > 0) {
			limits.per_address = (int)parsed;
		} else {
			fprintf(stderr, PROGRAM ": bad option or value: %s %s\n", option, value);
			return 2;
		}
		++i;
	}

	if (!dir) {
		usage(stderr);
		return 2;
	}
	if (port_count == 0) {
		ports[port_count++] = 7227;
		ports[port_count++] = 443;
	}

	const int as_root = geteuid() == 0;
	if (as_root && !user) {
		fprintf(stderr, PROGRAM ": started as root, so --user is required\n");
		return 2;
	}
	if (!as_root && user) {
		fprintf(stderr, PROGRAM ": --user needs to be started as root\n");
		return 2;
	}

	/* Every connection is a descriptor; make sure the limit allows them. */
	struct rlimit files;
	if (getrlimit(RLIMIT_NOFILE, &files) == 0) {
		const rlim_t want = (rlim_t)limits.max_connections + 32;
		if (files.rlim_cur < want) {
			files.rlim_cur = want < files.rlim_max ? want : files.rlim_max;
			setrlimit(RLIMIT_NOFILE, &files);
			getrlimit(RLIMIT_NOFILE, &files);
		}
		if (files.rlim_cur < (rlim_t)limits.max_connections + 32) {
			limits.max_connections = (int)files.rlim_cur - 32;
			if (limits.max_connections < 1) {
				fprintf(stderr, PROGRAM ": too few file descriptors allowed\n");
				return 1;
			}
		}
	}

	/*
	 * The demand file (sec 15.14), opened BEFORE the chroot because it
	 * lives outside the tree, and appended to through this one descriptor
	 * for the life of the process. Never created here -- root would own
	 * it and the publisher could not empty it -- and never through a
	 * symlink. Asked for and unusable is a configuration error, said at
	 * start rather than discovered as stations that never get added.
	 */
	int demand_fd = -1;
	if (demand) {
		demand_fd = open(demand, O_WRONLY | O_APPEND | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
		struct stat file;
		if (demand_fd < 0 || fstat(demand_fd, &file) != 0 || !S_ISREG(file.st_mode)) {
			fprintf(stderr, PROGRAM ": demand file %s: %s\n", demand,
			        demand_fd < 0 ? strerror(errno) : "not a regular file");
			return 2;
		}
	}

	int listen_fds[MAX_PORTS];
	for (int i = 0; i < port_count; ++i) {
		listen_fds[i] = bind_port(ports[i]);
		if (listen_fds[i] < 0) {
			return 1;
		}
	}

	int dir_fd;
	if (as_root) {
		dir_fd = confine(dir, user);
	} else {
		dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	}
	if (dir_fd < 0) {
		fprintf(stderr, PROGRAM ": cannot serve %s\n", dir);
		return 1;
	}

#ifdef __linux__
	/* Nothing this process runs may gain privileges, should it ever exec. */
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#endif

	int wake[2];
	if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0) {
		fprintf(stderr, PROGRAM ": pipe: %s\n", strerror(errno));
		return 1;
	}
	wake_write_fd = wake[1];

	struct sigaction action;
	memset(&action, 0, sizeof action);
	action.sa_handler = on_signal;
	sigemptyset(&action.sa_mask);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	signal(SIGPIPE, SIG_IGN);

	fprintf(stderr, PROGRAM ": serving %s on port(s)", dir);
	for (int i = 0; i < port_count; ++i) {
		fprintf(stderr, " %d", ports[i]);
	}
	fprintf(stderr, "%s\n", as_root ? ", confined" : ", unconfined (not root)");

	struct bbq_serve_loop loop;
	memset(&loop, 0, sizeof loop);
	loop.dir_fd = dir_fd;
	loop.listen_fds = listen_fds;
	loop.listen_count = (size_t)port_count;
	loop.wake_fd = wake[0];
	loop.limits = limits;
	loop.record_demand = demand_fd >= 0;
	loop.demand_fd = demand_fd;
	loop.log = log_line;

	const int result = bbq_serve_run(&loop);
	fprintf(stderr, PROGRAM ": %s\n", result == 0 ? "stopped" : "could not start");
	return result == 0 ? 0 : 1;
}
