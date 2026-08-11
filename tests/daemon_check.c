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
#include <unistd.h>

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

/*
 * Whether a packet appears anywhere since the last comparison, for a test
 * that cares that something was sent rather than exactly what surrounded it.
 * Consumes the log like expect_log does.
 */
static int
log_contains(const char *want)
{
	int found;

	(void)fflush(logfp);
	found = strstr(logbuf + consumed, want) != NULL;
	consumed = loglen;

	return found;
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

/*
 * Run the emitter. An upload only sets what the slot should hold, so every
 * test that expects packets from one has to say when the daemon got round to
 * sending them, and every test that expects none has to prove the frame
 * itself was silent. Both halves are asserted, because expect_log("") also
 * passes for a test that simply forgot to tick.
 */
static unsigned int
tick(uint64_t now)
{
	return t150_session_tick(&sess, now);
}

static void
hello(uint64_t now)
{
	frame(T150_OP_HELLO, (const uint8_t *)TOKEN, T150_TOKEN_LEN, now,
	    T150_OP_OK, T150_ERR_NONE);
	/*
	 * A successful hello opens the wheel's input and states the device
	 * settings a client inherits, which every test after this one would
	 * otherwise have to account for. The packets themselves are checked by
	 * test_handshake and test_hello_states_the_settings.
	 */
	expect_log("hello opens the wheel's input and sets the gain",
	    "write 2: 42 04\n"
	    "write 2: 43 80\n");
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
	expect_log("range", "write 4: 40 11 55 d5\n");

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
	expect_log("an upload writes nothing by itself", "");
	(void)tick(0);
	expect_log("a constant uploads as three packets",
	    "write 9: 02 1c 00 00 00 00 00 00 00\n"
	    "write 4: 03 0e 00 40\n"
	    "write 15: 01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00\n");

	/* The same effect again is already on the wheel and costs nothing. */
	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 1, T150_OP_OK,
	    T150_ERR_NONE);
	(void)tick(T150_EMIT_MS + 1);
	expect_log("re-uploading an unchanged effect is silent", "");

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
	expect_log("the upload itself is silent", "");
	(void)tick(0);
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

	/* Square has its own type code, 0x4020, and is sent as itself. */
	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_SQUARE;
	ef.slot = 1;
	ef.duration = 2000000;
	ef.gain = T150_DI_MAX;
	ef.u.periodic.magnitude = 10000;
	ef.u.periodic.period = 20000;

	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 0, T150_OP_OK,
	    T150_ERR_NONE);
	expect_log("the upload itself is silent", "");
	(void)tick(0);
	expect_log("square is sent as a square",
	    "write 9: 02 38 00 00 00 00 00 00 00\n"
	    "write 8: 04 2a 00 7f 00 00 14 00\n"
	    "write 15: 01 01 20 40 d0 07 00 00 00 2a 00 38 00 00 00\n");
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
	 * that the daemon re-computes as it slides, so the first tick carries
	 * the start value and the later ones carry the rest. Each of those is
	 * the full set of three packets, because any difference sends the set.
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
	expect_log("the upload itself is silent", "");
	(void)tick(0);
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
	(void)tick(1500);
	expect_log("a ramp slides: the level, then the re-play",
	    "write 4: 03 46 00 20\n"
	    "write 4: 41 02 41 01\n");

	/* Past the end it holds, rather than wrapping back to the start. */
	frame(T150_OP_KEEPALIVE, NULL, 0, 2500, T150_OP_OK, T150_ERR_NONE);
	(void)tick(2500);
	expect_log("a ramp holds at its end",
	    "write 4: 03 46 00 40\n"
	    "write 4: 41 02 41 01\n");

	frame(T150_OP_KEEPALIVE, NULL, 0, 2600, T150_OP_OK, T150_ERR_NONE);
	(void)tick(2600);
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
 * A per-effect gain has to reach everything the effect pushes with, not just
 * the magnitude. A condition's coefficients are its slope and the
 * saturations only cap it, so scaling the caps alone left the wheel pushing
 * at full rate everywhere below them.
 */
