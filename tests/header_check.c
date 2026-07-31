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

	if (failures != 0) {
		fprintf(stderr, "header_check: %d failure(s)\n", failures);
		return 1;
	}

	printf("header_check: ok\n");
	return 0;
}
