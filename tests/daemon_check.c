/*
 * daemon_check - what the daemon does with a frame.
 *
 * The session takes the current time as an argument rather than reading a
 * clock, so the watchdog and the ramp slicer can be driven through
 * simulated time and checked exactly, instead of by sleeping and hoping.
 *
 * Every expected packet here is a byte string from docs/PROTOCOL.md, so a
 * failure means either the session picked the wrong packet or the encoder
 * built it wrongly, and the two are easy to tell apart.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t150/proto.h"
#include "t150d.h"

#define TOKEN	"0123456789abcdef0123456789abcdef"

static int failures;
static char *logbuf;
static size_t loglen;
static size_t consumed;
static FILE *logfp;
static struct t150_backend be;
static struct t150_session sess;

static void
fail(const char *what)
{
	fprintf(stderr, "FAIL %s\n", what);
	failures++;
}

/*
 * Compare everything written since the last call. The stream is never
 * rewound: a memory stream keeps whatever it had beyond the seek point, so
 * rewinding would leave the tail of a long line behind a short one and the
 * comparisons would drift.
 */
static void
expect_log(const char *what, const char *want)
{
	const char *tail;

	(void)fflush(logfp);
	tail = logbuf + consumed;

	if (strcmp(tail, want) != 0) {
		fprintf(stderr, "FAIL %s\n  want: %s  got:  %s", what,
		    want[0] == '\0' ? "(nothing)\n" : want,
		    tail[0] == '\0' ? "(nothing)\n" : tail);
		failures++;
	}

	consumed = loglen;
}

/* Ignore whatever the setup for a test wrote, without asserting anything. */
static void
drain_log(void)
{
	(void)fflush(logfp);
	consumed = loglen;
}

static void
frame(uint8_t op, const uint8_t *payload, size_t len, uint64_t now,
    uint8_t want_op, enum t150_proto_err want_err)
{
	struct t150_reply rep;

	memset(&rep, 0, sizeof(rep));
	(void)t150_session_frame(&sess, op, payload, len, now, &rep);

	if (rep.op != want_op) {
		fprintf(stderr, "FAIL op %u: wanted reply %u, got %u\n", op,
		    want_op, rep.op);
		failures++;
		return;
	}
	if (want_op == T150_OP_ERROR && rep.payload[0] != (uint8_t)want_err) {
		fprintf(stderr, "FAIL op %u: wanted error %u, got %u\n", op,
		    (unsigned)want_err, rep.payload[0]);
		failures++;
	}
}

static void
hello(uint64_t now)
{
	frame(T150_OP_HELLO, (const uint8_t *)TOKEN, T150_TOKEN_LEN, now,
	    T150_OP_OK, T150_ERR_NONE);
}

static size_t
pack(uint8_t *buf, const struct t150_effect *ef)
{
	return t150_proto_pack_effect(buf, T150_PROTO_EFFECT_LEN, ef);
}

static void
put_u32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v & 0xff);
	b[1] = (uint8_t)((v >> 8) & 0xff);
	b[2] = (uint8_t)((v >> 16) & 0xff);
	b[3] = (uint8_t)((v >> 24) & 0xff);
}

static void
reset_session(void)
{
	t150_session_init(&sess, &be, TOKEN);
	(void)fflush(logfp);
	consumed = loglen;
}

static void
test_handshake(void)
{
	reset_session();

	/* Nothing is accepted before the token, not even a keepalive. */
	frame(T150_OP_KEEPALIVE, NULL, 0, 0, T150_OP_ERROR, T150_ERR_BAD_TOKEN);
	frame(T150_OP_HELLO, (const uint8_t *)"wrong", 5, 0, T150_OP_ERROR,
	    T150_ERR_BAD_TOKEN);
	frame(T150_OP_HELLO, (const uint8_t *)TOKEN, T150_TOKEN_LEN - 1, 0,
	    T150_OP_ERROR, T150_ERR_BAD_TOKEN);
	expect_log("handshake writes nothing", "");

	hello(0);
	frame(T150_OP_KEEPALIVE, NULL, 0, 0, T150_OP_OK, T150_ERR_NONE);
	expect_log("a good handshake writes nothing either", "");

	/* An operation nobody has defined is refused, not guessed at. */
	frame(99, NULL, 0, 0, T150_OP_ERROR, T150_ERR_UNSUPPORTED);
}