static void
test_gain_reaches_conditions(void)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	struct t150_effect ef;

	reset_session();
	hello(0);

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_SPRING;
	ef.duration = T150_DURATION_INFINITE;
	ef.gain = T150_DI_MAX / 2;
	ef.u.condition.pos_coeff = 10000;
	ef.u.condition.neg_coeff = -10000;
	ef.u.condition.pos_saturation = 10000;
	ef.u.condition.neg_saturation = 10000;

	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 100, T150_OP_OK,
	    T150_ERR_NONE);
	expect_log("the upload itself is silent", "");
	(void)tick(100);

	/*
	 * Half gain, so the coefficients land at half of 100 and the
	 * saturations at half of the spring's 0x54 maximum.
	 */
	expect_log("half gain halves a spring's slope as well as its cap",
	    "write 11: 05 1c 00 00 00 00 00 00 00 46 54\n"
	    "write 11: 05 0e 00 32 ce 00 00 00 00 2a 2a\n"
	    "write 15: 01 00 40 40 ff ff 00 00 00 0e 00 1c 00 00 00\n");
}

/*
 * A client leaving for good closes the wheel's input, where the watchdog
 * only quiets it. The wheel renders nothing while no input is open, so
 * whatever opened one has to close it or the wheel is left in a state no
 * game asked for.
 */
static void
test_session_end_closes_the_input(void)
{
	reset_session();
	hello(0);
	drain_log();

	t150_session_end(&sess, "test");
	expect_log("ending a session releases the wheel and closes its input",
	    "write 4: 40 03 00 00\n"
	    "write 4: 40 04 00 00\n"
	    "write 2: 42 00\n");

	/* The watchdog does not: a quiet client may yet come back. */
	reset_session();
	hello(0);
	drain_log();
	(void)t150_session_tick(&sess, T150_WATCHDOG_MS);
	expect_log("the watchdog leaves the input open",
	    "write 4: 40 03 00 00\n"
	    "write 4: 40 04 00 00\n");

	/*
	 * A goodbye is the graceful way out, and it has to close the input
	 * like any other. The client clears its own hello on the way, so
	 * anything that decides by asking whether it is still there skips
	 * the close and leaves the wheel's input open for good.
	 */
	reset_session();
	hello(0);
	frame(T150_OP_BYE, NULL, 0, 0, T150_OP_OK, T150_ERR_NONE);
	drain_log();
	t150_session_end(&sess, "test");
	expect_log("a goodbye still closes the wheel's input",
	    "write 4: 40 03 00 00\n"
	    "write 4: 40 04 00 00\n"
	    "write 2: 42 00\n");

	/*
	 * A connection that never said hello never had the wheel's input
	 * opened for it and never sent anything to render, so there is
	 * nothing of its doing to undo. Any local process can open the port,
	 * and doing so must not reach the hardware.
	 */
	reset_session();
	drain_log();
	t150_session_end(&sess, "test");
	expect_log("a client that never said hello leaves the wheel alone", "");

	/* A rejected hello is no hello: same silence. */
	reset_session();
	frame(T150_OP_HELLO, (const uint8_t *)"wrongwrongwrongwrongwrongwrong12",
	    T150_TOKEN_LEN, 0, T150_OP_ERROR, T150_ERR_BAD_TOKEN);
	drain_log();
	t150_session_end(&sess, "test");
	expect_log("a rejected hello leaves the wheel alone", "");
}

/*
 * DISFFC_STOPALL must stop without releasing, or a game that pauses cannot
 * start its effects again afterwards.
 */
static void
test_stop_all_keeps_slots(void)
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
	drain_log();

	frame(T150_OP_STOP_ALL, NULL, 0, 110, T150_OP_OK, T150_ERR_NONE);
	expect_log("stop all stops the effect", "write 4: 41 00 00 01\n");

	/* The slot survives, so the same effect starts again. */
	frame(T150_OP_EFFECT_START, start, 2, 120, T150_OP_OK, T150_ERR_NONE);
	expect_log("and the slot is still loaded", "write 4: 41 00 41 01\n");

	/* A reset, by contrast, releases it. */
	frame(T150_OP_RESET, NULL, 0, 130, T150_OP_OK, T150_ERR_NONE);
	drain_log();
	frame(T150_OP_EFFECT_START, start, 2, 140, T150_OP_ERROR,
	    T150_ERR_BAD_SLOT);
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

/* ------------------------------------------------------------------ */
/* The emitter: what is coalesced, what is not, and what it costs. */

/* Fill a plain infinite constant, which is what most of these need. */
static void
constant(struct t150_effect *ef, uint8_t slot, int32_t magnitude)
{
	memset(ef, 0, sizeof(*ef));
	ef->kind = T150_EFFECT_CONSTANT;
	ef->slot = slot;
	ef->duration = T150_DURATION_INFINITE;
	ef->direction = 9000;
	ef->gain = T150_DI_MAX;
	ef->u.constant.magnitude = magnitude;
}

