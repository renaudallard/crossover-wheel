/*
 * encode.c - normalized effects to T150 wire packets.
 *
 * Layouts and scaling come from docs/PROTOCOL.md, which transcribes
 * scarburato/t150_driver. Nothing here talks to a device or allocates, so
 * every byte this file produces is checked by tests/encode_check.c on any
 * machine.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>

#include "t150/encode.h"
#include "t150/t150.h"

#define SETTINGS_LEN	4
#define GAIN_LEN	2
#define INPUT_LEN	2

/*
 * Scale a signed DirectInput value onto a signed wire range, rounding to
 * nearest and clamping rather than wrapping. A game is free to send a
 * magnitude outside the documented range and must not be able to turn a
 * strong push into a weak one in the other direction by doing so.
 */
static int32_t
scale_signed(int32_t v, int32_t in_max, int32_t out_max)
{
	int64_t n;

	if (v > in_max)
		v = in_max;
	else if (v < -in_max)
		v = -in_max;

	n = (int64_t)v * out_max;
	n += (v < 0 ? -in_max : in_max) / 2;

	return (int32_t)(n / in_max);
}

/* The same for a value that has no negative half. */
static uint32_t
scale_unsigned(uint32_t v, uint32_t in_max, uint32_t out_max)
{
	if (v > in_max)
		v = in_max;

	return (uint32_t)(((uint64_t)v * out_max + in_max / 2) / in_max);
}

/* Durations cross the wire in milliseconds and are capped, never wrapped. */
static uint16_t
us_to_ms(uint32_t us)
{
	if (us >= (uint32_t)0xffff * 1000)
		return 0xffff;

	return (uint16_t)((us + 500) / 1000);
}

static void
put_le16(uint8_t *buf, uint16_t v)
{
	buf[0] = (uint8_t)(v & 0xff);
	buf[1] = (uint8_t)(v >> 8);
}

/* The effect class byte, which ff_first and ff_update encode differently. */
static int
first_class(uint8_t kind, uint8_t *out)
{
	switch (kind) {
	case T150_EFFECT_CONSTANT:
		*out = T150_FF_FIRST_CONSTANT;
		return 0;
	case T150_EFFECT_SINE:
	case T150_EFFECT_SAWTOOTH_UP:
	case T150_EFFECT_SAWTOOTH_DOWN:
		*out = T150_FF_FIRST_PERIODIC;
		return 0;
	case T150_EFFECT_SPRING:
	case T150_EFFECT_DAMPER:
		*out = T150_FF_FIRST_CONDITION;
		return 0;
	default:
		return -1;
	}
}

static int
commit_type(uint8_t kind, uint16_t *out)
{
	switch (kind) {
	case T150_EFFECT_CONSTANT:
		*out = T150_FF_TYPE_CONSTANT;
		return 0;
	case T150_EFFECT_SINE:
		*out = T150_FF_TYPE_SINE;
		return 0;
	case T150_EFFECT_SAWTOOTH_UP:
		*out = T150_FF_TYPE_SAW_UP;
		return 0;
	case T150_EFFECT_SAWTOOTH_DOWN:
		*out = T150_FF_TYPE_SAW_DOWN;
		return 0;
	case T150_EFFECT_SPRING:
		*out = T150_FF_TYPE_SPRING;
		return 0;
	case T150_EFFECT_DAMPER:
		*out = T150_FF_TYPE_DAMPER;
		return 0;
	default:
		return -1;
	}
}

uint8_t
t150_effect_downgrade(uint8_t kind)
{
	switch (kind) {
	case T150_EFFECT_SQUARE:
	case T150_EFFECT_TRIANGLE:
		return T150_EFFECT_SINE;
	case T150_EFFECT_FRICTION:
	case T150_EFFECT_INERTIA:
		return T150_EFFECT_DAMPER;
	case T150_EFFECT_RAMP:
		return T150_EFFECT_CONSTANT;
	default:
		return kind;
	}
}

