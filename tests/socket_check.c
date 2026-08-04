/*
 * socket_check - drive the real daemon over a real socket.
 *
 * daemon_check proves the session picks the right packets. This proves the
 * rest of the daemon: that it publishes an endpoint the way the DLL will
 * read it, that framing survives a socket, and above all that a client which
 * goes quiet gets the wheel released without having to say so.
 *
 * It runs build/bin/t150d as a child with its output on a pipe, so it needs
 * to be started from the top of the tree, which is where make runs it.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <netinet/in.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "t150/proto.h"

#define DAEMON		"build/bin/t150d"
#define ENDPOINT_DIR	"tmp"
#define ENDPOINT	"tmp/socket_check.endpoint"

#define STARTUP_MS	5000
#define OUTPUT_MS	4000
#define LOGMAX		8192

static int failures;
static char logbuf[LOGMAX];
static size_t loghave;

static void
fail(const char *what)
{
	fprintf(stderr, "FAIL %s\n", what);
	failures++;
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

/*
 * Wait for the daemon to publish its endpoint. It is written to a temporary
 * and renamed, so anything readable here is complete.
 */
static int
read_endpoint(unsigned short *port, char *token, size_t tokenlen)
{
	uint64_t deadline = now_ms() + STARTUP_MS;

	while (now_ms() < deadline) {
		struct timespec nap = { 0, 10 * 1000 * 1000 };
		char line[64];
		unsigned long p;
		FILE *fp;

		if ((fp = fopen(ENDPOINT, "r")) != NULL) {
			int ok = 0;

			/*
			 * Read by line rather than with a scanf width, which
			 * would have to repeat T150_TOKEN_LEN as a literal
			 * and silently truncate if the two ever disagreed.
			 */
			if (fgets(line, sizeof(line), fp) != NULL) {
				p = strtoul(line, NULL, 10);
				if (p > 0 && p <= 0xffff &&
				    fgets(token, (int)tokenlen, fp) != NULL) {
					token[strcspn(token, "\r\n")] = '\0';
					if (strlen(token) == T150_TOKEN_LEN) {
						*port = (unsigned short)p;
						ok = 1;
					}
				}
			}
			(void)fclose(fp);
			if (ok)
				return 0;
		}
		(void)nanosleep(&nap, NULL);
	}

	return -1;
}

static int
connect_to(unsigned short port)
{
	struct sockaddr_in sa;
	int fd;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
		return -1;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons(port);

	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
		(void)close(fd);
		return -1;
	}

	return fd;
}

static int
send_frame(int fd, uint8_t op, const uint8_t *payload, size_t len)
{
	uint8_t buf[T150_PROTO_HDR_LEN + T150_PROTO_MAX_PAYLOAD];
	struct t150_proto_hdr hdr;
	size_t n;

	hdr.magic = T150_PROTO_MAGIC;
	hdr.version = T150_PROTO_VERSION;
	hdr.op = op;
	hdr.length = (uint16_t)len;

	if ((n = t150_proto_pack_hdr(buf, sizeof(buf), &hdr)) == 0)
		return -1;
	if (len > 0)
		memcpy(buf + n, payload, len);

	return write(fd, buf, n + len) == (ssize_t)(n + len) ? 0 : -1;
}

/* Read one reply and check it is the OK the daemon owes us. */
static void
expect_ok(int fd, const char *what)
{
	uint8_t buf[T150_PROTO_HDR_LEN];
	struct t150_proto_hdr hdr;
	struct pollfd pfd;
	ssize_t r;

	pfd.fd = fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, OUTPUT_MS) != 1) {
		fail(what);
		return;
	}
	if ((r = read(fd, buf, sizeof(buf))) != (ssize_t)sizeof(buf)) {
		fail(what);
		return;
	}
	if (t150_proto_unpack_hdr(buf, (size_t)r, &hdr) != 0 ||
	    hdr.op != T150_OP_OK)
		fail(what);
}

/*
 * Collect the daemon's output until it contains want, or until the deadline.
 * Everything read stays in the buffer, so the caller can go on looking for
 * the next thing without losing what already arrived.
 */
