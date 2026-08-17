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

#include <stddef.h>
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
 * And what the wheel is when the selector on its base is in the PS4 position
 * instead of the PS3 one. Nothing here drives it there: it takes no mode
 * switch, it has a different report descriptor, and RESEARCH.md D7 is why
 * driving it there is the wrong road.
 *
 * It is named because it is worth telling apart from an empty bus. A wheel in
 * this position matches neither id above, so anything looking only for those
 * two reports that no wheel is plugged in while one is sitting there rigid,
 * which is a diagnosis nobody can make from that sentence.
 */
#define T150_PID_PS4		0xb66du	/* "Thrustmaster Racing Wheel FFB" */

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
 * The last two bytes of a condition upload, and they are not a trailer at
 * all. Thrustmaster's descriptor names them: they are the positive and
 * negative saturation of the parameter block the packet addresses, the same
 * two fields ff_update writes at bytes 9 and 10. A condition effect is two
 * parameter blocks, so it is two of these packets rather than an envelope
 * and a trailer, and damper0.pcapng shows the pair varying across slots,
 * which no fixed trailer could do.
 *
 * The values stay as they are: 0x46 0x54 for a spring and 0x64 0x64 for a
 * damper reproduce the vendor's own bytes exactly, and 0x54 and 0x64 are the
 * spring and damper saturation maxima below. Only the model was wrong.
 *
 * Measured: traffic/ffb/windows/spring0.pcapng and traffic/ffb/damper0.pcapng,
 * named by tmHidUsb.sys's report descriptor. RESEARCH.md A40.
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
 * ff_commit effect type codes, all eight taken from Thrustmaster's own
 * Windows driver. Its HID PID report descriptor declares the effect type
 * array as constant, square, triangle, sine, sawtooth up, sawtooth down,
 * spring, damper, and it indexes a nine byte table with that position to get
 * the low byte on the wire. The order is not numeric and could not have been
 * guessed. RESEARCH.md A40.
 *
 * That table has no ninth entry, which is why 0x4025 renders nothing on
 * hardware: it is not a waveform, it is off the end.
 */
#define T150_FF_TYPE_CONSTANT	0x4000u
#define T150_FF_TYPE_SQUARE	0x4020u
#define T150_FF_TYPE_TRIANGLE	0x4021u
#define T150_FF_TYPE_SINE	0x4022u
#define T150_FF_TYPE_SAW_UP	0x4023u
#define T150_FF_TYPE_SAW_DOWN	0x4024u
#define T150_FF_TYPE_SPRING	0x4040u
#define T150_FF_TYPE_DAMPER	0x4041u

/*
 * Wire ranges for the effect parameter fields. Each was derived from the
 * divisor t150_driver applies to a full scale Linux force feedback value,
 * and each is now confirmed against Thrustmaster's own Windows driver: the
 * four divisors 0x147, 0x030c, 0x028f and 0x01ff appear in it as literal
 * constants, and its report descriptor declares the coefficient as -100 to
 * 100, the centre as -500 to 500 and the deadband as 0 to 1000, which is
 * exactly what these say. RESEARCH.md A40.
 *
 * The constant level stops at 64 while a periodic magnitude reaches 127, and
 * the asymmetry is real but not a limit of the field: both are one signed
 * byte. The vendor halves constant force on this model before it reaches the
 * wire, so 64 is what a full scale constant becomes, and its own captures
 * step 0x40, 0x20, 0x10 for 100, 50 and 25 percent. Keep 64: it reproduces
 * the vendor's bytes.
 */
#define T150_FF_LEVEL_MAX	64u	/* constant, from /0x01ff on an int16 */
#define T150_FF_PERIODIC_MAX	127u	/* periodic magnitude and offset, from >>8 */
#define T150_FF_PHASE_MAX	0xffu	/* a full turn */
#define T150_FF_COEFF_MAX	100u	/* from /0x147 */

/*
 * How far a condition coefficient is actually allowed to go, which is not as
 * far as the wheel will take.
 *
 * The T150's own damper loop is unstable at the top of its range. Measured on
 * hardware with no game, no daemon and no proxy, one raw packet: at 100 the
 * wheel buzzes wherever it is left standing, at 99 it still buzzes, at 90 it
 * buzzes more slowly, and at 80 it stops. An oscillation that slows as the
 * gain falls is a gain dependent limit cycle rather than a resonance, so this
 * is the wheel fighting itself. RESEARCH.md A46.
 *
 * This is a workaround and not a correction. The encoding is right: the
 * vendor's own divisor takes the same request to 100 too, so the wheel
 * dislikes a value that is faithfully encoded, and clamping here hands a game
 * less than it asked for. What justifies it is that the value cannot be
 * reached from the game's own settings, so nobody can work around it: Assetto
 * Corsa asks for 9998 of 10000 and its force feedback page has no damping
 * control at all.
 *
 * Clamped rather than rescaled, so everything below the ceiling is untouched
 * and only the top of the range is flattened. The tester's verdict on the
 * difference at full request: "about the same, maybe a little less hard".
 */