static void
upload_at(struct t150_effect *ef, uint64_t now)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];

	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, ef), now, T150_OP_OK,
	    T150_ERR_NONE);
}

/*
 * A change too small to reach the wire is not a change. The constant's level
 * is one signed byte, so sixty four DirectInput magnitudes share each value,
 * and a comparison of effect structs rather than of encoded bytes would send
 * a packet the wheel cannot tell from the one it already has.
 */
static void
test_subwire_change_is_silent(void)
{
	struct t150_effect ef;

	reset_session();
	hello(0);

	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	(void)tick(0);
	drain_log();

	ef.u.constant.magnitude = 9990;
	upload_at(&ef, 10);
	(void)tick(10);
	expect_log("a magnitude that encodes to the same byte is silent", "");

	/* One that does reach the wire still goes. */
	ef.u.constant.magnitude = 5000;
	upload_at(&ef, 20);
	(void)tick(20);
	expect_log("a magnitude that does reach the wire is sent, alone",
	    "write 4: 03 0e 00 20\n");
}

/*
 * A stop cancels whatever was waiting for that slot. Nothing establishes that
 * a parameter packet is inert on a stopped slot, and a pass firing just after
 * a stop would be asking that of a wheel someone is holding.
 */
static void
test_stop_drops_pending_state(void)
{
	struct t150_effect ef;
	uint8_t start[2];

	reset_session();
	hello(0);

	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	(void)tick(0);
	start[0] = 0;
	start[1] = 1;
	frame(T150_OP_EFFECT_START, start, 2, 0, T150_OP_OK, T150_ERR_NONE);
	drain_log();

	ef.u.constant.magnitude = 5000;
	upload_at(&ef, 1);
	frame(T150_OP_EFFECT_STOP, start, 1, 2, T150_OP_OK, T150_ERR_NONE);
	expect_log("the stop itself goes out at once", "write 4: 41 00 00 01\n");

	(void)tick(2 + T150_EMIT_MS);
	expect_log("and nothing follows it to a stopped slot", "");
}

/*
 * The emitter is a floor on the interval between passes, not a timer. With
 * nothing to send it must arm nothing, or the daemon would wake 250 times a
 * second to discover there is no work.
 */
static void
test_idle_tick_returns_the_watchdog(void)
{
	struct t150_effect ef;
	unsigned int wait;

	reset_session();
	hello(0);

	wait = tick(0);
	if (wait != T150_WATCHDOG_MS)
		fail("an idle tick sleeps until the watchdog");

	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	(void)tick(0);
	drain_log();

	wait = tick(1);
	if (wait <= T150_EMIT_MS)
		fail("a tick with nothing dirty still sleeps");
	if (wait != T150_WATCHDOG_MS - 1)
		fail("an idle tick sleeps until the watchdog, less the quiet time");
}

/* Two uploads inside one emit period cost one pass, carrying the newer one. */
static void
test_emit_rate_is_bounded(void)
{
	struct t150_effect ef;

	reset_session();
	hello(0);

	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	(void)tick(0);
	drain_log();

	ef.u.constant.magnitude = 5000;
	upload_at(&ef, 1);
	(void)tick(1);
	expect_log("a second pass inside the emit period is held", "");

	ef.u.constant.magnitude = 2500;
	upload_at(&ef, 2);
	(void)tick(T150_EMIT_MS);
	expect_log("and the pass that follows carries only the newest value",
	    "write 4: 03 0e 00 10\n");
}

/*
 * A game updating faster than the emit period has the superseded values
 * dropped rather than queued, which is the point: the wheel holds one value
 * per slot and the ones in between could not have been felt.
 *
 * Assetto Corsa's physics runs at 333 Hz, one update every three
 * milliseconds, which is faster than the four millisecond floor, so roughly
 * every other update is superseded and the wheel sees about 250 Hz.
 */
