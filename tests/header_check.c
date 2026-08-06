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
_Static_assert(T150_FF_FIRST_LEN == 9, "ff_first length, constant/periodic");
_Static_assert(T150_FF_FIRST_LEN_CONDITION == 11, "ff_first length, condition");
_Static_assert(T150_FF_COMMIT_LEN == 15, "ff_commit length");
_Static_assert(T150_FF_UPDATE_LEN_CONDITION == 11, "ff_update condition length");
_Static_assert(T150_FF_TYPE_CONSTANT == 0x4000, "constant type code");
_Static_assert(T150_FF_TYPE_SQUARE == 0x4020, "square type code");
_Static_assert(T150_FF_TYPE_TRIANGLE == 0x4021, "triangle type code");
_Static_assert(T150_FF_TYPE_SINE == 0x4022, "sine type code");
_Static_assert(T150_FF_TYPE_SAW_UP == 0x4023, "sawtooth up type code");
_Static_assert(T150_FF_TYPE_SAW_DOWN == 0x4024, "sawtooth down type code");
_Static_assert(T150_FF_TYPE_SPRING == 0x4040, "spring type code");
_Static_assert(T150_FF_TYPE_DAMPER == 0x4041, "damper type code");
_Static_assert(T150_FF_OP_CONTROL == 0x41, "effect control opcode");

/*
 * The input open and close bytes. The driver writes them as little-endian
 * uint16 0x0442 and 0x0042, so the opcode is the low byte and leads on the
 * wire. Transcribing them the other way round would send 04 42.
 */
_Static_assert(T150_GAIN_MAX == 0x80, "gain full scale, not 0xff");
_Static_assert(T150_FF_ENVELOPE_MAX == 0x7f, "envelope level full scale");
_Static_assert(T150_OP_INPUT == 0x42, "input open/close opcode");
_Static_assert(T150_INPUT_OPEN == 0x04, "input open subcommand");
_Static_assert(T150_INPUT_WHAT == 0x05, "the packet sent twice before close");
_Static_assert(T150_INPUT_CLOSE == 0x00, "input close subcommand");

/* Measured on hardware, not transcribed, so worth pinning down. */
_Static_assert(T150_EP_INTR_OUT == 0x01, "interrupt OUT endpoint");
_Static_assert(T150_EP_INTR_IN == 0x82, "interrupt IN endpoint");
_Static_assert(T150_FF_COMMIT_LEN <= T150_EP_INTR_OUT_MAX,
    "the longest packet must fit one interrupt transfer");

/*
 * Square and triangle went back and forth for a long time: recorded as
 * native, withdrawn when no type code could be found for either, and native
 * again once Thrustmaster's own driver named both, 0x4020 and 0x4021.
 * RESEARCH.md A40. Ramp, friction and inertia have no type code at all.
 */
_Static_assert(T150_SUPPORTS_NATIVE(T150_EFFECT_SINE), "sine is native");
_Static_assert(T150_SUPPORTS_NATIVE(T150_EFFECT_DAMPER), "damper is native");
_Static_assert(T150_SUPPORTS_NATIVE(T150_EFFECT_SQUARE), "square is native");
_Static_assert(T150_SUPPORTS_NATIVE(T150_EFFECT_TRIANGLE), "triangle is native");
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
	 * Slots 0 to 8 are what every vendor capture reaches, and slot 8's
	 * 0xfc/0xee appears verbatim in windows_spring0.pcapng. Above that the
	 * keys pass 255, and they used to wrap: slot 9 gave 0x18, which is a
	 * slot 0 key, so two live effects addressed the same block. The
	 * vendor's descriptor declares these sixteen bits wide.
	 */
	expect_pk_id(0, 0x1c, 0x0e);
	expect_pk_id(1, 0x38, 0x2a);
	expect_pk_id(2, 0x54, 0x46);
	expect_pk_id(8, 0xfc, 0xee);
	expect_pk_id(9, 0x118, 0x10a);
	expect_pk_id(T150_SLOT_MAX - 1, 0x1c0, 0x1b2);

	if (failures != 0) {
		fprintf(stderr, "header_check: %d failure(s)\n", failures);
		return 1;
	}

	printf("header_check: ok\n");
	return 0;
}
