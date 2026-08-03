/*
 * t150.h - Thrustmaster T150 wire constants.
 *
 * Every value here is transcribed from a cited source. Nothing in this
 * header is inferred. See docs/PROTOCOL.md for the derivation and for the
 * points that are still unconfirmed against real hardware.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef T150_H
#define T150_H

#include <stdint.h>

/*
 * USB identity. All T-series wheels enumerate at the shared boot PID and
 * only reveal their model-specific firmware PID after the mode switch below.
 * Source: scarburato/hid-tminit, linux drivers/hid/hid-thrustmaster.c.
 */
#define T150_VID		0x044fu
#define T150_PID_BOOT		0xb65du	/* "Thrustmaster FFB Wheel" */
#define T150_PID_FIRMWARE	0xb677u

/*
 * Boot to firmware mode switch, two vendor control transfers on endpoint 0.
 *
 * 1. model query:  bmRequestType 0xC1, bRequest 73, wLength 16.
 *    Response byte 6 is the attachment, byte 7 is the model.
 * 2. mode switch:  bmRequestType 0x41, bRequest 83, wValue = switch value,
 *    no data stage. The wheel detaches and re-enumerates.
 *
 * Source: linux drivers/hid/hid-thrustmaster.c, which issues exactly these
 * two requests via usb_fill_control_urb().
 */
#define T150_RQ_MODEL_TYPE	0xc1u
#define T150_RQ_MODEL		73u
#define T150_RQ_MODEL_LEN	16u
#define T150_RQ_MODEL_OFF_ATTACH 6u
#define T150_RQ_MODEL_OFF_MODEL	7u

#define T150_RQ_SWITCH_TYPE	0x41u
#define T150_RQ_SWITCH		83u

/* The T150 row of hid-tminit's model table. */
#define T150_MODEL		0x03u
#define T150_ATTACHMENT		0x06u
#define T150_SWITCH_VALUE	0x0006u

/*
 * USB endpoints in firmware mode. Measured on hardware by probe_intr, which
 * enumerates the pipes on interface 0 and prints them: the wheel has exactly
 * two, and the OUT one takes 32 bytes.
 *
 * This settles an old disagreement. t150_driver discovers the OUT endpoint
 * at runtime, its own traffic/old_caps/t150_test.py writes to 0x01, and
 * macoswheels recorded 0x02. It is 0x01. The IN address was recorded here as
 * 0x81 on no better authority than assumption, and is 0x82.
 */
#define T150_EP_INTR_IN		0x82u
#define T150_EP_INTR_OUT	0x01u
#define T150_EP_INTR_OUT_MAX	32u

/*
 * HID output report declared by the wheel's own report descriptor.
 *
 * Decoded from t150_driver/traffic/old_caps/hid_report_fw35 (firmware 3.5):
 *
 *   85 0A           Report ID (0x0A)
 *   06 00 FF        Usage Page (vendor 0xFF00)
 *   09 0A           Usage (0x0A)
 *   75 08 95 0E     Report Size 8, Report Count 14
 *   26 FF 00        Logical Maximum 255
 *   46 FF 00        Physical Maximum 255
 *   91 02           Output (Data, Var, Abs)
 *
 * This is the report IOHIDDeviceSetReport() would address. It is NOT how
 * the Linux driver talks to the wheel: hid-t150 bypasses the HID layer and
 * writes raw on the interrupt OUT pipe with no report ID prefix. Whether
 * the firmware accepts the HID-framed form is exactly what probe_setreport
 * exists to answer.
 */
#define T150_OUT_REPORT_ID	0x0au
#define T150_OUT_REPORT_LEN	14u

/*
 * Settings packets. Every setting except gain shares one 4-byte form:
 *
 *   [0x40, op, arg_lo, arg_hi]      little-endian uint16 argument
 *
 * Source: scarburato/t150_driver hid-t150/settings.c.
 */
