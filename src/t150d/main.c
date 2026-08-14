/*
 * t150d - the macOS side of the force feedback bridge.
 *
 * Listens on loopback for the proxy DLL running inside CrossOver, turns the
 * effects it sends into wheel packets, and writes them out through a
 * backend. Until the macOS HID backend exists the only backend is the
 * logging one, so this drives nothing yet and says so at startup.
 *
 * One client at a time. A second connection displaces the first, but only
 * after it has proved the token and only after the first has been put back
 * into a safe state, because the common case for a second connection is a
 * game that crashed and was restarted. Requiring the token first matters:
 * anything that can reach loopback can connect, and displacing on the
 * connection alone let any local process kill a game's force feedback at
 * will.
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
#include "t150/t150.h"
#include "t150d.h"

#define RXBUF	1024
#define ENDPOINT_REL	"/Library/Application Support/t150ffb/endpoint"

/*
 * How long a newcomer has to prove the token before it is dropped. It only
 * has to send one frame, so this is generous, and it bounds how long a
 * process that connects and says nothing can occupy the pending slot.
 */
#define PEND_MS	2000

/*
 * How long to leave the listening socket out of the poll set after an accept
 * failed for want of a resource. Long enough that a descriptor shortage does
 * not become a busy loop, short enough that a game waiting to connect does
 * not notice.
 */
#define ACCEPT_COOL_MS	200

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
 * One daemon per endpoint, held for the life of the process.
 *
 * Two daemons on one wheel is not a configuration that degrades: the second
 * one acquires it, which scrubs every slot and closes and reopens the input,
 * so a game already being driven by the first loses its effects. The second
 * also publishes its own port over the first's, and unlinks it again on the
 * way out, leaving the first running perfectly well with nothing able to find
 * it. There are two first class ways to get here, the login agent and the
 * menu bar item's own child, and neither could see the other.
 *
 * A lock on a file beside the endpoint rather than a check of the endpoint
 * itself, because the question is whether a process is alive and only the
 * kernel can answer that without a race. It is released when the process ends,
 * however it ends. Running two daemons on purpose still works: the lock is per
 * endpoint, so -e gives each its own.
 *
 * Returns the descriptor, which must stay open, or -1 if somebody has it.
 */
static int
lock_endpoint(const char *path)
{
	char lockpath[PATH_MAX];
	struct flock fl;
	int fd;

	if ((size_t)snprintf(lockpath, sizeof(lockpath), "%s.lock", path) >=
	    sizeof(lockpath))
		return -1;
	if (mkpath(lockpath) != 0)
		return -1;
	if ((fd = open(lockpath, O_WRONLY | O_CREAT, 0600)) == -1)
		return -1;

	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	if (fcntl(fd, F_SETLK, &fl) == -1) {
		(void)close(fd);
		return -1;
	}

	return fd;
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
 * A pending client may send exactly one kind of frame: HELLO with the token.
 * Returns 1 when it has proved itself, 0 when the frame has not fully
 * arrived, and -1 when it should be dropped.
 *
 * The only thing this reaches the wheel with is the input open, which the
 * promoted session needs held from the moment it takes over. Its device
 * settings wait for the caller to make the incumbent safe first: see the
 * pending flag in struct t150_session. A wrong token is answered and dropped
 * having written nothing at all.
 */
static int
pend_hello(struct t150_session *ps, int fd, uint8_t *buf, size_t *have)
{
	struct t150_proto_hdr hdr;
	struct t150_reply rep;
	size_t total;

	if (*have < T150_PROTO_HDR_LEN)
		return 0;
	if (t150_proto_unpack_hdr(buf, *have, &hdr) != 0)
		return -1;
	if (hdr.version != T150_PROTO_VERSION || hdr.op != T150_OP_HELLO)
		return -1;

	total = T150_PROTO_HDR_LEN + hdr.length;
	if (total > T150_PROTO_HDR_LEN + T150_PROTO_MAX_PAYLOAD)
		return -1;
	if (*have < total)
		return 0;

	memset(&rep, 0, sizeof(rep));
	(void)t150_session_frame(ps, hdr.op, buf + T150_PROTO_HDR_LEN,
	    hdr.length, now_ms(), &rep);
	(void)send_reply(fd, &rep);

	if (!ps->hello)
		return -1;

	/* Anything sent after the HELLO waits for the promoted session. */
	*have -= total;
	memmove(buf, buf + total, *have);

	return 1;
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

/* Degrees lock to lock, refusing anything the wheel will not take. */
static int
parse_range(const char *s, unsigned int *out)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' ||
	    v < T150_RANGE_MIN || v > T150_RANGE_MAX)
		return -1;
	*out = (unsigned int)v;

	return 0;
}