static int
wait_for_after(int fd, const char *want, size_t from)
{
	uint64_t deadline = now_ms() + OUTPUT_MS;

	for (;;) {
		struct pollfd pfd;
		uint64_t left;
		ssize_t r;

		/*
		 * From an offset, because the daemon repeats itself: waiting
		 * for a packet the log already contains from an earlier step
		 * returns at once without reading anything, and a test built
		 * on that measures nothing.
		 */
		if (from <= loghave && strstr(logbuf + from, want) != NULL)
			return 0;
		if (loghave + 1 >= sizeof(logbuf))
			return -1;

		left = now_ms() < deadline ? deadline - now_ms() : 0;
		if (left == 0)
			return -1;

		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, (int)left) != 1)
			return -1;

		r = read(fd, logbuf + loghave, sizeof(logbuf) - loghave - 1);
		if (r <= 0)
			return -1;
		loghave += (size_t)r;
		logbuf[loghave] = '\0';
	}
}

static int
wait_for(int fd, const char *want)
{
	return wait_for_after(fd, want, 0);
}

static void
put_u32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v & 0xff);
	b[1] = (uint8_t)((v >> 8) & 0xff);
	b[2] = (uint8_t)((v >> 16) & 0xff);
	b[3] = (uint8_t)((v >> 24) & 0xff);
}