/*
 * Bhaskara's sine approximation, which stays within a fifth of a percent of
 * the real thing over the whole turn and needs no table and no floating
 * point. In hundredths of a degree, with h = d * (18000 - d):
 *
 *   sin(d) = 4h / (405000000 - h)
 */
int32_t
t150_dir_sin(uint32_t direction)
{
	int64_t h, n;
	uint32_t d = direction % T150_DI_DIR_MAX;
	int negate = 0;

	if (d >= 18000) {
		d -= 18000;
		negate = 1;
	}

	h = (int64_t)d * (18000 - d);
	n = (int64_t)32767 * 4 * h / (405000000 - h);

	return negate ? (int32_t)-n : (int32_t)n;
}

static size_t
enc_settings(uint8_t *buf, size_t buflen, uint8_t op, uint16_t arg)
{
	if (buflen < SETTINGS_LEN)
		return 0;

	buf[0] = T150_OP_SETTINGS;
	buf[1] = op;
	put_le16(buf + 2, arg);

	return SETTINGS_LEN;
}

size_t
t150_enc_autocenter_force(uint8_t *buf, size_t buflen, uint32_t force)
{
	uint16_t arg = (uint16_t)scale_unsigned(force, T150_DI_MAX,
	    T150_AUTOCENTER_MAX);

	return enc_settings(buf, buflen, T150_OP_AUTOCENTER_FORCE, arg);
}

/*
 * The wheel's input open and close, two bytes each. The driver sends the
 * close as three packets, "what" twice then close, and does so after
 * hid_hw_close(); only the last of them is the close itself.
 */
static size_t
enc_input(uint8_t *buf, size_t buflen, uint8_t sub)
{
	if (buflen < INPUT_LEN)
		return 0;

	buf[0] = T150_OP_INPUT;
	buf[1] = sub;

	return INPUT_LEN;
}

size_t
t150_enc_input_open(uint8_t *buf, size_t buflen)
{
	return enc_input(buf, buflen, T150_INPUT_OPEN);
}

size_t
t150_enc_input_close(uint8_t *buf, size_t buflen)
{
	return enc_input(buf, buflen, T150_INPUT_CLOSE);
}

size_t
t150_enc_autocenter_enable(uint8_t *buf, size_t buflen, int enable)
{
	return enc_settings(buf, buflen, T150_OP_AUTOCENTER_ENABLE,
	    enable ? 1 : 0);
}

size_t
t150_enc_range(uint8_t *buf, size_t buflen, unsigned int degrees)
{
	return enc_settings(buf, buflen, T150_OP_RANGE,
	    t150_range_arg(degrees));
}

size_t
t150_enc_gain(uint8_t *buf, size_t buflen, uint32_t gain)
{
	if (buflen < GAIN_LEN)
		return 0;

	buf[0] = T150_OP_GAIN;
	buf[1] = (uint8_t)scale_unsigned(gain, T150_DI_MAX, T150_GAIN_MAX);

	return GAIN_LEN;
}