/*
 * The wheel's own centring spring, in the same 0 to 10000 a game would use
 * for a force. Zero releases it, which is the default and what a game wants.
 */
static int
parse_level(const char *s, unsigned int *out)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || v > T150_DI_MAX)
		return -1;
	*out = (unsigned int)v;

	return 0;
}

/* Milliseconds, refusing anything that is not a number or is absurd. */
static int
parse_ms(const char *s, unsigned int *out)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || v > 50)
		return -1;
	*out = (unsigned int)v;

	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: t150d [-ntvw] [-a force] [-e endpoint] [-g ms] [-r degrees]\n"
	    "\n"
	    "  -a force     leave the wheel's own centring spring at this, 0 to\n"
	    "               %u, on every acquire. 0 releases it and is the\n"
	    "               default: a game's own forces do the centring and\n"
	    "               the firmware's spring only fights them. For a game\n"
	    "               that sends no forces at all, which cannot ask for\n"
	    "               this itself, a person can\n"
	    "  -e endpoint  where to publish the port and token\n"
	    "               (default $HOME%s)\n"
	    "  -g ms        pause this long after each packet, 0 to 50. Off\n"
	    "               by default, see hid_darwin.c. A safe state is up\n"
	    "               to eighteen packets, so this delays it\n"
	    "  -n           drive nothing, log the packets instead\n"
	    "  -r degrees   lock to lock, %u to %u, set whenever a client takes\n"
	    "               the wheel. No game can ask for this: DirectInput has\n"
	    "               no property for it. Unset leaves the wheel's own\n"
	    "  -t           re-send an effect's three packets whenever any of\n"
	    "               them changes, as builds before this one did. The\n"
	    "               default sends only the packet that moved, which is\n"
	    "               what the vendor's own driver does and is a third of\n"
	    "               the writes. Here to compare the two by feel\n"
	    "  -v           log effects, downgrades and safe states\n"
	    "  -w           write to the wheel from a thread of its own, so a\n"
	    "               game is never waiting for a USB transfer. macOS\n"
	    "               only, and new: compare it against a run without\n",
	    T150_DI_MAX, ENDPOINT_REL, T150_RANGE_MIN, T150_RANGE_MAX);
	exit(2);
}