static void
test_settings(void)
{
	uint8_t arg[4];

	reset_session();
	hello(0);

	put_u32(arg, 10000);
	frame(T150_OP_SET_AUTOCENTER, arg, 4, 0, T150_OP_OK, T150_ERR_NONE);
	expect_log("autocenter on is strength then enable",
	    "write 4: 40 03 64 00\n"
	    "write 4: 40 04 01 00\n");

	put_u32(arg, 0);
	frame(T150_OP_SET_AUTOCENTER, arg, 4, 0, T150_OP_OK, T150_ERR_NONE);
	/*
	 * Off is the force, not the enable flag. Clearing 0x04 alone leaves
	 * the wheel gripped, which is the mistake that cost this project six
	 * hardware sessions and which used to live in this code path.
	 */
	expect_log("autocenter off releases the force, not just the flag",
	    "write 4: 40 03 00 00\n"
	    "write 4: 40 04 00 00\n");

	/* Half gain is 0x40: the wire's full scale is 0x80, not 0xff. */
	put_u32(arg, 5000);
	frame(T150_OP_SET_GAIN, arg, 4, 0, T150_OP_OK, T150_ERR_NONE);
	expect_log("gain", "write 2: 43 40\n");

	put_u32(arg, 900);
	frame(T150_OP_SET_RANGE, arg, 4, 0, T150_OP_OK, T150_ERR_NONE);
	expect_log("range", "write 4: 40 11 54 d5\n");

	/* A truncated payload is a frame error, not a zero argument. */
	frame(T150_OP_SET_GAIN, arg, 3, 0, T150_OP_ERROR, T150_ERR_BAD_FRAME);
}

static void
test_upload_and_play(void)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	struct t150_effect ef;
	uint8_t start[2];

	reset_session();
	hello(0);

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_CONSTANT;
	ef.slot = 0;
	ef.duration = T150_DURATION_INFINITE;
	ef.direction = 9000;
	ef.gain = T150_DI_MAX;
	ef.u.constant.magnitude = 10000;

	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 0, T150_OP_OK,
	    T150_ERR_NONE);
	expect_log("a constant uploads as three packets",
	    "write 9: 02 1c 00 00 00 00 00 00 00\n"
	    "write 4: 03 0e 00 40\n"
	    "write 15: 01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00\n");

	start[0] = 0;
	start[1] = 1;
	frame(T150_OP_EFFECT_START, start, 2, 0, T150_OP_OK, T150_ERR_NONE);
	expect_log("start", "write 4: 41 00 41 01\n");

	frame(T150_OP_EFFECT_STOP, start, 1, 0, T150_OP_OK, T150_ERR_NONE);
	expect_log("stop", "write 4: 41 00 00 01\n");

	/* Stopping twice does not write twice: it is already stopped. */
	frame(T150_OP_EFFECT_STOP, start, 1, 0, T150_OP_OK, T150_ERR_NONE);
	expect_log("a second stop is silent", "");

	/* A slot nobody uploaded to cannot be played. */
	start[0] = 5;
	frame(T150_OP_EFFECT_START, start, 2, 0, T150_OP_ERROR,
	    T150_ERR_BAD_SLOT);
	start[0] = T150_SLOT_MAX;
	frame(T150_OP_EFFECT_START, start, 2, 0, T150_OP_ERROR,
	    T150_ERR_BAD_SLOT);

	/* Nor can an effect be uploaded to one. */
	ef.slot = T150_SLOT_MAX;
	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 0, T150_OP_ERROR,
	    T150_ERR_BAD_SLOT);
	expect_log("a bad slot writes nothing", "");
}

static void
test_gain_folding(void)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	struct t150_effect ef;

	reset_session();
	hello(0);

	/*
	 * The wheel has one device gain and no per-effect gain, so a
	 * per-effect gain has to be folded into the magnitude before the
	 * effect is encoded. Half gain on a full magnitude is half force.
	 */
	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_CONSTANT;
	ef.duration = T150_DURATION_INFINITE;
	ef.direction = 9000;
	ef.gain = 5000;
	ef.u.constant.magnitude = 10000;

	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 0, T150_OP_OK,
	    T150_ERR_NONE);
	expect_log("per-effect gain is folded into the magnitude",
	    "write 9: 02 1c 00 00 00 00 00 00 00\n"
	    "write 4: 03 0e 00 20\n"
	    "write 15: 01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00\n");
}

