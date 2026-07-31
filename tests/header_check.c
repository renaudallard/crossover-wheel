/*
 * header_check - compile and sanity check the portable headers.
 *
 * The probe tools only build on macOS, so this is what CI runs on Linux to
 * keep the shared headers honest: it proves they are self contained, that
 * the constants transcribed from the protocol sources still say what the
 * documentation says, and that the one piece of arithmetic in a header
 * produces the values recorded in docs/PROTOCOL.md.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>

#include "t150/effect.h"
#include "t150/proto.h"
#include "t150/t150.h"

/* Transcription errors in these would be silent and expensive. */
_Static_assert(T150_VID == 0x044f, "T150 vendor id");
_Static_assert(T150_PID_BOOT == 0xb65d, "T-series boot product id");
_Static_assert(T150_PID_FIRMWARE == 0xb677, "T150 firmware product id");
_Static_assert(T150_RQ_MODEL_TYPE == 0xc1, "model query bmRequestType");
_Static_assert(T150_RQ_MODEL == 73, "model query bRequest");
_Static_assert(T150_RQ_SWITCH_TYPE == 0x41, "mode switch bmRequestType");
_Static_assert(T150_RQ_SWITCH == 83, "mode switch bRequest");
_Static_assert(T150_SWITCH_VALUE == 0x0006, "T150 switch value");
_Static_assert(T150_OUT_REPORT_ID == 0x0a, "declared output report id");
_Static_assert(T150_OUT_REPORT_LEN == 14, "declared output report length");
_Static_assert(T150_PROTO_HDR_LEN == 8, "wire header length");

/* Force feedback packet shapes and type codes. */
_Static_assert(T150_FF_FIRST_LEN == 11, "ff_first length");
_Static_assert(T150_FF_COMMIT_LEN == 15, "ff_commit length");
_Static_assert(T150_FF_UPDATE_LEN_CONDITION == 11, "ff_update condition length");
_Static_assert(T150_FF_TYPE_CONSTANT == 0x4000, "constant type code");
_Static_assert(T150_FF_TYPE_SINE == 0x4022, "sine type code");
_Static_assert(T150_FF_TYPE_SAW_UP == 0x4023, "sawtooth up type code");
_Static_assert(T150_FF_TYPE_SAW_DOWN == 0x4024, "sawtooth down type code");
_Static_assert(T150_FF_TYPE_SPRING == 0x4040, "spring type code");
_Static_assert(T150_FF_TYPE_DAMPER == 0x4041, "damper type code");
_Static_assert(T150_FF_OP_CONTROL == 0x41, "effect control opcode");

/*
 * Square and triangle read like periodics the wheel ought to render, and
 * were once recorded as native here. The protocol has no type code for
 * either, so pin them down.
 */
_Static_assert(T150_SUPPORTS_NATIVE(T150_EFFECT_SINE), "sine is native");
_Static_assert(T150_SUPPORTS_NATIVE(T150_EFFECT_DAMPER), "damper is native");
_Static_assert(!T150_SUPPORTS_NATIVE(T150_EFFECT_SQUARE), "square is not native");
_Static_assert(!T150_SUPPORTS_NATIVE(T150_EFFECT_TRIANGLE), "triangle is not native");
_Static_assert(!T150_SUPPORTS_NATIVE(T150_EFFECT_RAMP), "ramp is not native");
_Static_assert(!T150_SUPPORTS_NATIVE(T150_EFFECT_FRICTION), "friction is not native");
_Static_assert(!T150_SUPPORTS_NATIVE(T150_EFFECT_INERTIA), "inertia is not native");

static int failures;

static void
expect_range(unsigned int degrees, unsigned int want)
{
	unsigned int got = t150_range_arg(degrees);

	if (got != want) {
		fprintf(stderr, "FAIL t150_range_arg(%u): want 0x%04x, "
		    "got 0x%04x\n", degrees, want, got);
		failures++;
	}
}

static void
expect_pk_id(unsigned int slot, unsigned int want0, unsigned int want1)
{
	unsigned int got0 = t150_ff_pk_id0(slot);
	unsigned int got1 = t150_ff_pk_id1(slot);

	if (got0 != want0) {
		fprintf(stderr, "FAIL t150_ff_pk_id0(%u): want 0x%02x, "
		    "got 0x%02x\n", slot, want0, got0);
		failures++;
	}
	if (got1 != want1) {
		fprintf(stderr, "FAIL t150_ff_pk_id1(%u): want 0x%02x, "
		    "got 0x%02x\n", slot, want1, got1);
		failures++;
	}
}

int
main(void)
{
	/* 1080 degrees is full scale, everything else scales linearly. */
	expect_range(0, 0x0000);
	expect_range(270, 16383);
	expect_range(540, 32767);
	expect_range(900, 54612);
	expect_range(1080, 0xffff);

	/* Anything above the hardware maximum clamps rather than wrapping. */
	expect_range(1081, 0xffff);
	expect_range(65535, 0xffff);

	/*
	 * Slot keys. Both fields are one byte on the wire, so the high slots
	 * wrap. That is inherited from the driver rather than chosen, and the
	 * two families still never collide: 28k+28 and 28j+14 stay distinct
	 * modulo 256 for every slot below T150_SLOT_MAX.
	 */
	expect_pk_id(0, 0x1c, 0x0e);
	expect_pk_id(1, 0x38, 0x2a);
	expect_pk_id(2, 0x54, 0x46);
	expect_pk_id(8, 0xfc, 0xee);
	expect_pk_id(9, 0x18, 0x0a);
	expect_pk_id(T150_SLOT_MAX - 1, 0xc0, 0xb2);

	if (failures != 0) {
		fprintf(stderr, "header_check: %d failure(s)\n", failures);
		return 1;
	}

	printf("header_check: ok\n");
	return 0;
}