size_t
t150_enc_ff_first(uint8_t *buf, size_t buflen, const struct t150_effect *ef)
{
	uint32_t attack_level = 0, fade_level = 0;
	uint16_t attack_ms = 0, fade_ms = 0;
	size_t len = T150_FF_FIRST_LEN;
	uint8_t cls, f2 = 0, f3 = 0;

	if (ef->slot >= T150_SLOT_MAX)
		return 0;
	if (first_class(ef->kind, &cls) != 0)
		return 0;

	/* Only a condition carries the trailer, and only it needs 11 bytes. */
	if (cls == T150_FF_FIRST_CONDITION) {
		len = T150_FF_FIRST_LEN_CONDITION;
		if (ef->kind == T150_EFFECT_DAMPER) {
			f2 = T150_FF_FIRST_F2_DAMPER;
			f3 = T150_FF_FIRST_F3_DAMPER;
		} else {
			f2 = T150_FF_FIRST_F2_SPRING;
			f3 = T150_FF_FIRST_F3_SPRING;
		}
	}
	if (buflen < len)
		return 0;

	/*
	 * Conditions carry no envelope, and this enforces it rather than
	 * trusting the caller: DirectInput lets a game attach one to a spring,
	 * and the wheel is only ever observed receiving zeros there. The
	 * driver this is transcribed from arrives at the same wire bytes by
	 * accident, clearing its envelope pointer for conditions and then
	 * leaving the fields uninitialised.
	 *
	 * A game need not supply an envelope for anything else either. The
	 * same driver fills the fade length from the attack length, which is a
	 * plain bug and is not reproduced here.
	 */
	if (ef->envelope.present && cls != T150_FF_FIRST_CONDITION) {
		attack_ms = us_to_ms(ef->envelope.attack_time);
		fade_ms = us_to_ms(ef->envelope.fade_time);
		if (ef->envelope.attack_level > 0)
			attack_level = scale_unsigned(
			    (uint32_t)ef->envelope.attack_level, T150_DI_MAX,
			    T150_FF_ENVELOPE_MAX);
		if (ef->envelope.fade_level > 0)
			fade_level = scale_unsigned(
			    (uint32_t)ef->envelope.fade_level, T150_DI_MAX,
			    T150_FF_ENVELOPE_MAX);
	}

	buf[0] = cls;
	buf[1] = t150_ff_pk_id0(ef->slot);
	buf[2] = 0;
	put_le16(buf + 3, attack_ms);
	buf[5] = (uint8_t)attack_level;
	put_le16(buf + 6, fade_ms);
	buf[8] = (uint8_t)fade_level;
	if (len == T150_FF_FIRST_LEN_CONDITION) {
		buf[9] = f2;
		buf[10] = f3;
	}

	return len;
}

size_t
t150_enc_ff_update(uint8_t *buf, size_t buflen, const struct t150_effect *ef)
{
	size_t len;

	if (ef->slot >= T150_SLOT_MAX)
		return 0;

	switch (ef->kind) {
	case T150_EFFECT_CONSTANT:
		len = T150_FF_UPDATE_LEN_CONSTANT;
		break;
	case T150_EFFECT_SINE:
	case T150_EFFECT_SAWTOOTH_UP:
	case T150_EFFECT_SAWTOOTH_DOWN:
		len = T150_FF_UPDATE_LEN_PERIODIC;
		break;
	case T150_EFFECT_SPRING:
	case T150_EFFECT_DAMPER:
		len = T150_FF_UPDATE_LEN_CONDITION;
		break;
	default:
		return 0;
	}

	if (buflen < len)
		return 0;

	buf[1] = t150_ff_pk_id1(ef->slot);
	buf[2] = 0;

	switch (ef->kind) {
	case T150_EFFECT_CONSTANT: {
		/*
		 * Only the X component reaches a one-axis wheel, so the
		 * magnitude is projected onto it first. See t150_dir_sin()
		 * for what the DLL has to guarantee about the direction.
		 */
		int32_t level = (int32_t)(((int64_t)ef->u.constant.magnitude *
		    t150_dir_sin(ef->direction)) / 32768);

		buf[0] = T150_FF_UPDATE_CONSTANT;
		buf[3] = (uint8_t)(int8_t)scale_signed(level, T150_DI_MAX,
		    T150_FF_LEVEL_MAX);
		break;
	}
	case T150_EFFECT_SINE:
	case T150_EFFECT_SAWTOOTH_UP:
	case T150_EFFECT_SAWTOOTH_DOWN:
		buf[0] = T150_FF_UPDATE_PERIODIC;
		buf[3] = (uint8_t)(int8_t)scale_signed(ef->u.periodic.magnitude,
		    T150_DI_MAX, T150_FF_PERIODIC_MAX);
		buf[4] = (uint8_t)(int8_t)scale_signed(ef->u.periodic.offset,
		    T150_DI_MAX, T150_FF_PERIODIC_MAX);
		buf[5] = (uint8_t)scale_unsigned(
		    ef->u.periodic.phase % T150_DI_DIR_MAX, T150_DI_DIR_MAX,
		    T150_FF_PHASE_MAX);
		put_le16(buf + 6, us_to_ms(ef->u.periodic.period));
		break;
	case T150_EFFECT_SPRING:
	case T150_EFFECT_DAMPER: {
		uint32_t sat_max = ef->kind == T150_EFFECT_SPRING ?
		    T150_FF_SAT_SPRING_MAX : T150_FF_SAT_DAMPER_MAX;
		uint32_t pos_sat, neg_sat;

		pos_sat = scale_unsigned(
		    ef->u.condition.pos_saturation < 0 ? 0 :
		    (uint32_t)ef->u.condition.pos_saturation, T150_DI_MAX,
		    sat_max);
		neg_sat = scale_unsigned(
		    ef->u.condition.neg_saturation < 0 ? 0 :
		    (uint32_t)ef->u.condition.neg_saturation, T150_DI_MAX,
		    sat_max);

		buf[0] = T150_FF_UPDATE_CONDITION;
		buf[3] = (uint8_t)(int8_t)scale_signed(
		    ef->u.condition.pos_coeff, T150_DI_MAX, T150_FF_COEFF_MAX);
		buf[4] = (uint8_t)(int8_t)scale_signed(
		    ef->u.condition.neg_coeff, T150_DI_MAX, T150_FF_COEFF_MAX);
		put_le16(buf + 5, (uint16_t)(int16_t)scale_signed(
		    ef->u.condition.center, T150_DI_MAX, T150_FF_CENTER_MAX));
		put_le16(buf + 7, (uint16_t)scale_unsigned(
		    ef->u.condition.deadband < 0 ? 0 :
		    (uint32_t)ef->u.condition.deadband, T150_DI_MAX,
		    T150_FF_DEADBAND_MAX));
		buf[9] = (uint8_t)pos_sat;
		buf[10] = (uint8_t)neg_sat;
		break;
	}
	default:
		return 0;
	}

	return len;
}