static void
test_downgrade(void)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	struct t150_effect ef;

	reset_session();
	hello(0);

	/* Square is not in the protocol, so it goes out as a sine. */
	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_SQUARE;
	ef.slot = 1;
	ef.duration = 2000000;
	ef.gain = T150_DI_MAX;
	ef.u.periodic.magnitude = 10000;
	ef.u.periodic.period = 20000;

	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 0, T150_OP_OK,
	    T150_ERR_NONE);
	expect_log("square is sent as a sine",
	    "write 9: 02 38 00 00 00 00 00 00 00\n"
	    "write 8: 04 2a 00 7f 00 00 14 00\n"
	    "write 15: 01 01 22 40 d0 07 00 00 00 2a 00 38 00 00 00\n");
}

static void
test_ramp(void)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	struct t150_effect ef;
	uint8_t start[2];

	reset_session();
	hello(0);

	/*
	 * A ramp is not in the protocol either. It goes out as a constant
	 * that the daemon re-sends as it slides, so the upload carries the
	 * start value and the ticks carry the rest.
	 */
	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_RAMP;
	ef.slot = 2;
	ef.duration = 1000000;
	ef.direction = 9000;
	ef.gain = T150_DI_MAX;
	ef.u.ramp.start = 0;
	ef.u.ramp.end = 10000;

	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 0, T150_OP_OK,
	    T150_ERR_NONE);
	expect_log("a ramp uploads as a constant at its start value",
	    "write 9: 02 54 00 00 00 00 00 00 00\n"
	    "write 4: 03 46 00 00\n"
	    "write 15: 01 02 00 40 e8 03 00 00 00 46 00 54 00 00 00\n");

	start[0] = 2;
	start[1] = 1;
	frame(T150_OP_EFFECT_START, start, 2, 1000, T150_OP_OK, T150_ERR_NONE);
	expect_log("ramp start", "write 4: 41 02 41 01\n");

	/*
	 * Keepalives between the ticks, because a real client sends them and
	 * because without one the watchdog would fire in the middle of the
	 * slide and this would be testing the wrong thing.
	 */
	frame(T150_OP_KEEPALIVE, NULL, 0, 1500, T150_OP_OK, T150_ERR_NONE);

	/* Halfway along, the level is halfway between the two ends. */
	(void)t150_session_tick(&sess, 1500);
	expect_log("a ramp slides", "write 4: 03 46 00 20\n");

	/* Past the end it holds, rather than wrapping back to the start. */
	frame(T150_OP_KEEPALIVE, NULL, 0, 2500, T150_OP_OK, T150_ERR_NONE);
	(void)t150_session_tick(&sess, 2500);
	expect_log("a ramp holds at its end", "write 4: 03 46 00 40\n");

	frame(T150_OP_KEEPALIVE, NULL, 0, 2600, T150_OP_OK, T150_ERR_NONE);
	(void)t150_session_tick(&sess, 2600);
	expect_log("a finished ramp stops writing", "");
}

static void
test_watchdog(void)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	struct t150_effect ef;
	uint8_t start[2];
	unsigned int next;

	reset_session();
	hello(0);

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_CONSTANT;
	ef.duration = T150_DURATION_INFINITE;
	ef.direction = 9000;
	ef.gain = T150_DI_MAX;
	ef.u.constant.magnitude = 10000;

	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 100, T150_OP_OK,
	    T150_ERR_NONE);
	start[0] = 0;
	start[1] = 1;
	frame(T150_OP_EFFECT_START, start, 2, 100, T150_OP_OK, T150_ERR_NONE);
	drain_log();

	/* Still inside the window, so nothing happens. */
	next = t150_session_tick(&sess, 100 + T150_WATCHDOG_MS - 1);
	expect_log("the watchdog waits", "");
	if (next == 0 || next >= T150_WATCHDOG_MS)
		fail("the watchdog asked to be called back at the wrong time");

	/*
	 * Silence for the whole window is treated as a fault, whether or not
	 * the socket is still open: a crashed game sends no reset, and the
	 * wheel would otherwise hold the last force it was given.
	 */
	(void)t150_session_tick(&sess, 100 + T150_WATCHDOG_MS);
	expect_log("the watchdog stops the effect and releases the wheel",
	    "write 4: 41 00 00 01\n"
	    "write 4: 40 03 00 00\n"
	    "write 4: 40 04 00 00\n");

	/* Having fired, it does not keep firing. */
	(void)t150_session_tick(&sess, 100 + 10 * T150_WATCHDOG_MS);
	expect_log("the watchdog fires once", "");
}

