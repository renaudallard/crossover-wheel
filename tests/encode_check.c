/*
 * encode_check - golden vectors for the wire encoders.
 *
 * Every expected byte string here was derived from docs/PROTOCOL.md
 * independently of src/lib/encode.c, so this is a check on the encoders
 * rather than a restatement of them. It needs no hardware and no Mac, which
 * is the point: the packet layouts can be wrong in only one place.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>

#include "t150/encode.h"
#include "t150/t150.h"

#define BUFLEN	32

static int failures;

static void
check(const char *what, const uint8_t *got, size_t gotlen,
    const uint8_t *want, size_t wantlen)
{
	size_t i;

	if (gotlen == wantlen && memcmp(got, want, wantlen) == 0)
		return;

	fprintf(stderr, "FAIL %s\n  want:", what);
	for (i = 0; i < wantlen; i++)
		fprintf(stderr, " %02x", want[i]);
	fprintf(stderr, "\n  got: ");
	for (i = 0; i < gotlen; i++)
		fprintf(stderr, " %02x", got[i]);
	fprintf(stderr, "\n");
	failures++;
}

#define CHECK(what, buf, len, ...) do {					\
	static const uint8_t want__[] = { __VA_ARGS__ };		\
	check((what), (buf), (len), want__, sizeof(want__));		\
} while (0)

static void
check_int(const char *what, long got, long want)
{
	if (got == want)
		return;

	fprintf(stderr, "FAIL %s: want %ld, got %ld\n", what, want, got);
	failures++;
}

static void
test_settings(void)
{
	uint8_t b[BUFLEN];

	CHECK("autocenter full", b, t150_enc_autocenter_force(b, sizeof(b), 10000),
	    0x40, 0x03, 0x64, 0x00);
	CHECK("autocenter half", b, t150_enc_autocenter_force(b, sizeof(b), 5000),
	    0x40, 0x03, 0x32, 0x00);
	CHECK("autocenter clamps", b, t150_enc_autocenter_force(b, sizeof(b), 99999),
	    0x40, 0x03, 0x64, 0x00);

	CHECK("autocenter on", b, t150_enc_autocenter_enable(b, sizeof(b), 1),
	    0x40, 0x04, 0x01, 0x00);
	CHECK("autocenter off", b, t150_enc_autocenter_enable(b, sizeof(b), 0),
	    0x40, 0x04, 0x00, 0x00);

	CHECK("range 270", b, t150_enc_range(b, sizeof(b), 270),
	    0x40, 0x11, 0xff, 0x3f);
	CHECK("range 900", b, t150_enc_range(b, sizeof(b), 900),
	    0x40, 0x11, 0x54, 0xd5);
	CHECK("range 1080", b, t150_enc_range(b, sizeof(b), 1080),
	    0x40, 0x11, 0xff, 0xff);

	/*
	 * A finite duration must never land on 0xFFFF, which is the wheel's
	 * endless marker. Anything from 65.535 seconds up used to saturate
	 * straight onto it, so a game asking for a long effect got one that
	 * never stopped.
	 */
	{
		struct t150_effect d;

		memset(&d, 0, sizeof(d));
		d.kind = T150_EFFECT_CONSTANT;
		d.gain = T150_DI_MAX;
		d.duration = 70000000;		/* 70 seconds */
		CHECK("a long finite duration stops short of endless", b,
		    t150_enc_ff_commit(b, sizeof(b), &d),
		    0x01, 0x00, 0x00, 0x40, 0xfe, 0xff, 0x00, 0x00, 0x00,
		    0x0e, 0x00, 0x1c, 0x00, 0x00, 0x00);

		d.duration = T150_DURATION_INFINITE;
		CHECK("endless still says endless", b,
		    t150_enc_ff_commit(b, sizeof(b), &d),
		    0x01, 0x00, 0x00, 0x40, 0xff, 0xff, 0x00, 0x00, 0x00,
		    0x0e, 0x00, 0x1c, 0x00, 0x00, 0x00);
	}

	/*
	 * Gain is two bytes and its full scale is 0x80, so half gain is 0x40
	 * rather than 0x80. Getting this wrong sends roughly twice the force
	 * the caller asked for.
	 */
	CHECK("gain full", b, t150_enc_gain(b, sizeof(b), 10000), 0x43, 0x80);
	CHECK("gain half", b, t150_enc_gain(b, sizeof(b), 5000), 0x43, 0x40);
	CHECK("gain zero", b, t150_enc_gain(b, sizeof(b), 0), 0x43, 0x00);

	/*
	 * Opening the wheel's input. Two bytes, and the driver builds them as
	 * a little-endian uint16 0x0442, so the opcode is the low byte and
	 * goes first. Getting that round the wrong way would send 04 42.
	 */
	CHECK("input open", b, t150_enc_input_open(b, sizeof(b)), 0x42, 0x04);
	CHECK("input close", b, t150_enc_input_close(b, sizeof(b)), 0x42, 0x00);
	check_int("input open refuses a short buffer",
	    (long)t150_enc_input_open(b, 1), 0);
}

