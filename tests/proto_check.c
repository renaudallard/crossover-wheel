/*
 * proto_check - the DLL to daemon wire format.
 *
 * The two ends of this protocol are built by different compilers for
 * different platforms, so the only thing keeping them agreeing is that
 * neither ever writes a struct to a socket. These tests hold the byte
 * layout still.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>

#include "t150/proto.h"

static int failures;

static void
fail(const char *what)
{
	fprintf(stderr, "FAIL %s\n", what);
	failures++;
}

static void
check_bytes(const char *what, const uint8_t *got, size_t gotlen,
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

static void
test_header(void)
{
	static const uint8_t want[] = {
		0x30, 0x35, 0x31, 0x54,	/* magic, little endian */
		0x01,			/* version */
		0x03,			/* op, EFFECT_UPLOAD */
		0x3b, 0x00		/* length, 59 */
	};
	struct t150_proto_hdr hdr, back;
	uint8_t buf[16];
	size_t n;

	hdr.magic = T150_PROTO_MAGIC;
	hdr.version = T150_PROTO_VERSION;
	hdr.op = T150_OP_EFFECT_UPLOAD;
	hdr.length = T150_PROTO_EFFECT_LEN;

	n = t150_proto_pack_hdr(buf, sizeof(buf), &hdr);
	check_bytes("header bytes", buf, n, want, sizeof(want));

	if (t150_proto_unpack_hdr(buf, n, &back) != 0)
		fail("header does not round trip");
	else if (back.magic != hdr.magic || back.version != hdr.version ||
	    back.op != hdr.op || back.length != hdr.length)
		fail("header round trips to different values");

	/* A short buffer is refused at both ends. */
	if (t150_proto_pack_hdr(buf, T150_PROTO_HDR_LEN - 1, &hdr) != 0)
		fail("pack accepted a short buffer");
	if (t150_proto_unpack_hdr(buf, T150_PROTO_HDR_LEN - 1, &back) == 0)
		fail("unpack accepted a short buffer");

	/* So is a foreign magic, which is the only framing check available. */
	buf[0] ^= 0xff;
	if (t150_proto_unpack_hdr(buf, sizeof(buf), &back) == 0)
		fail("unpack accepted a bad magic");
	buf[0] ^= 0xff;

	/* And a length this protocol could never have produced. */
	buf[6] = 0xff;
	buf[7] = 0xff;
	if (t150_proto_unpack_hdr(buf, sizeof(buf), &back) == 0)
		fail("unpack accepted an impossible length");

	hdr.length = T150_PROTO_MAX_PAYLOAD + 1;
	if (t150_proto_pack_hdr(buf, sizeof(buf), &hdr) != 0)
		fail("pack accepted an impossible length");
}

static void
roundtrip(const char *what, const struct t150_effect *ef)
{
	struct t150_effect back;
	uint8_t buf[T150_PROTO_EFFECT_LEN];

	if (t150_proto_pack_effect(buf, sizeof(buf), ef) !=
	    T150_PROTO_EFFECT_LEN) {
		fail(what);
		return;
	}
	if (t150_proto_unpack_effect(buf, sizeof(buf), &back) != 0) {
		fail(what);
		return;
	}

	/*
	 * Compared field by field rather than with memcmp, because the union
	 * carries whichever member the kind selects and the rest is padding
	 * this protocol deliberately does not transport.
	 */
	if (back.kind != ef->kind || back.slot != ef->slot ||
	    back.duration != ef->duration ||
	    back.start_delay != ef->start_delay || back.gain != ef->gain ||
	    back.direction != ef->direction ||
	    back.envelope.attack_time != ef->envelope.attack_time ||
	    back.envelope.attack_level != ef->envelope.attack_level ||
	    back.envelope.fade_time != ef->envelope.fade_time ||
	    back.envelope.fade_level != ef->envelope.fade_level ||
	    back.envelope.present != ef->envelope.present) {
		fail(what);
		return;
	}

	switch (ef->kind) {
	case T150_EFFECT_CONSTANT:
		if (back.u.constant.magnitude != ef->u.constant.magnitude)
			fail(what);
		break;
	case T150_EFFECT_RAMP:
		if (back.u.ramp.start != ef->u.ramp.start ||
		    back.u.ramp.end != ef->u.ramp.end)
			fail(what);
		break;
	case T150_EFFECT_SQUARE:
	case T150_EFFECT_SINE:
	case T150_EFFECT_TRIANGLE:
	case T150_EFFECT_SAWTOOTH_UP:
	case T150_EFFECT_SAWTOOTH_DOWN:
		if (back.u.periodic.magnitude != ef->u.periodic.magnitude ||
		    back.u.periodic.offset != ef->u.periodic.offset ||
		    back.u.periodic.phase != ef->u.periodic.phase ||
		    back.u.periodic.period != ef->u.periodic.period)
			fail(what);
		break;
	case T150_EFFECT_SPRING:
	case T150_EFFECT_DAMPER:
	case T150_EFFECT_FRICTION:
	case T150_EFFECT_INERTIA:
		if (back.u.condition.center != ef->u.condition.center ||
		    back.u.condition.pos_coeff != ef->u.condition.pos_coeff ||
		    back.u.condition.neg_coeff != ef->u.condition.neg_coeff ||
		    back.u.condition.pos_saturation !=
		    ef->u.condition.pos_saturation ||
		    back.u.condition.neg_saturation !=
		    ef->u.condition.neg_saturation ||
		    back.u.condition.deadband != ef->u.condition.deadband)
			fail(what);
		break;
	default:
		break;
	}
}