static void
test_updates_faster_than_the_emit_period_are_coalesced(void)
{
	struct t150_effect ef;

	reset_session();
	hello(0);

	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	(void)tick(0);
	drain_log();

	/* Three milliseconds later, inside the floor: held, not queued. */
	ef.u.constant.magnitude = 7500;
	upload_at(&ef, 3);
	(void)tick(3);
	expect_log("an update inside the emit period waits", "");

	/* Six milliseconds: the pass runs and carries only the newest. */
	ef.u.constant.magnitude = 5000;
	upload_at(&ef, 6);
	(void)tick(6);
	expect_log("and the superseded value is dropped, not sent late",
	    "write 4: 03 0e 00 20\n");

	/* Slower than the floor, and nothing is coalesced at all. */
	ef.u.constant.magnitude = 2500;
	upload_at(&ef, 20);
	(void)tick(20);
	expect_log("an update slower than the emit period goes straight out",
	    "write 4: 03 0e 00 10\n");
}

static int write_fails;
static int (*real_write)(void *priv, const uint8_t *buf, size_t len);

/* The fake backend, with a switch for refusing every write. */
static int
failing_write(void *priv, const uint8_t *buf, size_t len)
{
	if (write_fails)
		return -1;

	return real_write(priv, buf, len);
}

/*
 * A wheel that refuses every write must not turn the poll timeout into a
 * spin. The slot stays dirty so the state is not lost, but the retry rides on
 * the next frame or the next watchdog wake rather than on a four millisecond
 * deadline.
 */
static void
test_write_failure_does_not_pin_the_poll_loop(void)
{
	struct t150_effect ef;
	unsigned int wait;

	reset_session();
	hello(0);
	drain_log();

	write_fails = 1;
	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	wait = tick(0);
	write_fails = 0;

	if (wait <= T150_EMIT_MS)
		fail("a failed pass must not shorten the poll timeout");
	drain_log();

	/* The state survived, so the next pass sends it. */
	frame(T150_OP_KEEPALIVE, NULL, 0, 100, T150_OP_OK, T150_ERR_NONE);
	(void)tick(100);
	expect_log("a failed write is retried in full, not lost",
	    "write 9: 02 1c 00 00 00 00 00 00 00\n"
	    "write 4: 03 0e 00 40\n"
	    "write 15: 01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00\n");
}

/*
 * The write happens after the frame that caused it has been answered, so a
 * device error has nowhere to go but the next upload. It is reported there
 * and on nothing else: the proxy drops its socket on an error reply to a
 * keepalive and nothing reconnects, so that would cost the game its force
 * feedback for the life of the process.
 */
static void
test_device_error_is_reported_on_the_next_upload(void)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	struct t150_effect ef;

	reset_session();
	hello(0);
	drain_log();

	write_fails = 1;
	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	(void)tick(0);
	write_fails = 0;

	/* A keepalive in between says nothing about it. */
	frame(T150_OP_KEEPALIVE, NULL, 0, 10, T150_OP_OK, T150_ERR_NONE);

	ef.u.constant.magnitude = 5000;
	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 20, T150_OP_ERROR,
	    T150_ERR_DEVICE_IO);

	/*
	 * The frame that carried the bad news is not also the frame that got
	 * thrown away: its state was stored before the error was answered.
	 */
	drain_log();
	(void)tick(20 + T150_EMIT_MS);
	expect_log("the upload that reported the error still took effect",
	    "write 9: 02 1c 00 00 00 00 00 00 00\n"
	    "write 4: 03 0e 00 20\n"
	    "write 15: 01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00\n");

	/* Reported once, not on every upload afterwards. */
	ef.u.constant.magnitude = 2500;
	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 30, T150_OP_OK,
	    T150_ERR_NONE);
}

/*
 * What a client inherits when it takes the wheel. The gain is unconditional,
 * because nothing else ever set it and a wheel scales every force by it. The
 * range is only sent when the daemon was given one, because no game can ask
 * for it and guessing a number would be worse than leaving the wheel alone.
 */
static void
test_hello_states_the_settings(void)
{
	reset_session();
	sess.range_deg = 900;
	frame(T150_OP_HELLO, (const uint8_t *)TOKEN, T150_TOKEN_LEN, 0,
	    T150_OP_OK, T150_ERR_NONE);
	expect_log("hello opens the input, sets full gain and the range",
	    "write 2: 42 04\n"
	    "write 2: 43 80\n"
	    "write 4: 40 11 55 d5\n");

	/* Without one, the wheel keeps whatever range it had. */
	reset_session();
	frame(T150_OP_HELLO, (const uint8_t *)TOKEN, T150_TOKEN_LEN, 0,
	    T150_OP_OK, T150_ERR_NONE);
	expect_log("no range given, none sent",
	    "write 2: 42 04\n"
	    "write 2: 43 80\n");
}