size_t
t150_enc_ff_commit(uint8_t *buf, size_t buflen, const struct t150_effect *ef)
{
	uint16_t type, length;

	if (buflen < T150_FF_COMMIT_LEN || ef->slot >= T150_SLOT_MAX)
		return 0;
	if (commit_type(ef->kind, &type) != 0)
		return 0;

	/*
	 * 0xFFFF is the wheel's endless marker, so a finite duration must
	 * never reach it. us_to_ms saturates there, which turned any effect
	 * of 65.535 seconds or more into one that never stops: the game would
	 * expect it to end on its own and the wheel would keep pushing. Cap a
	 * finite length one millisecond short instead.
	 */
	if (ef->duration == T150_DURATION_INFINITE) {
		length = T150_FF_LENGTH_INFINITE;
	} else {
		length = us_to_ms(ef->duration);
		if (length == T150_FF_LENGTH_INFINITE)
			length = T150_FF_LENGTH_INFINITE - 1;
	}

	memset(buf, 0, T150_FF_COMMIT_LEN);
	buf[0] = T150_FF_COMMIT_F0;
	buf[1] = ef->slot;
	put_le16(buf + 2, type);
	put_le16(buf + 4, length);
	buf[9] = t150_ff_pk_id1(ef->slot);
	buf[11] = t150_ff_pk_id0(ef->slot);
	/* The driver sends the high byte of the delay, so the unit is 256ms. */
	buf[13] = (uint8_t)(us_to_ms(ef->start_delay) >> 8);

	return T150_FF_COMMIT_LEN;
}

size_t
t150_enc_control(uint8_t *buf, size_t buflen, uint8_t slot, int play,
    uint8_t iterations)
{
	if (buflen < T150_FF_CONTROL_LEN || slot >= T150_SLOT_MAX)
		return 0;

	buf[0] = T150_FF_OP_CONTROL;
	buf[1] = slot;
	buf[2] = play ? T150_FF_CTRL_PLAY : T150_FF_CTRL_STOP;
	buf[3] = play && iterations > 0 ? iterations : 1;

	return T150_FF_CONTROL_LEN;
}