#define T150_OP_SETTINGS	0x40u
#define T150_OP_AUTOCENTER_FORCE	0x03u	/* arg 0..100, a hardware percent */
#define T150_OP_AUTOCENTER_ENABLE	0x04u	/* arg 0 = off, 1 = on */
#define T150_OP_RANGE			0x11u	/* arg = degrees * 0xFFFF / 1080 */

#define T150_OP_GAIN		0x43u	/* [0x43, gain], one byte, not two */

/*
 * Force feedback packets. An effect uploads as ff_first, then ff_update,
 * then ff_commit, and is started or stopped by a separate control packet.
 * Source: scarburato/t150_driver hid-t150/forcefeedback.{c,h}. See
 * docs/PROTOCOL.md for the field-by-field layouts.
 */
/*
 * ff_first is 9 bytes for a constant or a periodic and 11 for a condition,
 * the extra two being a trailer that only conditions carry. Measured in
 * Thrustmaster's own Windows driver: traffic/ffb/windows/constant0.pcapng
 * sends "02 1c 00 e8 03 02 e8 03 01" and stops, while its spring capture
 * sends "05 1c 00 00 00 00 00 00 00 46 54".
 */
#define T150_FF_FIRST_LEN		9u
#define T150_FF_FIRST_LEN_CONDITION	11u
#define T150_FF_COMMIT_LEN	15u
#define T150_FF_CONTROL_LEN	4u

/* ff_first byte 0, the effect class. Constant and periodic share a code. */
#define T150_FF_FIRST_CONSTANT	0x02u
#define T150_FF_FIRST_PERIODIC	0x02u
#define T150_FF_FIRST_CONDITION	0x05u

/*
 * The condition trailer, and it is not one constant pair. Spring uploads end
 * 0x46 0x54, damper uploads end 0x64 0x64, and 0x54 and 0x64 are exactly the
 * spring and damper saturation maxima below. So the trailer looks like a
 * saturation hint keyed to the effect type rather than the magic numbers the
 * driver hardcodes, which sends 0x46 0x54 for both.
 *
 * Measured: traffic/ffb/windows/spring0.pcapng and traffic/ffb/damper0.pcapng.
 */
#define T150_FF_FIRST_F2_SPRING	0x46u
#define T150_FF_FIRST_F3_SPRING	0x54u
#define T150_FF_FIRST_F2_DAMPER	0x64u
#define T150_FF_FIRST_F3_DAMPER	0x64u

/* ff_update byte 0, a different class encoding from ff_first. */
#define T150_FF_UPDATE_CONSTANT		0x03u
#define T150_FF_UPDATE_PERIODIC		0x04u
#define T150_FF_UPDATE_CONDITION	0x05u

/* ff_update total length, which varies with the class. */
#define T150_FF_UPDATE_LEN_CONSTANT	4u
#define T150_FF_UPDATE_LEN_PERIODIC	8u
#define T150_FF_UPDATE_LEN_CONDITION	11u

/* ff_commit byte 0, and its endless-duration marker. */
#define T150_FF_COMMIT_F0	0x01u
#define T150_FF_LENGTH_INFINITE	0xffffu

/*
 * ff_commit effect type codes. The driver implements no square or triangle
 * and declares no code for either. The codes are contiguous around the
 * periodics, so 0x4020, 0x4021 and 0x4025 may be the missing waveforms, but
 * that is a guess and is not used until hardware confirms it.
 */
#define T150_FF_TYPE_CONSTANT	0x4000u
#define T150_FF_TYPE_SINE	0x4022u
#define T150_FF_TYPE_SAW_UP	0x4023u
#define T150_FF_TYPE_SAW_DOWN	0x4024u
#define T150_FF_TYPE_SPRING	0x4040u
#define T150_FF_TYPE_DAMPER	0x4041u

/*
 * Wire ranges for the effect parameter fields. Each is derived from the
 * divisor t150_driver applies to a full scale Linux force feedback value,
 * which is how that driver encodes the wheel's real limits: the condition
 * ones land exactly on the maxima its own struct comments document, so the
 * derivation is sound.
 *
 * The constant level stops at 64 while a periodic magnitude reaches 127,
 * which is asymmetric and may simply be conservative. Only hardware can say,
 * and probe_setreport can ask it directly.
 */