#define T150_FF_COEFF_SAFE_MAX	80
#define T150_FF_CENTER_MAX	500u	/* from /(0x7fff / 0x01f4) */
#define T150_FF_DEADBAND_MAX	1000u	/* from /(0xffff / 0x03e8) */
#define T150_FF_SAT_SPRING_MAX	0x54u	/* from /0x030c */
#define T150_FF_SAT_DAMPER_MAX	0x64u	/* from /0x028f */

/*
 * Bounds the vendor's descriptor declares that the Linux driver never
 * mentioned. Envelope times and a periodic's period are 0 to 10000 ms, and
 * a control packet's iteration count stops one short of a full byte.
 */
#define T150_FF_TIME_MAX	10000u
#define T150_FF_LOOP_MAX	254u

/*
 * Envelope levels are one byte each, running to 127 rather than 255. This
 * was 0xff on a guess, the Linux driver's own comment admitting its scaling
 * of them was wrong. Thrustmaster's descriptor settles it: attack level and
 * fade level are both declared 09 5b 25 7f and 09 5d 25 7f, logical maximum
 * 127 against a physical maximum of 10000. RESEARCH.md A40.
 */
#define T150_FF_ENVELOPE_MAX	0x7fu

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
 * active while no input are open".
 *
 * Force feedback is gated on it too, and that was the open question these
 * exist to answer: the wheel renders nothing until something sends the open.
 * RESEARCH.md A28. Nothing on macOS sends it on a daemon's behalf, so t150d
 * sends it itself and holds it open for as long as it holds the wheel, which
 * is also what leaves the pedals resting where a game expects them. That makes
 * the enable flag load bearing rather than decorative: A15 measured it a no-op
 * only because nothing had the input open when it was measured.
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

/*
 * The two slot keys that correlate an effect's three upload packets. They
 * are parameter block offsets into the wheel's own effect memory, 28 bytes
 * per slot, and they are sixteen bits wide: the vendor's descriptor declares
 * report size 16 for them and its driver stores them as words. These
 * returned uint8_t, which was invisible up to slot 8, the highest any
 * capture reaches, and wrapped from slot 9 on, where 0x11a became 0x1a and
 * collided with an earlier slot. RESEARCH.md A40.
 */
static inline uint16_t
t150_ff_pk_id0(unsigned int slot)
{
	return (uint16_t)(slot * 0x1cu + 0x1cu);
}

static inline uint16_t
t150_ff_pk_id1(unsigned int slot)
{
	return (uint16_t)(slot * 0x1cu + 0x0eu);
}

/*
 * Rotation range accepted by the firmware. The wheel caps anything smaller
 * than the minimum itself, which is why the minimum is 270 rather than 0;
 * every tool here refuses a number outside these bounds rather than sending
 * one and relying on that, so the range they advertise is the range they
 * keep.
 */
#define T150_RANGE_MIN		270u
#define T150_RANGE_MAX		1080u

/*
 * Scale a rotation range in degrees to the wheel's 16-bit argument.
 *
 * The vendor does not scale. Its driver switches on six discrete ranges and
 * stores a literal token for each beside the degrees it means, which is what
 * `mov eax, 0xd555` followed by `mov [rsi+0x68c], 0x384` is at 0x14004a8f5:
 * the token and 900. The three tokens that appear as literals rather than
 * through a register are 0x3fff for 270, 0x5555 for 360 and 0xd555 for 900.
 *
 * The scaling below agrees with two of those and misses the third: 900 is
 * exactly 54612.5 parts of 0xffff and truncating gives 0xd554 where the
 * vendor sends 0xd555. Rounding instead would fix 900 and break 270, whose
 * exact value is 16383.75 against the vendor's 16383. So neither is the
 * vendor's rule, and the table is.
 *
 * The table wins for a range the vendor names, because a firmware that
 * dispatches on equality would see 0xd554 as no range at all rather than as
 * one a fraction of a degree out, and 900 is the range people ask for. The
 * scaling stays for anything else, because refusing a number the wheel might
 * well accept is worse than approximating it, and nothing has measured which
 * it does. Whether the firmware takes anything but the six is unknown.
 */
static inline uint16_t
t150_range_arg(unsigned int degrees)
{
	static const struct {
		unsigned int	degrees;
		uint16_t	arg;
	} vendor[] = {
		{ 270, 0x3fffu }, { 360, 0x5555u }, { 900, 0xd555u }
	};
	size_t i;

	if (degrees > T150_RANGE_MAX)
		degrees = T150_RANGE_MAX;

	for (i = 0; i < sizeof(vendor) / sizeof(vendor[0]); i++) {
		if (vendor[i].degrees == degrees)
			return vendor[i].arg;
	}

	return (uint16_t)(((uint32_t)degrees * 0xffffu) / T150_RANGE_MAX);
}

#endif /* T150_H */
