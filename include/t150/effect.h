/*
 * effect.h - the normalized effect model shared by the proxy DLL and the
 * daemon.
 *
 * The proxy DLL sits above Wine's DirectInput and intercepts DIEFFECT
 * structures, so this model deliberately keeps DirectInput's units and
 * conventions rather than the USB PID ones:
 *
 *   magnitudes and levels   -10000 .. 10000
 *   saturations             0 .. 10000
 *   coefficients            -10000 .. 10000
 *   times and periods       microseconds, T150_DURATION_INFINITE for endless
 *   directions              hundredths of a degree, 0 .. 35999, 0 = from north
 *
 * All conversion to wheel bytes happens in the daemon's encoder, so the DLL
 * carries no wheel knowledge and the encoder stays unit-testable on Linux.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef T150_EFFECT_H
#define T150_EFFECT_H

#include <stdint.h>

#define T150_DURATION_INFINITE	UINT32_MAX

/*
 * Effect slots. The wheel addresses effects by slot; the DLL allocates them
 * and is responsible for keeping the mapping stable for the life of an
 * IDirectInputEffect.
 */
#define T150_SLOT_MAX		16u

enum t150_effect_kind {
	T150_EFFECT_NONE = 0,
	T150_EFFECT_CONSTANT,
	T150_EFFECT_RAMP,
	T150_EFFECT_SQUARE,
	T150_EFFECT_SINE,
	T150_EFFECT_TRIANGLE,
	T150_EFFECT_SAWTOOTH_UP,
	T150_EFFECT_SAWTOOTH_DOWN,
	T150_EFFECT_SPRING,
	T150_EFFECT_DAMPER,
	T150_EFFECT_FRICTION,
	T150_EFFECT_INERTIA
};

/*
 * Which of the above the T150 renders in hardware.
 *
 * Constant, the five periodics, spring and damper are native. Ramp,
 * friction and inertia are not in the wire protocol at all and have to be
 * either synthesized by the daemon or refused. Refusing is wrong: a game
 * that gets DIERR_UNSUPPORTED from CreateEffect may disable force feedback
 * outright, so the daemon downgrades instead (inertia to damper, ramp to a
 * time-sliced constant) and says so in its log.
 */
#define T150_SUPPORTS_NATIVE(k)						\
	((k) == T150_EFFECT_CONSTANT || (k) == T150_EFFECT_SQUARE ||	\
	 (k) == T150_EFFECT_SINE || (k) == T150_EFFECT_TRIANGLE ||	\
	 (k) == T150_EFFECT_SAWTOOTH_UP ||				\
	 (k) == T150_EFFECT_SAWTOOTH_DOWN ||				\
	 (k) == T150_EFFECT_SPRING || (k) == T150_EFFECT_DAMPER)

struct t150_envelope {
	uint32_t	attack_time;	/* microseconds */
	int32_t		attack_level;
	uint32_t	fade_time;	/* microseconds */
	int32_t		fade_level;
	uint8_t		present;	/* 0 when the game supplied no envelope */
};

struct t150_constant {
	int32_t		magnitude;
};

struct t150_ramp {
	int32_t		start;
	int32_t		end;
};

struct t150_periodic {
	int32_t		magnitude;
	int32_t		offset;
	uint32_t	phase;		/* hundredths of a degree, 0 .. 35999 */
	uint32_t	period;		/* microseconds */
};

struct t150_condition {
	int32_t		center;
	int32_t		pos_coeff;
	int32_t		neg_coeff;
	int32_t		pos_saturation;
	int32_t		neg_saturation;
	int32_t		deadband;
};

struct t150_effect {
	uint8_t			kind;		/* enum t150_effect_kind */
	uint8_t			slot;		/* 0 .. T150_SLOT_MAX - 1 */
	uint32_t		duration;	/* microseconds */
	uint32_t		start_delay;	/* microseconds */
	uint32_t		gain;		/* 0 .. 10000, per effect */
	uint32_t		direction;	/* hundredths of a degree */
	struct t150_envelope	envelope;
	union {
		struct t150_constant	constant;
		struct t150_ramp	ramp;
		struct t150_periodic	periodic;
		struct t150_condition	condition;
	} u;
};

#endif /* T150_EFFECT_H */
