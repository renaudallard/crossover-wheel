/*
 * t150d - the macOS side of the force feedback bridge.
 *
 * Listens on loopback for the proxy DLL running inside CrossOver, turns the
 * effects it sends into wheel packets, and writes them out through a
 * backend. Until the macOS HID backend exists the only backend is the
 * logging one, so this drives nothing yet and says so at startup.
 *
 * One client at a time. A second connection displaces the first, but only
 * after the first has been put back into a safe state, because the common
 * case for a second connection is a game that crashed and was restarted.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "t150/proto.h"
#include "t150d.h"

#define RXBUF	1024
#define ENDPOINT_REL	"/Library/Application Support/t150ffb/endpoint"

static volatile sig_atomic_t quit;

static void
on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;

	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 16 random bytes as 32 hex characters, which is the token on the wire. */
static int
make_token(char *out, size_t outlen)
{
	static const char hex[] = "0123456789abcdef";
	unsigned char raw[T150_TOKEN_LEN / 2];
	ssize_t n;
	size_t i;
	int fd;

	if (outlen < T150_TOKEN_LEN + 1)
		return -1;
	if ((fd = open("/dev/urandom", O_RDONLY)) == -1)
		return -1;

	n = read(fd, raw, sizeof(raw));
	(void)close(fd);
	if (n != (ssize_t)sizeof(raw))
		return -1;

	for (i = 0; i < sizeof(raw); i++) {
		out[i * 2] = hex[raw[i] >> 4];
		out[i * 2 + 1] = hex[raw[i] & 0x0f];
	}
	out[T150_TOKEN_LEN] = '\0';

	return 0;
}

/* mkdir -p for the directory holding path, ignoring components that exist. */
static int
mkpath(const char *path)
{
	char buf[PATH_MAX];
	char *p;

	if (strlen(path) >= sizeof(buf))
		return -1;
	(void)strncpy(buf, path, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	if ((p = strrchr(buf, '/')) == NULL)
		return 0;
	*p = '\0';

	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0700) == -1 && errno != EEXIST)
			return -1;
		*p = '/';
	}

	if (mkdir(buf, 0700) == -1 && errno != EEXIST)
		return -1;

	return 0;
}

/*
 * Publish the port and token where the DLL can read them through Wine's Z:
 * mapping. Written to a temporary and renamed, so a client never reads half
 * a file, and 0600 because anything that can read it can drive the motors.
 */
static int
write_endpoint(const char *path, unsigned short port, const char *token,
    struct stat *st)
{
	char tmp[PATH_MAX];
	FILE *fp;
	int fd;

	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.new", path) >= sizeof(tmp))
		return -1;
	if (mkpath(path) != 0)
		return -1;
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600)) == -1)
		return -1;
	if ((fp = fdopen(fd, "w")) == NULL) {
		(void)close(fd);
		return -1;
	}
	if (fprintf(fp, "%u\n%s\n", port, token) < 0 || fclose(fp) != 0) {
		(void)unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) == -1) {
		(void)unlink(tmp);
		return -1;
	}
	if (st != NULL && stat(path, st) == -1)
		return -1;

	return 0;
}

/*
 * Remove the endpoint file, but only while it is still the one this process
 * published. A second daemon overwrites it with its own port and token, and
 * unlinking unconditionally on the way out would delete the newcomer's, so
 * every game would then fail to find a daemon that is running perfectly
 * well.
 */
static void
unlink_endpoint(const char *path, const struct stat *mine)
{
	struct stat now;

	if (stat(path, &now) == -1)
		return;
	if (now.st_dev != mine->st_dev || now.st_ino != mine->st_ino)
		return;

	(void)unlink(path);
}

static int
listen_loopback(unsigned short *port)
{
	struct sockaddr_in sa;
	socklen_t salen = sizeof(sa);
	int fd, on = 1;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
		return -1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == -1 ||
	    listen(fd, 1) == -1 ||
	    getsockname(fd, (struct sockaddr *)&sa, &salen) == -1) {
		(void)close(fd);
		return -1;
	}

	*port = ntohs(sa.sin_port);

	return fd;
}

/*
 * A reply is at most a header and eight bytes, so a peer whose receive
 * window has not opened within a second is not reading at all. Without this
 * the write below blocks forever, and it blocks the single loop that also
 * runs the watchdog, so any local process that connected and then stopped
 * reading could leave the wheel holding a force indefinitely.
 */