int
main(int argc, char *argv[])
{
	char endpoint[PATH_MAX], token[T150_TOKEN_LEN + 1];
	struct stat epstat;
	struct t150_backend be;
	struct t150_session sess, psess;
	struct pollfd pfd[3];
	const char *epopt = NULL, *home;
	uint8_t rx[RXBUF], prx[RXBUF];
	uint64_t pend_deadline = 0, accept_cool_ms = 0;
	size_t have = 0, phave = 0;
	unsigned short port;
	unsigned int gap_ms = 0, range_deg = 0, autocenter = 0;
	int always_triple = 0, writer = 0;
	int ch, lfd, cfd = -1, pfd_pend = -1, verbose = 0, fake = 0;

	while ((ch = getopt(argc, argv, "a:e:g:nr:tvw")) != -1) {
		switch (ch) {
		case 'a':
			if (parse_level(optarg, &autocenter) != 0)
				usage();
			break;
		case 'e':
			epopt = optarg;
			break;
		case 'g':
			if (parse_ms(optarg, &gap_ms) != 0)
				usage();
			break;
		case 'n':
			fake = 1;
			break;
		case 'r':
			if (parse_range(optarg, &range_deg) != 0)
				usage();
			break;
		case 't':
			always_triple = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'w':
			writer = 1;
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

	/*
	 * Zeroed before either backend fills it in, so an optional hook a
	 * backend does not set is NULL rather than whatever was on the stack.
	 */
	memset(&be, 0, sizeof(be));

	if (make_token(token, sizeof(token)) != 0)
		err(1, "cannot generate a token");
#ifdef __APPLE__
	if (!fake) {
		if (t150_backend_hid(&be, T150_VID, T150_PID_FIRMWARE, gap_ms,
		    verbose, writer, autocenter) != 0)
			errx(1, "cannot open the wheel backend");
	} else if (t150_backend_fake(&be, stdout) != 0) {
		errx(1, "cannot open the logging backend");
	}
#else
	/* Nowhere else has a wheel backend, so -n is the only behaviour. */
	if (!fake)
		fprintf(stderr, "t150d: this build drives no wheel, "
		    "logging instead\n");
	if (t150_backend_fake(&be, stdout) != 0)
		errx(1, "cannot open the logging backend");
	(void)gap_ms;
	(void)writer;
#endif
	if (lock_endpoint(endpoint) == -1)
		errx(1, "another t150d already has %s. Stop it first, or "
		    "give this one its own with -e", endpoint);
	if ((lfd = listen_loopback(&port)) == -1)
		err(1, "cannot listen on loopback");
	if (write_endpoint(endpoint, port, token, &epstat) != 0)
		err(1, "cannot write %s", endpoint);

	t150_session_init(&sess, &be, token);
	sess.verbose = verbose;
	sess.range_deg = range_deg;
	sess.autocenter = autocenter;
	sess.always_triple = always_triple;

	/*
	 * Every signal that would otherwise kill the process outright, so the
	 * loop can exit through the safe state at the bottom. Dying without
	 * it leaves the wheel holding whatever force it was last given, with
	 * its input still open so it does not even fall back to its own
	 * autocenter. SIGHUP matters most: the documented way to run this is
	 * in a terminal, and closing that terminal sends one.
	 */
	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGHUP, on_signal);
	(void)signal(SIGQUIT, on_signal);

	printf("t150d: listening on 127.0.0.1:%u, endpoint %s\n", port, endpoint);
	printf("t150d: backend %s\n", be.name);
	(void)fflush(stdout);

	while (!quit) {
		unsigned int wait_ms;

		/*
		 * Before the session, because a backend that has just found
		 * the wheel again should be able to take this round's writes.
		 */
		if (be.tick != NULL)
			be.tick(be.priv, now_ms());

		wait_ms = t150_session_tick(&sess, now_ms());
		int nfd = 0, n, ilisten, iclient = -1, ipend = -1;

		ilisten = -1;
		if (now_ms() >= accept_cool_ms) {
			ilisten = nfd;
			pfd[nfd].fd = lfd;
			pfd[nfd].events = POLLIN;
			nfd++;
		} else if (wait_ms > ACCEPT_COOL_MS) {
			wait_ms = ACCEPT_COOL_MS;
		}
		if (cfd != -1) {
			iclient = nfd;
			pfd[nfd].fd = cfd;
			pfd[nfd].events = POLLIN;
			nfd++;
		}
		if (pfd_pend != -1) {
			uint64_t left;

			ipend = nfd;
			pfd[nfd].fd = pfd_pend;
			pfd[nfd].events = POLLIN;
			nfd++;
			/* Do not sleep past a pending client's deadline. */
			left = pend_deadline > now_ms() ?
			    pend_deadline - now_ms() : 0;
			if ((uint64_t)wait_ms > left)
				wait_ms = (unsigned int)left;
		}

		n = poll(pfd, (nfds_t)nfd, (int)wait_ms);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			err(1, "poll");
		}

		/* A newcomer that never proves the token does not linger. */
		if (pfd_pend != -1 && now_ms() >= pend_deadline) {
			(void)close(pfd_pend);
			pfd_pend = -1;
			phave = 0;
		}
		if (n == 0)
			continue;

		if (iclient != -1 &&
		    (pfd[iclient].revents & (POLLIN | POLLHUP | POLLERR))) {
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
				t150_session_end(&sess, "client went away");
				(void)close(cfd);
				cfd = -1;
				have = 0;
			}
		}

		/*
		 * A pending client is allowed exactly one thing: prove the
		 * token. Anything else and it is dropped, so it can neither
		 * reach the wheel nor displace whoever holds it.
		 */
		if (ipend != -1 && pfd_pend != -1 &&
		    (pfd[ipend].revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t r = read(pfd_pend, prx + phave,
			    sizeof(prx) - phave);
			int drop = 1, hi = -1;

			if (r > 0) {
				phave += (size_t)r;
				hi = pend_hello(&psess, pfd_pend, prx, &phave);
				/* 0 means the frame is still arriving, and
				 * only a full buffer ends that patience. */
				if (hi == 0 && phave < sizeof(prx))
					drop = 0;
			}

			if (hi > 0) {
				if (verbose)
					fprintf(stderr, "t150d: a new client "
					    "proved the token, displacing the "
					    "old one\n");
				/*
				 * Panic, not end. The newcomer has already
				 * opened the wheel's input as part of
				 * proving its token, so closing it here
				 * would undo that and hand the replacement
				 * a wheel that renders nothing. This is a
				 * handover: the input stays open, only the
				 * effects go.
				 */
				t150_session_panic(&sess,
				    "displaced by a new client");
				(void)close(cfd);
				sess = psess;
				sess.verbose = verbose;
				cfd = pfd_pend;
				/* Carry anything it sent after the HELLO. */
				memcpy(rx, prx, phave);
				have = phave;
				pfd_pend = -1;
				phave = 0;
				drop = 0;

				/*
				 * Act on it now. Waiting for the next read
				 * would stall a client that sent its whole
				 * opening burst at once and then listened.
				 */
				if (have > 0) {
					int done = 0;
					ssize_t used = consume(&sess, cfd, rx,
					    have, &done);

					if (used < 0 || done) {
						t150_session_end(&sess,
						    "client went away");
						(void)close(cfd);
						cfd = -1;
						have = 0;
					} else {
						have -= (size_t)used;
						memmove(rx, rx + used, have);
					}
				}
			}

			if (drop && pfd_pend != -1) {
				(void)close(pfd_pend);
				pfd_pend = -1;
				phave = 0;
			}
		}

		if (ilisten != -1 && (pfd[ilisten].revents & POLLIN)) {
			struct sockaddr_in peer;
			socklen_t plen = sizeof(peer);
			int nfd2 = accept(lfd, (struct sockaddr *)&peer, &plen);
			unsigned peer_port;

			/*
			 * poll is level triggered on the listening socket, so
			 * a failure that leaves the connection in the backlog
			 * comes straight back: running out of descriptors used
			 * to free-run this loop at full tilt for as long as the
			 * shortage lasted. ECONNABORTED dequeues and is
			 * harmless; EINTR and EAGAIN carry nothing to wait for.
			 * The rest are resource exhaustion, and the only thing
			 * to do about those is stop asking for a moment.
			 */
			if (nfd2 == -1) {
				switch (errno) {
				case EINTR:
				case EAGAIN:
				case ECONNABORTED:
					break;
				default:
					accept_cool_ms = now_ms() + ACCEPT_COOL_MS;
					if (verbose)
						warn("accept");
					break;
				}
				continue;
			}
			set_send_timeout(nfd2);

			/*
			 * The port is what tells two connections apart in the
			 * log. Test 27 came back with two connect and
			 * disconnect pairs for one run of one program, and
			 * nothing in the log could say whether that was the
			 * program twice or something else once.
			 */
			peer_port = plen >= sizeof(peer) ?
			    (unsigned)ntohs(peer.sin_port) : 0;

			if (cfd == -1) {
				/*
				 * Nobody to displace, so it takes the slot
				 * straight away. It still has to say HELLO
				 * before the session will do anything.
				 */
				t150_session_init(&sess, &be, token);
				sess.verbose = verbose;
				sess.range_deg = range_deg;
				sess.autocenter = autocenter;
				sess.always_triple = always_triple;
				sess.peer_port = peer_port;
				cfd = nfd2;
				have = 0;
				if (verbose)
					fprintf(stderr, "t150d: client "
					    "connected from port %u\n", peer_port);
			} else if (pfd_pend == -1) {
				pfd_pend = nfd2;
				phave = 0;
				pend_deadline = now_ms() + PEND_MS;
				t150_session_init(&psess, &be, token);
				psess.pending = 1;
				psess.verbose = verbose;
				psess.range_deg = range_deg;
				psess.autocenter = autocenter;
				psess.always_triple = always_triple;
				psess.peer_port = peer_port;
				if (verbose)
					fprintf(stderr, "t150d: second client "
					    "from port %u, waiting for the "
					    "first to go\n", peer_port);
			} else {
				if (verbose)
					fprintf(stderr, "t150d: turned away a "
					    "client from port %u, two are "
					    "already here\n", peer_port);
				(void)close(nfd2);
			}
		}
	}


	t150_session_shutdown(&sess, "shutting down");
	if (cfd != -1)
		(void)close(cfd);
	if (pfd_pend != -1)
		(void)close(pfd_pend);
	(void)close(lfd);
	unlink_endpoint(endpoint, &epstat);
	if (be.close != NULL)
		be.close(be.priv);

	return 0;
}
