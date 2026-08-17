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

/*
 * One per run, because t150d now refuses to start while another holds the
 * same endpoint. A run killed part way through leaves its daemon behind, and
 * with a fixed path that daemon blocked every later run until somebody found
 * and killed it.
 */
static char endpoint[64];

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

		if ((fp = fopen(endpoint, "r")) != NULL) {
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

/*
 * The same, delivered a byte at a time with a pause between, so consume() in
 * the daemon meets a header split across reads and a payload split from its
 * header. Every other frame here arrives whole in one write, which is the one
 * shape consume() never has to work for.
 */
static int
send_frame_dribbled(int fd, uint8_t op, const uint8_t *payload, size_t len)
{
	uint8_t buf[T150_PROTO_HDR_LEN + T150_PROTO_MAX_PAYLOAD];
	struct t150_proto_hdr hdr;
	size_t n, i;

	hdr.magic = T150_PROTO_MAGIC;
	hdr.version = T150_PROTO_VERSION;
	hdr.op = op;
	hdr.length = (uint16_t)len;

	if ((n = t150_proto_pack_hdr(buf, sizeof(buf), &hdr)) == 0)
		return -1;
	if (len > 0)
		memcpy(buf + n, payload, len);
	n += len;

	for (i = 0; i < n; i++) {
		struct timespec ts = { 0, 2 * 1000 * 1000 };

		if (write(fd, buf + i, 1) != 1)
			return -1;
		(void)nanosleep(&ts, NULL);
	}

	return 0;
}

/* Two whole frames in one write, which is the other half consume() must do. */
static int
send_two_frames(int fd, uint8_t a_op, const uint8_t *a, size_t alen,
    uint8_t b_op, const uint8_t *b, size_t blen)
{
	uint8_t buf[2 * (T150_PROTO_HDR_LEN + T150_PROTO_MAX_PAYLOAD)];
	struct t150_proto_hdr hdr;
	size_t n = 0, k;

	hdr.magic = T150_PROTO_MAGIC;
	hdr.version = T150_PROTO_VERSION;
	hdr.op = a_op;
	hdr.length = (uint16_t)alen;
	if ((k = t150_proto_pack_hdr(buf, sizeof(buf), &hdr)) == 0)
		return -1;
	n = k;
	if (alen > 0)
		memcpy(buf + n, a, alen);
	n += alen;

	hdr.op = b_op;
	hdr.length = (uint16_t)blen;
	if ((k = t150_proto_pack_hdr(buf + n, sizeof(buf) - n, &hdr)) == 0)
		return -1;
	n += k;
	if (blen > 0)
		memcpy(buf + n, b, blen);
	n += blen;

	return write(fd, buf, n) == (ssize_t)n ? 0 : -1;
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

/*
 * Pull everything the daemon has already written into logbuf, so that a
 * position taken afterwards really does separate the past from the future.
 * Without this an offset means nothing: unread bytes from an earlier step
 * land after it and are indistinguishable from what comes next.
 */
static void
drain(int fd)
{
	for (;;) {
		struct pollfd pfd;
		ssize_t r;

		if (loghave + 1 >= sizeof(logbuf))
			return;
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 100) != 1)
			return;
		if ((r = read(fd, logbuf + loghave, sizeof(logbuf) - loghave - 1)) <= 0)
			return;
		loghave += (size_t)r;
		logbuf[loghave] = '\0';
	}
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

	(void)snprintf(endpoint, sizeof(endpoint),
	    ENDPOINT_DIR "/socket_check.%ld.endpoint", (long)getpid());

	(void)mkdir(ENDPOINT_DIR, 0700);
	(void)unlink(endpoint);

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
		execl(DAEMON, "t150d", "-n", "-e", endpoint, (char *)NULL);
		_exit(127);
	}
	(void)close(pipefd[1]);

	if (read_endpoint(&port, token, sizeof(token)) != 0) {
		fail("the daemon did not publish an endpoint");
		goto out;
	}
	/*
	 * The version gate, before anything else takes the client slot.
	 *
	 * consume() answers T150_ERR_BAD_VERSION and drops the connection
	 * rather than parsing a frame under a layout it does not speak, and
	 * that is the whole of what protects an older proxy left in a bottle
	 * if the wire format ever changes. Nothing reached it: every frame in
	 * this file carries T150_PROTO_VERSION.
	 *
	 * On a connection of its own and first, because the answer is followed
	 * by a hangup, and because only the established client reaches
	 * consume() at all: a second connection is pending and pend_hello
	 * drops a wrong version without a word, which is its own rule.
	 */
	{
		uint8_t bad[T150_PROTO_HDR_LEN + 4];
		uint8_t rb[T150_PROTO_HDR_LEN + 2];
		struct t150_proto_hdr h;
		struct pollfd pfd;
		int vfd;

		if ((vfd = connect_to(port)) == -1) {
			fail("cannot open a connection for the version gate");
		} else {
			h.magic = T150_PROTO_MAGIC;
			h.version = T150_PROTO_VERSION + 1;
			h.op = T150_OP_SET_GAIN;
			h.length = 4;
			(void)t150_proto_pack_hdr(bad, sizeof(bad), &h);
			memset(bad + T150_PROTO_HDR_LEN, 0, 4);
			if (write(vfd, bad, sizeof(bad)) != (ssize_t)sizeof(bad))
				fail("cannot send a wrong version");

			pfd.fd = vfd;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, OUTPUT_MS) != 1 ||
			    read(vfd, rb, sizeof(rb)) != (ssize_t)sizeof(rb) ||
			    t150_proto_unpack_hdr(rb, sizeof(rb), &h) != 0 ||
			    h.op != T150_OP_ERROR ||
			    rb[T150_PROTO_HDR_LEN] != T150_ERR_BAD_VERSION)
				fail("a frame with the wrong version was not "
				    "refused with BAD_VERSION");
			(void)close(vfd);
			/* Let the daemon see the close before the real client. */
			{
				struct timespec ts = { 0, 300 * 1000 * 1000 };

				(void)nanosleep(&ts, NULL);
			}
		}
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
	 * consume() has to find frames in a byte stream, and every frame above
	 * arrived whole in one write, which is the one shape it never has to
	 * work for. These two are the others: a frame dribbled a byte at a
	 * time, so the header itself is split across reads, and two frames in
	 * one write, so the second is only reachable through the leftover
	 * carry that follows the first.
	 */
	{
		uint8_t g[4];

		put_u32(g, 7500);
		if (send_frame_dribbled(fd, T150_OP_SET_GAIN, g, 4) != 0)
			fail("cannot dribble a frame");
		expect_ok(fd, "a frame split across reads was refused");
		if (wait_for(pipefd[0], "write 2: 43 60\n") != 0)
			fail("a frame split across reads did not reach the wheel");

		put_u32(g, 2500);
		if (send_two_frames(fd, T150_OP_KEEPALIVE, NULL, 0,
		    T150_OP_SET_GAIN, g, 4) != 0)
			fail("cannot send two frames at once");
		expect_ok(fd, "the first of two frames in one write");
		expect_ok(fd, "the second of two frames in one write");
		if (wait_for(pipefd[0], "write 2: 43 20\n") != 0)
			fail("the second of two frames in one write was lost");
	}

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
		size_t mark;
		uint8_t g[4];
		struct timespec nap = { 0, 200 * 1000 * 1000 };

		if (bad == -1)
			fail("cannot open a second connection");

		/*
		 * Give the daemon a poll cycle to accept it, or the checks
		 * below race the accept and pass whatever the daemon does.
		 */
		(void)nanosleep(&nap, NULL);

		/*
		 * From here rather than from the start of the log. Both of
		 * these packets have already been written once by this run,
		 * the half gain by the first SET_GAIN and the full one by the
		 * hello, so searching from offset 0 returned at once without
		 * reading a byte and the two checks below asserted nothing.
		 * The file says so at wait_for_after, and these were the two
		 * places that fell into it.
		 */
		mark = loghave;

		/* Say nothing at all, then check the first client still works. */
		put_u32(g, 5000);
		if (send_frame(fd, T150_OP_SET_GAIN, g, 4) != 0)
			fail("the original client lost its socket");
		expect_ok(fd, "the original client was displaced by a silent peer");
		if (wait_for_after(pipefd[0], "write 2: 43 40\n", mark) != 0)
			fail("the original client's gain did not reach the wheel");

		/* A wrong token is refused and buys nothing either. */
		mark = loghave;
		if (send_frame(bad, T150_OP_HELLO, (const uint8_t *)
		    "ffffffffffffffffffffffffffffffff", T150_TOKEN_LEN) != 0)
			fail("cannot send a bad token");
		put_u32(g, 10000);
		if (send_frame(fd, T150_OP_SET_GAIN, g, 4) != 0)
			fail("the original client lost its socket");
		expect_ok(fd, "a bad token displaced the original client");
		if (wait_for_after(pipefd[0], "write 2: 43 80\n", mark) != 0)
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
		uint64_t went_ms;
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

		/*
		 * And a client that simply goes away leaves the wheel safe.
		 *
		 * main.c calls t150_session_end on the read that returns zero,
		 * and nothing here asserted it: daemon_check covers the
		 * function and cannot see the call site, so deleting the call
		 * from the poll loop passed the whole suite. A game that quits
		 * while an effect is playing would then hold its force for up
		 * to the watchdog instead of being released at once.
		 *
		 * Something has to be playing for the release to be visible,
		 * so this session gets an effect of its own first.
		 */
		mark = loghave;
		memset(&ef, 0, sizeof(ef));
		ef.kind = T150_EFFECT_CONSTANT;
		ef.slot = 1;
		ef.duration = T150_DURATION_INFINITE;
		ef.direction = 9000;
		ef.gain = T150_DI_MAX;
		ef.u.constant.magnitude = 10000;
		if (send_frame(good, T150_OP_EFFECT_UPLOAD, buf,
		    t150_proto_pack_effect(buf, sizeof(buf), &ef)) != 0)
			fail("the newcomer could not upload");
		expect_ok(good, "the newcomer's upload was refused");
		buf[0] = 1;
		buf[1] = 1;
		if (send_frame(good, T150_OP_EFFECT_START, buf, 2) != 0)
			fail("the newcomer could not start");
		expect_ok(good, "the newcomer's start was refused");
		if (wait_for_after(pipefd[0], "write 4: 41 01 41 01\n", mark) != 0)
			fail("the newcomer's effect never played");

		mark = loghave;
		went_ms = now_ms();
		(void)close(good);

		/*
		 * The stop for the slot that was playing, then the autocenter
		 * pair. The input stays open, deliberately: the daemon holds
		 * the wheel until it is itself leaving, and closing it here
		 * would rest the pedals at maximum for the next game.
		 */
		if (wait_for_after(pipefd[0], "write 4: 41 01 00 01\n"
		    "write 4: 40 03 00 00\n"
		    "write 4: 40 04 00 00\n", mark) != 0)
			fail("a client going away did not leave the wheel safe");
		if (strstr(logbuf + mark, "write 2: 42 00\n") != NULL)
			fail("a client going away closed the wheel's input");

		/*
		 * And it has to be the disconnect that did it rather than the
		 * watchdog noticing the silence afterwards. Both produce the
		 * same packets, so only the timing tells them apart: the read
		 * of end of file is immediate and the watchdog is a whole
		 * T150_WATCHDOG_MS away. Without this the test passed with the
		 * t150_session_end call deleted from the poll loop, which is
		 * the regression it exists to catch.
		 */
		if (now_ms() - went_ms >= T150_WATCHDOG_MS)
			fail("the wheel was released by the watchdog rather "
			    "than by the client going away");
	}

	(void)close(fd);

	/*
	 * A hangup has to leave the wheel safe too. The documented way to run
	 * the daemon is in a terminal, and closing that terminal sends one:
	 * dying without the safe state leaves the wheel holding whatever
	 * force it was last given, with its input still open so it does not
	 * even fall back to its own autocenter.
	 */
	{
		size_t mark;

		drain(pipefd[0]);
		mark = loghave;
		(void)kill(pid, SIGHUP);
		if (wait_for_after(pipefd[0], "write 4: 40 03 00 00\n"
		    "write 4: 40 04 00 00\n"
		    "write 2: 42 00\n", mark) != 0)
			fail("a hangup did not leave the wheel safe");
	}

out:
	(void)kill(pid, SIGTERM);
	(void)waitpid(pid, &status, 0);
	(void)close(pipefd[0]);
	(void)unlink(endpoint);
	{
		char lock[80];

		(void)snprintf(lock, sizeof(lock), "%s.lock", endpoint);
		(void)unlink(lock);
	}

	if (failures != 0) {
		fprintf(stderr, "socket_check: %d failure(s)\n", failures);
		fprintf(stderr, "daemon said:\n%s", logbuf);
		return 1;
	}

	printf("socket_check: ok\n");
	return 0;
}