/*
 * The level a constant actually reaches on the wire, which is the sine
 * projection and the magnitude scaling together. A hardware run once drove
 * the wheel to full lock and was read here as a scaling fault; it was not,
 * a constant force is a steady torque and a free wheel travels to its stop
 * under one. This pins the arithmetic so the next such reading has
 * something to check against.
 */
static int8_t
const_level(int32_t magnitude, uint32_t direction)
{
	struct t150_effect ef;
	uint8_t b[16];

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_CONSTANT;
	ef.gain = T150_DI_MAX;
	ef.direction = direction;
	ef.u.constant.magnitude = magnitude;
	if (t150_enc_ff_update(b, sizeof(b), &ef) != T150_FF_UPDATE_LEN_CONSTANT)
		return 0;

	return (int8_t)b[3];
}

static void
test_constant_level(void)
{
	/* Direction, at full magnitude. Zero across the axis, not a fault. */
	check_int("north is no force on one axis", const_level(10000, 0), 0);
	check_int("east is full right", const_level(10000, 9000), 64);
	check_int("south is no force", const_level(10000, 18000), 0);
	check_int("west is full left", const_level(10000, 27000), -64);
	check_int("45 degrees projects", const_level(10000, 4500), 45);
	check_int("225 degrees projects", const_level(10000, 22500), -45);

	/* Magnitude, due east, linear onto the ceiling and symmetric. */
	check_int("full magnitude", const_level(10000, 9000), 64);
	check_int("half magnitude", const_level(5000, 9000), 32);
	check_int("quarter magnitude", const_level(2500, 9000), 16);
	check_int("zero magnitude", const_level(0, 9000), 0);
	check_int("negative magnitude mirrors", const_level(-5000, 9000), -32);
	check_int("full negative", const_level(-10000, 9000), -64);
}

static void
test_direction(void)
{
	check_int("sin 0", t150_dir_sin(0), 0);
	check_int("sin 30 degrees", t150_dir_sin(3000), 16383);
	check_int("sin 45 degrees", t150_dir_sin(4500), 23129);
	check_int("sin east", t150_dir_sin(9000), 32767);
	check_int("sin south", t150_dir_sin(18000), 0);
	check_int("sin west", t150_dir_sin(27000), -32767);
	check_int("sin 330 degrees", t150_dir_sin(33000), -16383);
	check_int("sin wraps", t150_dir_sin(36000 + 9000), 32767);
}