#define T150_FF_LEVEL_MAX	64u	/* constant, from /0x01ff on an int16 */
#define T150_FF_PERIODIC_MAX	127u	/* periodic magnitude and offset, from >>8 */
#define T150_FF_PHASE_MAX	0xffu	/* a full turn */
#define T150_FF_COEFF_MAX	100u	/* from /0x147 */
#define T150_FF_CENTER_MAX	500u	/* from /(0x7fff / 0x01f4) */
#define T150_FF_DEADBAND_MAX	1000u	/* from /(0xffff / 0x03e8) */
#define T150_FF_SAT_SPRING_MAX	0x54u	/* from /0x030c */
#define T150_FF_SAT_DAMPER_MAX	0x64u	/* from /0x028f */

/*
 * Envelope levels are one byte each and the driver's own comment says its
 * scaling of them is wrong, so full scale is a guess rather than a
 * transcription. See docs/PROTOCOL.md.
 */
#define T150_FF_ENVELOPE_MAX	0xffu

/*
 * Gain full scale is 0x80, not 0xff. The driver's original setter documented
 * "a value between 0x00 and 0x80 where 0x80 is 100% gain" and passed 0x66 as
 * its "~80%" default, which is 102/128 = 79.7%, and the one capture of a
 * working session sets 0x43 0x80.
 *
 * The current driver reads 0..0xffff and assigns it straight into a uint8,
 * which truncates: its "~75%" default of 0xbffe leaves 0xfe on the wire,
 * nearly double full scale. That is an upstream regression from commit
 * 0e7c85f, February 2025, and not something to reproduce.
 */
#define T150_GAIN_MAX		0x80u
#define T150_AUTOCENTER_MAX	100u

/*
 * Opening and closing the wheel's input, two bytes each on the interrupt OUT
 * pipe. The firmware tracks whether an application has the input open, which
 * is why t150_set_enable_autocenter's comment says the autocenter "is always
 * active while no input are open": nothing on macOS opens it, so it always is.
 * Whether the wheel also gates force feedback on this is the open question
 * these exist to answer.
 *
 * Source: scarburato/t150_driver hid-t150/hid-t150.c, which allocates the
 * three packets in t150_init() as little-endian uint16 0x0442, 0x0542 and
 * 0x0042, and hid-t150/input.c, which sends them with usb_interrupt_msg() on
 * pipe_out with length 2: open on t150_input_open(), then "what" twice and
 * close on t150_input_close().
 */
#define T150_OP_INPUT		0x42u
#define T150_INPUT_OPEN		0x04u
#define T150_INPUT_WHAT		0x05u	/* sent twice, before close */
#define T150_INPUT_CLOSE	0x00u

/* Control packet: [0x41, slot, mode, times]. There is no erase packet. */
#define T150_FF_OP_CONTROL	0x41u
#define T150_FF_CTRL_PLAY	0x41u
#define T150_FF_CTRL_STOP	0x00u

/* The two slot keys that correlate an effect's three upload packets. */
static inline uint8_t
t150_ff_pk_id0(unsigned int slot)
{
	return (uint8_t)(slot * 0x1cu + 0x1cu);
}

static inline uint8_t
t150_ff_pk_id1(unsigned int slot)
{
	return (uint8_t)(slot * 0x1cu + 0x0eu);
}

/* Rotation range accepted by the firmware. Values below the minimum are
 * capped by the wheel itself. */
#define T150_RANGE_MIN		270u
#define T150_RANGE_MAX		1080u

/* Scale a rotation range in degrees to the wheel's 16-bit argument. */
static inline uint16_t
t150_range_arg(unsigned int degrees)
{
	if (degrees > T150_RANGE_MAX)
		degrees = T150_RANGE_MAX;
	return (uint16_t)(((uint32_t)degrees * 0xffffu) / T150_RANGE_MAX);
}

#endif /* T150_H */