static void
test_effects(void)
{
	struct t150_effect ef;

	memset(&ef, 0, sizeof(ef));
	ef.slot = 3;
	ef.duration = 1500000;
	ef.start_delay = 250000;
	ef.gain = 7500;
	ef.direction = 27000;
	ef.envelope.present = 1;
	ef.envelope.attack_time = 100000;
	ef.envelope.attack_level = 4000;
	ef.envelope.fade_time = 200000;
	ef.envelope.fade_level = 1000;

	ef.kind = T150_EFFECT_CONSTANT;
	ef.u.constant.magnitude = -10000;
	roundtrip("constant round trip", &ef);

	ef.kind = T150_EFFECT_RAMP;
	ef.u.ramp.start = -10000;
	ef.u.ramp.end = 10000;
	roundtrip("ramp round trip", &ef);

	ef.kind = T150_EFFECT_SINE;
	ef.u.periodic.magnitude = -7500;
	ef.u.periodic.offset = 2500;
	ef.u.periodic.phase = 18000;
	ef.u.periodic.period = 33333;
	roundtrip("sine round trip", &ef);

	ef.kind = T150_EFFECT_SQUARE;
	roundtrip("square round trip", &ef);

	ef.kind = T150_EFFECT_SPRING;
	ef.u.condition.center = -2500;
	ef.u.condition.pos_coeff = 10000;
	ef.u.condition.neg_coeff = -10000;
	ef.u.condition.pos_saturation = 8000;
	ef.u.condition.neg_saturation = 6000;
	ef.u.condition.deadband = 500;
	roundtrip("spring round trip", &ef);

	ef.kind = T150_EFFECT_INERTIA;
	roundtrip("inertia round trip", &ef);

	/* An infinite duration is a value, not a sentinel to be clamped. */
	ef.kind = T150_EFFECT_CONSTANT;
	ef.duration = T150_DURATION_INFINITE;
	ef.u.constant.magnitude = 10000;
	roundtrip("infinite duration round trip", &ef);

	/* Extremes, because signed fields cross the wire as two's complement. */
	ef.envelope.attack_level = INT32_MIN;
	ef.envelope.fade_level = INT32_MAX;
	ef.u.constant.magnitude = INT32_MIN;
	roundtrip("extreme values round trip", &ef);

	/* A kind the wheel cannot render still has to survive the trip, so
	 * that the daemon rather than the codec decides what to do. */
	ef.kind = 200;
	roundtrip("unknown kind round trips", &ef);

	if (t150_proto_pack_effect(NULL, 0, &ef) != 0)
		fail("pack accepted a short effect buffer");
}

int
main(void)
{
	test_header();
	test_effects();

	if (failures != 0) {
		fprintf(stderr, "proto_check: %d failure(s)\n", failures);
		return 1;
	}

	printf("proto_check: ok\n");
	return 0;
}