int
main(void)
{
	char token[T150_TOKEN_LEN + 1];
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	struct t150_effect ef;
	unsigned short port;
	int pipefd[2], fd, status;
	pid_t pid;

	(void)mkdir(ENDPOINT_DIR, 0700);
	(void)unlink(ENDPOINT);

	if (pipe(pipefd) == -1) {
		perror("pipe");
		return 1;
	}

	if ((pid = fork()) == -1) {
		perror("fork");
		return 1;
	}
	if (pid == 0) {
		(void)close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) == -1)
			_exit(127);
		(void)close(pipefd[1]);
		/*
		 * -n, because this test reads the packets out of the daemon's
		 * log. On macOS the daemon defaults to driving a real wheel
		 * and prints nothing, so without this every expectation below
		 * fails on a Mac and passes everywhere else.
		 */
		execl(DAEMON, "t150d", "-n", "-e", ENDPOINT, (char *)NULL);
		_exit(127);
	}
	(void)close(pipefd[1]);

	if (read_endpoint(&port, token, sizeof(token)) != 0) {
		fail("the daemon did not publish an endpoint");
		goto out;
	}
	if ((fd = connect_to(port)) == -1) {
		fail("cannot connect to the daemon");
		goto out;
	}

	/* A wrong token is refused before anything can reach the wheel. */
	if (send_frame(fd, T150_OP_HELLO, (const uint8_t *)
	    "ffffffffffffffffffffffffffffffff", T150_TOKEN_LEN) != 0)
		fail("cannot send");
	else {
		uint8_t rb[T150_PROTO_HDR_LEN + 2];
		struct t150_proto_hdr hdr;
		struct pollfd pfd;

		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, OUTPUT_MS) != 1 ||
		    read(fd, rb, sizeof(rb)) != (ssize_t)sizeof(rb) ||
		    t150_proto_unpack_hdr(rb, sizeof(rb), &hdr) != 0 ||
		    hdr.op != T150_OP_ERROR ||
		    rb[T150_PROTO_HDR_LEN] != T150_ERR_BAD_TOKEN)
			fail("a wrong token was not refused");
	}

	if (send_frame(fd, T150_OP_HELLO, (const uint8_t *)token,
	    T150_TOKEN_LEN) != 0)
		fail("cannot send the token");
	expect_ok(fd, "the token was not accepted");

	put_u32(buf, 5000);
	if (send_frame(fd, T150_OP_SET_GAIN, buf, 4) != 0)
		fail("cannot set the gain");
	expect_ok(fd, "the gain was refused");
	if (wait_for(pipefd[0], "write 2: 43 40\n") != 0)
		fail("the gain packet did not reach the backend");

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_CONSTANT;
	ef.slot = 0;
	ef.duration = T150_DURATION_INFINITE;
	ef.direction = 9000;
	ef.gain = T150_DI_MAX;
	ef.u.constant.magnitude = 10000;

	if (send_frame(fd, T150_OP_EFFECT_UPLOAD, buf,
	    t150_proto_pack_effect(buf, sizeof(buf), &ef)) != 0)
		fail("cannot upload");
	expect_ok(fd, "the upload was refused");
	if (wait_for(pipefd[0],
	    "write 15: 01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00\n") != 0)
		fail("the upload did not reach the backend");

	buf[0] = 0;
	buf[1] = 1;
	if (send_frame(fd, T150_OP_EFFECT_START, buf, 2) != 0)
		fail("cannot start");
	expect_ok(fd, "the start was refused");
	if (wait_for(pipefd[0], "write 4: 41 00 41 01\n") != 0)
		fail("the start did not reach the backend");

	/*
	 * Now go quiet while holding the socket open, which is what a frozen
	 * or crashed game looks like from here. The wheel is holding a full
	 * constant force, and nothing will tell the daemon otherwise, so the
	 * watchdog has to notice by itself.
	 */
	if (wait_for(pipefd[0], "write 4: 41 00 00 01\n") != 0)
		fail("the watchdog did not stop the effect");
	if (wait_for(pipefd[0], "write 4: 40 03 00 00\n"
	    "write 4: 40 04 00 00\n") != 0)
		fail("the watchdog did not release the wheel");

	/*
	 * A second connection that never proves the token must not take the
	 * wheel. Anything on this machine can reach loopback, and displacing
	 * on the connection alone let any local process kill a game's force
	 * feedback whenever it liked.
	 */
	{
		int bad = connect_to(port);
		uint8_t g[4];
		struct timespec nap = { 0, 200 * 1000 * 1000 };

		if (bad == -1)
			fail("cannot open a second connection");

		/*
		 * Give the daemon a poll cycle to accept it, or the checks
		 * below race the accept and pass whatever the daemon does.
		 */
		(void)nanosleep(&nap, NULL);

		/* Say nothing at all, then check the first client still works. */
		put_u32(g, 5000);
		if (send_frame(fd, T150_OP_SET_GAIN, g, 4) != 0)
			fail("the original client lost its socket");
		expect_ok(fd, "the original client was displaced by a silent peer");
		if (wait_for(pipefd[0], "write 2: 43 40\n") != 0)
			fail("the original client's gain did not reach the wheel");

		/* A wrong token is refused and buys nothing either. */
		if (send_frame(bad, T150_OP_HELLO, (const uint8_t *)
		    "ffffffffffffffffffffffffffffffff", T150_TOKEN_LEN) != 0)
			fail("cannot send a bad token");
		put_u32(g, 10000);
		if (send_frame(fd, T150_OP_SET_GAIN, g, 4) != 0)
			fail("the original client lost its socket");
		expect_ok(fd, "a bad token displaced the original client");
		if (wait_for(pipefd[0], "write 2: 43 80\n") != 0)
			fail("the original client stopped reaching the wheel");

		(void)close(bad);
	}

	/*
	 * A newcomer with the right token does take over, and the wheel's
	 * input has to survive the handover. The newcomer opens it as part of
	 * proving itself, so a daemon that then closed the outgoing session's
	 * input would hand the replacement a wheel that renders nothing.
	 */
	{
		struct timespec settle = { 0, 300 * 1000 * 1000 };
		int good;
		size_t mark;

		/*
		 * Only one newcomer may be pending at a time, and the one
		 * above has only just been closed. Let the daemon see that
		 * before connecting, or this one is refused the slot and the
		 * handover never happens.
		 */
		(void)nanosleep(&settle, NULL);

		if ((good = connect_to(port)) == -1)
			fail("cannot open a third connection");

		mark = strlen(logbuf);
		if (send_frame(good, T150_OP_HELLO, (const uint8_t *)token,
		    T150_TOKEN_LEN) != 0)
			fail("cannot send the token on the new connection");
		expect_ok(good, "the newcomer's token was refused");

		if (wait_for_after(pipefd[0], "write 2: 42 04\n", mark) != 0)
			fail("the newcomer did not open the wheel's input");

		/*
		 * Drive something through the new session before judging the
		 * input. Its packet lands after everything the handover
		 * emits, so reaching it means any stray close has arrived
		 * too and the absence below is real rather than early.
		 */
		put_u32(buf, 10000);
		if (send_frame(good, T150_OP_SET_GAIN, buf, 4) != 0)
			fail("the newcomer lost its socket");
		expect_ok(good, "the newcomer could not set the gain");
		if (wait_for_after(pipefd[0], "write 2: 43 80\n", mark) != 0)
			fail("the newcomer's gain did not reach the wheel");

		if (strstr(logbuf + mark, "write 2: 42 00\n") != NULL)
			fail("the handover closed the wheel's input");

		(void)close(good);
	}

	(void)close(fd);

out:
	(void)kill(pid, SIGTERM);
	(void)waitpid(pid, &status, 0);
	(void)close(pipefd[0]);
	(void)unlink(ENDPOINT);

	if (failures != 0) {
		fprintf(stderr, "socket_check: %d failure(s)\n", failures);
		fprintf(stderr, "daemon said:\n%s", logbuf);
		return 1;
	}

	printf("socket_check: ok\n");
	return 0;
}