static void
test_constant(void)
{
	struct t150_effect ef;
	uint8_t b[BUFLEN];

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_CONSTANT;
	ef.slot = 0;
	ef.duration = T150_DURATION_INFINITE;
	ef.direction = 9000;
	ef.u.constant.magnitude = 10000;

	/*
	 * Nine bytes and no trailer. Thrustmaster's own driver ends a
	 * constant's ff_first at fade_level, and only a condition carries the
	 * two extra bytes. Sending them here put two bytes the wheel was not
	 * expecting at the head of every upload this project ever tried.
	 */
	CHECK("constant first", b, t150_enc_ff_first(b, sizeof(b), &ef),
	    0x02, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	CHECK("constant update", b, t150_enc_ff_update(b, sizeof(b), &ef),
	    0x03, 0x0e, 0x00, 0x40);
	CHECK("constant commit", b, t150_enc_ff_commit(b, sizeof(b), &ef),
	    0x01, 0x00, 0x00, 0x40, 0xff, 0xff, 0x00, 0x00, 0x00, 0x0e, 0x00,
	    0x1c, 0x00, 0x00, 0x00);

	/* West is the same force the other way, and stays inside an int8. */
	ef.direction = 27000;
	CHECK("constant westward", b, t150_enc_ff_update(b, sizeof(b), &ef),
	    0x03, 0x0e, 0x00, 0xc0);

	/* North has no sideways component at all, so it renders as nothing. */
	ef.direction = 0;
	CHECK("constant northward", b, t150_enc_ff_update(b, sizeof(b), &ef),
	    0x03, 0x0e, 0x00, 0x00);

	CHECK("play once", b, t150_enc_control(b, sizeof(b), 0, 1, 1),
	    0x41, 0x00, 0x41, 0x01);
	CHECK("play five times", b, t150_enc_control(b, sizeof(b), 0, 1, 5),
	    0x41, 0x00, 0x41, 0x05);
	CHECK("stop", b, t150_enc_control(b, sizeof(b), 0, 0, 0),
	    0x41, 0x00, 0x00, 0x01);
}

static void
test_periodic(void)
{
	struct t150_effect ef;
	uint8_t b[BUFLEN];

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_SINE;
	ef.slot = 1;
	ef.duration = 2000000;
	ef.u.periodic.magnitude = 10000;
	ef.u.periodic.phase = 9000;
	ef.u.periodic.period = 20000;

	CHECK("sine update", b, t150_enc_ff_update(b, sizeof(b), &ef),
	    0x04, 0x2a, 0x00, 0x7f, 0x00, 0x40, 0x14, 0x00);
	CHECK("sine commit", b, t150_enc_ff_commit(b, sizeof(b), &ef),
	    0x01, 0x01, 0x22, 0x40, 0xd0, 0x07, 0x00, 0x00, 0x00, 0x2a, 0x00,
	    0x38, 0x00, 0x00, 0x00);

	/*
	 * An envelope, which is the one place the driver is knowingly wrong:
	 * it fills the fade length from the attack length. The fade must
	 * survive the trip unchanged.
	 */
	ef.envelope.present = 1;
	ef.envelope.attack_time = 100000;
	ef.envelope.attack_level = 5000;
	ef.envelope.fade_time = 250000;
	ef.envelope.fade_level = 2500;
	CHECK("sine first with envelope", b, t150_enc_ff_first(b, sizeof(b), &ef),
	    0x02, 0x38, 0x00, 0x64, 0x00, 0x80, 0xfa, 0x00, 0x40);

	ef.kind = T150_EFFECT_SAWTOOTH_UP;
	CHECK("sawtooth up commit", b, t150_enc_ff_commit(b, sizeof(b), &ef),
	    0x01, 0x01, 0x23, 0x40, 0xd0, 0x07, 0x00, 0x00, 0x00, 0x2a, 0x00,
	    0x38, 0x00, 0x00, 0x00);

	ef.kind = T150_EFFECT_SAWTOOTH_DOWN;
	CHECK("sawtooth down commit", b, t150_enc_ff_commit(b, sizeof(b), &ef),
	    0x01, 0x01, 0x24, 0x40, 0xd0, 0x07, 0x00, 0x00, 0x00, 0x2a, 0x00,
	    0x38, 0x00, 0x00, 0x00);
}

static void
test_condition(void)
{
	struct t150_effect ef;
	uint8_t b[BUFLEN];

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_SPRING;
	ef.slot = 2;
	ef.duration = T150_DURATION_INFINITE;
	ef.u.condition.pos_coeff = 10000;
	ef.u.condition.neg_coeff = -10000;
	ef.u.condition.pos_saturation = 10000;
	ef.u.condition.neg_saturation = 10000;

	/* A condition does carry the trailer, and a spring's is 46 54. */
	CHECK("spring first", b, t150_enc_ff_first(b, sizeof(b), &ef),
	    0x05, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x54);

	/*
	 * And an envelope on a condition is dropped, not encoded. DirectInput
	 * lets a game attach one to a spring; the wheel is only ever seen
	 * receiving zeros in those fields.
	 */
	ef.envelope.present = 1;
	ef.envelope.attack_time = 100000;
	ef.envelope.attack_level = 10000;
	ef.envelope.fade_time = 250000;
	ef.envelope.fade_level = 10000;
	CHECK("a condition ignores its envelope", b,
	    t150_enc_ff_first(b, sizeof(b), &ef),
	    0x05, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x54);
	memset(&ef.envelope, 0, sizeof(ef.envelope));
	/* A spring saturates at 0x54, a damper at 0x64. */
	CHECK("spring update", b, t150_enc_ff_update(b, sizeof(b), &ef),
	    0x05, 0x46, 0x00, 0x64, 0x9c, 0x00, 0x00, 0x00, 0x00, 0x54, 0x54);
	CHECK("spring commit", b, t150_enc_ff_commit(b, sizeof(b), &ef),
	    0x01, 0x02, 0x40, 0x40, 0xff, 0xff, 0x00, 0x00, 0x00, 0x46, 0x00,
	    0x54, 0x00, 0x00, 0x00);

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_DAMPER;
	ef.slot = 3;
	ef.duration = T150_DURATION_INFINITE;
	ef.start_delay = 3000000;
	ef.u.condition.center = 2500;
	ef.u.condition.deadband = 1000;
	ef.u.condition.pos_coeff = 5000;
	ef.u.condition.neg_coeff = 5000;
	ef.u.condition.pos_saturation = 5000;
	ef.u.condition.neg_saturation = 5000;

	CHECK("damper update", b, t150_enc_ff_update(b, sizeof(b), &ef),
	    0x05, 0x62, 0x00, 0x32, 0x32, 0x7d, 0x00, 0x64, 0x00, 0x32, 0x32);
	/* The delay field carries the high byte only, so its unit is 256ms. */
	CHECK("damper commit with delay", b, t150_enc_ff_commit(b, sizeof(b), &ef),
	    0x01, 0x03, 0x41, 0x40, 0xff, 0xff, 0x00, 0x00, 0x00, 0x62, 0x00,
	    0x70, 0x00, 0x0b, 0x00);
}

static void
test_downgrades(void)
{
	check_int("square downgrades", t150_effect_downgrade(T150_EFFECT_SQUARE),
	    T150_EFFECT_SINE);
	check_int("triangle downgrades",
	    t150_effect_downgrade(T150_EFFECT_TRIANGLE), T150_EFFECT_SINE);
	check_int("friction downgrades",
	    t150_effect_downgrade(T150_EFFECT_FRICTION), T150_EFFECT_DAMPER);
	check_int("inertia downgrades",
	    t150_effect_downgrade(T150_EFFECT_INERTIA), T150_EFFECT_DAMPER);
	check_int("ramp downgrades", t150_effect_downgrade(T150_EFFECT_RAMP),
	    T150_EFFECT_CONSTANT);

	/* Everything native passes through untouched. */
	check_int("sine stays", t150_effect_downgrade(T150_EFFECT_SINE),
	    T150_EFFECT_SINE);
	check_int("spring stays", t150_effect_downgrade(T150_EFFECT_SPRING),
	    T150_EFFECT_SPRING);
	check_int("constant stays", t150_effect_downgrade(T150_EFFECT_CONSTANT),
	    T150_EFFECT_CONSTANT);
}

static void
test_refusals(void)
{
	struct t150_effect ef;
	uint8_t b[BUFLEN];
	size_t i;

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_CONSTANT;

	/* A short buffer is refused rather than truncated. */
	for (i = 0; i < T150_FF_FIRST_LEN; i++)
		check_int("first refuses a short buffer",
		    (long)t150_enc_ff_first(b, i, &ef), 0);
	for (i = 0; i < T150_FF_COMMIT_LEN; i++)
		check_int("commit refuses a short buffer",
		    (long)t150_enc_ff_commit(b, i, &ef), 0);
	check_int("gain refuses a short buffer",
	    (long)t150_enc_gain(b, 1, 10000), 0);
	check_int("settings refuse a short buffer",
	    (long)t150_enc_range(b, 3, 900), 0);

	/*
	 * A kind the wheel has no type code for is refused, not guessed at.
	 * The caller downgrades first, which is why t150_effect_downgrade
	 * exists.
	 */
	ef.kind = T150_EFFECT_SQUARE;
	check_int("square is refused", (long)t150_enc_ff_first(b, sizeof(b), &ef), 0);
	check_int("square update is refused",
	    (long)t150_enc_ff_update(b, sizeof(b), &ef), 0);
	check_int("square commit is refused",
	    (long)t150_enc_ff_commit(b, sizeof(b), &ef), 0);

	ef.kind = T150_EFFECT_RAMP;
	check_int("ramp is refused", (long)t150_enc_ff_commit(b, sizeof(b), &ef), 0);
	ef.kind = T150_EFFECT_NONE;
	check_int("no effect is refused",
	    (long)t150_enc_ff_commit(b, sizeof(b), &ef), 0);

	/* So is a slot the wheel does not have. */
	ef.kind = T150_EFFECT_CONSTANT;
	ef.slot = T150_SLOT_MAX;
	check_int("bad slot is refused",
	    (long)t150_enc_ff_first(b, sizeof(b), &ef), 0);
	check_int("bad control slot is refused",
	    (long)t150_enc_control(b, sizeof(b), T150_SLOT_MAX, 1, 1), 0);
}

int
main(void)
{
	test_settings();
	test_direction();
	test_constant_level();
	test_constant();
	test_periodic();
	test_condition();
	test_downgrades();
	test_refusals();

	if (failures != 0) {
		fprintf(stderr, "encode_check: %d failure(s)\n", failures);
		return 1;
	}

	printf("encode_check: ok\n");
	return 0;
}