static void
set_send_timeout(int fd)
{
	struct timeval tv;

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int
send_reply(int fd, const struct t150_reply *rep)
{
	uint8_t buf[T150_PROTO_HDR_LEN + sizeof(rep->payload)];
	struct t150_proto_hdr hdr;
	size_t n, off = 0;

	hdr.magic = T150_PROTO_MAGIC;
	hdr.version = T150_PROTO_VERSION;
	hdr.op = rep->op;
	hdr.length = (uint16_t)rep->len;

	if ((n = t150_proto_pack_hdr(buf, sizeof(buf), &hdr)) == 0)
		return -1;
	memcpy(buf + n, rep->payload, rep->len);
	n += rep->len;

	while (off < n) {
		ssize_t w = write(fd, buf + off, n - off);

		if (w == -1) {
			if (errno == EINTR)
				continue;
			/* EAGAIN here is the send timeout above expiring. */
			return -1;
		}
		off += (size_t)w;
	}

	return 0;
}

/*
 * Consume whole frames from the read buffer. Returns the number of bytes
 * used, or -1 when the connection has to go: a desynchronised stream cannot
 * be recovered by guessing where the next header starts.
 */
static ssize_t
consume(struct t150_session *s, int fd, uint8_t *buf, size_t have, int *done)
{
	size_t off = 0;

	while (have - off >= T150_PROTO_HDR_LEN) {
		struct t150_proto_hdr hdr;
		struct t150_reply rep;
		size_t total;

		if (t150_proto_unpack_hdr(buf + off, have - off, &hdr) != 0) {
			rep.op = T150_OP_ERROR;
			rep.payload[0] = T150_ERR_BAD_FRAME;
			rep.payload[1] = 0;
			rep.len = 2;
			(void)send_reply(fd, &rep);
			return -1;
		}

		total = T150_PROTO_HDR_LEN + hdr.length;
		if (have - off < total)
			break;

		if (hdr.version != T150_PROTO_VERSION) {
			rep.op = T150_OP_ERROR;
			rep.payload[0] = T150_ERR_BAD_VERSION;
			rep.payload[1] = 0;
			rep.len = 2;
			(void)send_reply(fd, &rep);
			return -1;
		}

		if (t150_session_frame(s, hdr.op, buf + off + T150_PROTO_HDR_LEN,
		    hdr.length, now_ms(), &rep) != 0)
			*done = 1;

		if (send_reply(fd, &rep) != 0)
			return -1;

		off += total;
		if (*done)
			break;
	}

	return (ssize_t)off;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: t150d [-v] [-e endpoint]\n"
	    "\n"
	    "  -e endpoint  where to publish the port and token\n"
	    "               (default $HOME%s)\n"
	    "  -v           log effects, downgrades and safe states\n",
	    ENDPOINT_REL);
	exit(2);
}

int
main(int argc, char *argv[])
{
	char endpoint[PATH_MAX], token[T150_TOKEN_LEN + 1];
	struct stat epstat;
	struct t150_backend be;
	struct t150_session sess;
	struct pollfd pfd[2];
	const char *epopt = NULL, *home;
	uint8_t rx[RXBUF];
	size_t have = 0;
	unsigned short port;
	int ch, lfd, cfd = -1, verbose = 0;

	while ((ch = getopt(argc, argv, "e:v")) != -1) {
		switch (ch) {
		case 'e':
			epopt = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
		}
	}
	if (optind != argc)
		usage();

	if (epopt != NULL) {
		if ((size_t)snprintf(endpoint, sizeof(endpoint), "%s", epopt) >=
		    sizeof(endpoint))
			errx(1, "endpoint path is too long");
	} else {
		if ((home = getenv("HOME")) == NULL)
			errx(1, "HOME is not set, use -e");
		if ((size_t)snprintf(endpoint, sizeof(endpoint), "%s%s", home,
		    ENDPOINT_REL) >= sizeof(endpoint))
			errx(1, "endpoint path is too long");
	}

	if (make_token(token, sizeof(token)) != 0)
		err(1, "cannot generate a token");
	if (t150_backend_fake(&be, stdout) != 0)
		errx(1, "cannot open the logging backend");
	if ((lfd = listen_loopback(&port)) == -1)
		err(1, "cannot listen on loopback");
	if (write_endpoint(endpoint, port, token, &epstat) != 0)
		err(1, "cannot write %s", endpoint);

	t150_session_init(&sess, &be, token);
	sess.verbose = verbose;

	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);

	printf("t150d: listening on 127.0.0.1:%u, endpoint %s\n", port, endpoint);
	printf("t150d: backend %s, no wheel is being driven\n", be.name);
	(void)fflush(stdout);

	while (!quit) {
		unsigned int wait_ms = t150_session_tick(&sess, now_ms());
		int nfd = 0, n;

		pfd[nfd].fd = lfd;
		pfd[nfd].events = POLLIN;
		nfd++;
		if (cfd != -1) {
			pfd[nfd].fd = cfd;
			pfd[nfd].events = POLLIN;
			nfd++;
		}

		n = poll(pfd, (nfds_t)nfd, (int)wait_ms);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			err(1, "poll");
		}
		if (n == 0)
			continue;

		if (cfd != -1 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t r = read(cfd, rx + have, sizeof(rx) - have);
			int done = 0;

			if (r > 0) {
				ssize_t used;

				have += (size_t)r;
				used = consume(&sess, cfd, rx, have, &done);
				if (used < 0) {
					done = 1;
				} else {
					have -= (size_t)used;
					memmove(rx, rx + used, have);
					if (have == sizeof(rx))
						done = 1;
				}
			} else if (r == 0 || (r == -1 && errno != EINTR)) {
				done = 1;
			}

			if (done) {
				t150_session_panic(&sess, "client went away");
				sess.hello = 0;
				(void)close(cfd);
				cfd = -1;
				have = 0;
			}
		}

		if (pfd[0].revents & POLLIN) {
			int nfd2 = accept(lfd, NULL, NULL);

			if (nfd2 == -1)
				continue;
			if (cfd != -1) {
				/* Safe state first, then hand the wheel over. */
				t150_session_panic(&sess, "displaced by a new client");
				(void)close(cfd);
			}
			set_send_timeout(nfd2);
			t150_session_init(&sess, &be, token);
			sess.verbose = verbose;
			cfd = nfd2;
			have = 0;
			if (verbose)
				fprintf(stderr, "t150d: client connected\n");
		}
	}

	t150_session_panic(&sess, "shutting down");
	if (cfd != -1)
		(void)close(cfd);
	(void)close(lfd);
	unlink_endpoint(endpoint, &epstat);

	return 0;
}