/*
 * A level change costs one packet, and -t puts the other two back.
 *
 * The vendor's own DirectInput capture uploads an effect once and then
 * modulates it with bare update packets, so that is the default. Every write
 * is a synchronous call on the thread a game is waiting for, and two in three
 * of them were re-stating bytes the wheel already had.
 */
static void
test_only_the_packet_that_moved_is_sent(void)
{
	struct t150_effect ef;
	uint8_t start[2];

	reset_session();
	hello(0);
	/*
	 * An earlier test moved the backend's epoch, so the first tick of any
	 * session after it re-applies the settings. Take that here rather
	 * than in the middle of an assertion about effect packets.
	 */
	(void)tick(0);
	drain_log();

	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	(void)tick(0);
	expect_log("the first emission is the whole effect",
	    "write 9: 02 1c 00 00 00 00 00 00 00\n"
	    "write 4: 03 0e 00 40\n"
	    "write 15: 01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00\n");

	ef.u.constant.magnitude = 5000;
	upload_at(&ef, 10);
	(void)tick(10);
	expect_log("a level change is one packet", "write 4: 03 0e 00 20\n");

	/* A change of duration is the effect being redefined, so all three. */
	ef.duration = 2000000;
	upload_at(&ef, 20);
	(void)tick(20);
	expect_log("a change to the commit sends the set",
	    "write 9: 02 1c 00 00 00 00 00 00 00\n"
	    "write 4: 03 0e 00 20\n"
	    "write 15: 01 00 00 40 d0 07 00 00 00 0e 00 1c 00 00 00\n");

	/*
	 * A constant that is playing is re-played after its level moves, the
	 * way Thrustmaster's own driver does it, and a stopped one is not.
	 * The tester felt the difference when the update went out alone.
	 */
	reset_session();
	hello(0);
	(void)tick(0);
	drain_log();
	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	(void)tick(0);
	start[0] = 0;
	start[1] = 1;
	frame(T150_OP_EFFECT_START, start, 2, 0, T150_OP_OK, T150_ERR_NONE);
	drain_log();

	ef.u.constant.magnitude = 5000;
	upload_at(&ef, 10);
	(void)tick(10);
	expect_log("a playing constant is re-played after its level moves",
	    "write 4: 03 0e 00 20\n"
	    "write 4: 41 00 41 01\n");

	frame(T150_OP_EFFECT_STOP, start, 1, 20, T150_OP_OK, T150_ERR_NONE);
	drain_log();
	ef.u.constant.magnitude = 2500;
	upload_at(&ef, 30);
	(void)tick(30);
	expect_log("a stopped one is not",
	    "write 4: 03 0e 00 10\n");

	/* -t restores the old behaviour for a side by side comparison. */
	reset_session();
	sess.always_triple = 1;
	hello(0);
	(void)tick(0);
	drain_log();
	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	(void)tick(0);
	drain_log();
	ef.u.constant.magnitude = 5000;
	upload_at(&ef, 10);
	(void)tick(10);
	expect_log("with -t a level change still sends all three",
	    "write 9: 02 1c 00 00 00 00 00 00 00\n"
	    "write 4: 03 0e 00 20\n"
	    "write 15: 01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00\n");
}

/*
 * A game that starts an effect on every frame must not starve the others.
 *
 * The emit deadline is one deadline for the whole session. When a start
 * pushed it, a game whose frames are closer together than the emit period
 * moved it further away than its own next frame, the pass never ran, and
 * every slot the game was not starting kept whatever the wheel already had.
 * Assetto Corsa's physics is 3 ms against a 4 ms period, and its damper went
 * half a second without an update while its constant force was restarted
 * every frame.
 */