/*
 * Updating a force that is already playing is the commonest thing a racing
 * game does, and the slot used to be wiped clean by it, taking the playing
 * flag with it. Nothing stops the effect on the wheel when a slot is
 * re-uploaded, so the watchdog then had no reason to send a stop and the
 * wheel kept pushing after the game died.
 */
static void
test_reupload_keeps_playing(void)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	struct t150_effect ef;
	uint8_t start[2];

	reset_session();
	hello(0);

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_CONSTANT;
	ef.duration = T150_DURATION_INFINITE;
	ef.direction = 9000;
	ef.gain = T150_DI_MAX;
	ef.u.constant.magnitude = 10000;

	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 100, T150_OP_OK,
	    T150_ERR_NONE);
	start[0] = 0;
	start[1] = 1;
	frame(T150_OP_EFFECT_START, start, 2, 100, T150_OP_OK, T150_ERR_NONE);

	/* The game changes the force without stopping it first. */
	ef.u.constant.magnitude = 5000;
	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 200, T150_OP_OK,
	    T150_ERR_NONE);
	drain_log();

	(void)t150_session_tick(&sess, 200 + T150_WATCHDOG_MS);
	expect_log("the watchdog still stops an effect that was re-uploaded",
	    "write 4: 41 00 00 01\n"
	    "write 4: 40 03 00 00\n"
	    "write 4: 40 04 00 00\n");
}

static void
test_panic_paths(void)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	struct t150_effect ef;
	uint8_t start[2];

	reset_session();
	hello(0);

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_CONSTANT;
	ef.duration = T150_DURATION_INFINITE;
	ef.direction = 9000;
	ef.gain = T150_DI_MAX;
	ef.u.constant.magnitude = 10000;
	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 0, T150_OP_OK,
	    T150_ERR_NONE);
	start[0] = 0;
	start[1] = 1;
	frame(T150_OP_EFFECT_START, start, 2, 0, T150_OP_OK, T150_ERR_NONE);
	drain_log();

	/* A reset stops and releases effects but leaves the autocenter, which
	 * the game owns separately. */
	frame(T150_OP_RESET, NULL, 0, 0, T150_OP_OK, T150_ERR_NONE);
	expect_log("reset stops the effect only", "write 4: 41 00 00 01\n");

	/* Goodbye is the graceful version, and does release the wheel. */
	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 0, T150_OP_OK,
	    T150_ERR_NONE);
	frame(T150_OP_EFFECT_START, start, 2, 0, T150_OP_OK, T150_ERR_NONE);
	drain_log();
	frame(T150_OP_BYE, NULL, 0, 0, T150_OP_OK, T150_ERR_NONE);
	expect_log("goodbye leaves the wheel limp",
	    "write 4: 41 00 00 01\n"
	    "write 4: 40 03 00 00\n"
	    "write 4: 40 04 00 00\n");
}

int
main(void)
{
	if ((logfp = open_memstream(&logbuf, &loglen)) == NULL) {
		fprintf(stderr, "daemon_check: cannot open a memory stream\n");
		return 1;
	}
	if (t150_backend_fake(&be, logfp) != 0) {
		fprintf(stderr, "daemon_check: cannot open the fake backend\n");
		return 1;
	}

	test_handshake();
	test_settings();
	test_upload_and_play();
	test_gain_folding();
	test_downgrade();
	test_ramp();
	test_watchdog();
	test_reupload_keeps_playing();
	test_panic_paths();

	(void)fclose(logfp);
	free(logbuf);

	if (failures != 0) {
		fprintf(stderr, "daemon_check: %d failure(s)\n", failures);
		return 1;
	}

	printf("daemon_check: ok\n");
	return 0;
}