static void
test_a_start_every_frame_does_not_starve_other_slots(void)
{
	struct t150_effect ef;
	uint8_t start[2];
	uint64_t t;

	reset_session();
	hello(0);

	/* Slot 0 is the force the game restarts; slot 1 is the damper. */
	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_DAMPER;
	ef.slot = 1;
	ef.duration = T150_DURATION_INFINITE;
	ef.direction = 9000;
	ef.gain = T150_DI_MAX;
	ef.u.condition.pos_coeff = 2000;
	ef.u.condition.neg_coeff = 2000;
	upload_at(&ef, 0);
	(void)tick(0);
	start[0] = 0;
	start[1] = 1;
	frame(T150_OP_EFFECT_START, start, 2, 0, T150_OP_OK, T150_ERR_NONE);
	drain_log();

	/* The surface changes: the damper is re-uploaded once, at 3 ms. */
	ef.u.condition.pos_coeff = 8000;
	ef.u.condition.neg_coeff = 8000;
	upload_at(&ef, 3);

	/*
	 * The game carries on at 3 ms a frame, restarting slot 0 each time.
	 * The damper must still reach the wheel within one emit period.
	 */
	for (t = 3; t <= 40; t += 3) {
		(void)tick(t);
		constant(&ef, 0, (int32_t)(t * 100));
		upload_at(&ef, t);
		frame(T150_OP_EFFECT_START, start, 2, t, T150_OP_OK,
		    T150_ERR_NONE);
	}

	/*
	 * 0x50 is the changed coefficient, on slot 1's parameter key 0x2a.
	 * Its presence anywhere in the burst is the whole assertion: before
	 * the fix this slot was silent for as long as the game kept starting.
	 */
	if (!log_contains("write 11: 05 2a 00 50 50"))
		fail("a start every frame starves the other slots");
}

/*
 * Re-acquiring the wheel scrubs every slot, and the session has no other way
 * to learn that. Without the epoch the coalescer would go on suppressing an
 * upload it believes the wheel already has, and a replugged wheel would stay
 * empty for the rest of the session with nothing said anywhere.
 */
static void
test_backend_epoch_reuploads_everything(void)
{
	struct t150_effect ef;
	uint8_t start[2];

	reset_session();
	hello(0);

	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	(void)tick(0);
	start[0] = 0;
	start[1] = 1;
	frame(T150_OP_EFFECT_START, start, 2, 0, T150_OP_OK, T150_ERR_NONE);
	drain_log();

	/* The same upload again changes nothing and writes nothing. */
	upload_at(&ef, 10);
	(void)tick(10);
	expect_log("nothing to say before the wheel is re-acquired", "");

	/*
	 * The settings, then the effect, then the start it was playing with,
	 * in that order: the wheel cannot be told to play parameters it does
	 * not have yet.
	 */
	be.epoch++;
	frame(T150_OP_KEEPALIVE, NULL, 0, 20, T150_OP_OK, T150_ERR_NONE);
	(void)tick(20);
	expect_log("a re-acquired wheel gets its settings, effect and start",
	    "write 2: 43 80\n"
	    "write 9: 02 1c 00 00 00 00 00 00 00\n"
	    "write 4: 03 0e 00 40\n"
	    "write 15: 01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00\n"
	    "write 4: 41 00 41 01\n");

	/* Once, not on every tick after it. */
	frame(T150_OP_KEEPALIVE, NULL, 0, 30, T150_OP_OK, T150_ERR_NONE);
	(void)tick(30);
	expect_log("and not again on the tick after", "");
}

/*
 * A start refused while the wheel was off the bus is still a start the game
 * asked for, and the wheel has to be told about it when it comes back.
 *
 * This is the whole of test 35: two starts were refused during a replug, the
 * daemon forgot they had ever been asked for, the game never asked again
 * because from its side nothing had failed, and the wheel came back with
 * every effect loaded and stopped. Force feedback was gone until the game
 * was restarted.
 */
static void
test_a_refused_start_is_replayed_when_the_wheel_returns(void)
{
	struct t150_effect ef;
	uint8_t start[2];

	reset_session();
	hello(0);

	constant(&ef, 0, 10000);
	upload_at(&ef, 0);
	(void)tick(0);

	/* The wheel goes: every write fails from here. */
	write_fails = 1;
	start[0] = 0;
	start[1] = 1;
	frame(T150_OP_EFFECT_START, start, 2, 0, T150_OP_ERROR,
	    T150_ERR_DEVICE_IO);
	write_fails = 0;
	drain_log();

	/* It comes back, which the backend says by moving its epoch. */
	be.epoch++;
	frame(T150_OP_KEEPALIVE, NULL, 0, 10, T150_OP_OK, T150_ERR_NONE);
	(void)tick(10);

	if (!log_contains("write 4: 41 00 41 01"))
		fail("a start refused during a replug is never replayed");
}

/*
 * The verbose parameter line has to say which condition it is. A spring
 * resists displacement from a centre and a damper resists velocity, so only
 * one of them can produce a vibration anchored to a position, and the tester
 * reported exactly that at dead centre and again near 135 degrees. The line
 * said "condition" for both and could not settle it.
 *
 * Worth a test rather than an eyeball because a reversed ternary here reads
 * perfectly and would send the next release chasing the wrong effect.
 */
static void
upload_condition_verbose(uint8_t kind, char *out, size_t outlen)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	struct t150_effect ef;
	FILE *cap;
	int saved;
	long n;

	reset_session();
	hello(0);
	sess.verbose = 1;

	memset(&ef, 0, sizeof(ef));
	ef.kind = kind;
	ef.slot = 3;
	ef.duration = T150_DURATION_INFINITE;
	ef.gain = T150_DI_MAX;
	ef.u.condition.center = 0;
	ef.u.condition.pos_coeff = 9998;
	ef.u.condition.neg_coeff = 9998;
	ef.u.condition.pos_saturation = 10000;
	ef.u.condition.neg_saturation = 10000;
	ef.u.condition.deadband = 0;

	out[0] = '\0';
	if ((cap = tmpfile()) == NULL)
		return;

	/*
	 * The line goes to stderr, so take it over for the length of the
	 * upload and put it back afterwards however that turns out.
	 */
	(void)fflush(stderr);
	if ((saved = dup(fileno(stderr))) < 0) {
		(void)fclose(cap);
		return;
	}
	(void)dup2(fileno(cap), fileno(stderr));

	/*
	 * Any time at all past the one second rate limit, which starts at
	 * zero on a fresh session.
	 */
	frame(T150_OP_EFFECT_UPLOAD, buf, pack(buf, &ef), 2000, T150_OP_OK,
	    T150_ERR_NONE);

	(void)fflush(stderr);
	(void)dup2(saved, fileno(stderr));
	(void)close(saved);

	if ((n = ftell(cap)) > 0 && (size_t)n < outlen) {
		rewind(cap);
		if (fread(out, 1, (size_t)n, cap) == (size_t)n)
			out[n] = '\0';
	}
	(void)fclose(cap);
	sess.verbose = 0;
}

static void
test_the_parameter_log_names_the_condition(void)
{
	char out[512];

	upload_condition_verbose(T150_EFFECT_SPRING, out, sizeof(out));
	if (strstr(out, "slot 3 spring:") == NULL)
		fail("a spring is logged as a spring");
	if (strstr(out, "deadband 0") == NULL)
		fail("and its deadband is shown");

	upload_condition_verbose(T150_EFFECT_DAMPER, out, sizeof(out));
	if (strstr(out, "slot 3 damper:") == NULL)
		fail("a damper is logged as a damper");

	/*
	 * A game asking for friction gets a damper, and the line has to show
	 * what the wheel was given rather than what was asked for, because
	 * the downgrade already says the latter on its own line.
	 */
	upload_condition_verbose(T150_EFFECT_FRICTION, out, sizeof(out));
	if (strstr(out, "slot 3 damper:") == NULL)
		fail("a downgraded friction is logged as the damper it became");
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
	/*
	 * Everything goes through the wrapper, which is transparent until a
	 * test asks it to refuse. That is how a device error is provoked
	 * without a device.
	 */
	real_write = be.write;
	be.write = failing_write;

	test_handshake();
	test_settings();
	test_upload_and_play();
	test_gain_folding();
	test_downgrade();
	test_ramp();
	test_watchdog();
	test_reupload_keeps_playing();
	test_stop_all_keeps_slots();
	test_session_end_closes_the_input();
	test_gain_reaches_conditions();
	test_panic_paths();
	test_subwire_change_is_silent();
	test_stop_drops_pending_state();
	test_idle_tick_returns_the_watchdog();
	test_emit_rate_is_bounded();
	test_updates_faster_than_the_emit_period_are_coalesced();
	test_write_failure_does_not_pin_the_poll_loop();
	test_device_error_is_reported_on_the_next_upload();
	test_backend_epoch_reuploads_everything();
	test_a_refused_start_is_replayed_when_the_wheel_returns();
	test_a_start_every_frame_does_not_starve_other_slots();
	test_the_parameter_log_names_the_condition();
	test_hello_states_the_settings();
	test_only_the_packet_that_moved_is_sent();

	(void)fclose(logfp);
	free(logbuf);

	if (failures != 0) {
		fprintf(stderr, "daemon_check: %d failure(s)\n", failures);
		return 1;
	}

	printf("daemon_check: ok\n");
	return 0;
}
