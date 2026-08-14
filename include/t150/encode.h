/*
 * encode.h - turn normalized effects into T150 wire packets.
 *
 * These are the only functions in the project that know both DirectInput
 * units and wheel units. The proxy DLL therefore carries no wheel knowledge
 * and the daemon carries no DirectInput knowledge, which is what keeps this
 * layer testable on Linux with no hardware.
 *
 * Every function writes into a caller-supplied buffer and returns the number
 * of bytes written, or 0 if the buffer is too small or the effect cannot be
 * encoded. Nothing here allocates, blocks or keeps state.
 *
 * The caller is expected to have downgraded the effect first, with
 * t150_effect_downgrade(), because the encoders refuse a kind the wheel has
 * no type code for rather than guessing one.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef T150_ENCODE_H
#define T150_ENCODE_H

#include <stddef.h>
#include <stdint.h>

#include "t150/effect.h"

/*
 * Open and close the wheel's input. The firmware tracks whether an
 * application has one open and renders nothing at all while none has: the
 * autocenter is unconditionally active and no effect plays, which RESEARCH.md
 * A28 measured and which was the missing packet behind every silent effect run
 * this project made. So a daemon opens before it uploads anything. An open
 * outlives the process that sent it (A30), so whatever opens one owes the
 * close.
 */
size_t	t150_enc_input_open(uint8_t *buf, size_t buflen);
size_t	t150_enc_input_close(uint8_t *buf, size_t buflen);

/* Settings. Arguments are in DirectInput units, see effect.h. */
size_t	t150_enc_autocenter_force(uint8_t *buf, size_t buflen, uint32_t force);
size_t	t150_enc_autocenter_enable(uint8_t *buf, size_t buflen, int enable);
size_t	t150_enc_range(uint8_t *buf, size_t buflen, unsigned int degrees);
size_t	t150_enc_gain(uint8_t *buf, size_t buflen, uint32_t gain);

/*
 * The three packets of an effect upload, in the order the wheel expects
 * them. They correlate through the slot keys, so all three must describe the
 * same effect and the same slot.
 */
size_t	t150_enc_ff_first(uint8_t *buf, size_t buflen,
	    const struct t150_effect *ef);
size_t	t150_enc_ff_update(uint8_t *buf, size_t buflen,
	    const struct t150_effect *ef);
size_t	t150_enc_ff_commit(uint8_t *buf, size_t buflen,
	    const struct t150_effect *ef);

/*
 * Start or stop an uploaded effect. iterations is how many times to play it
 * and is ignored when stopping. There is no erase packet: the wheel has no
 * opcode for it, so releasing a slot is a stop plus forgetting about it.
 */
size_t	t150_enc_control(uint8_t *buf, size_t buflen, uint8_t slot, int play,
	    uint8_t iterations);

/*
 * The kind actually sent to the wheel for a given requested kind. Friction and
 * inertia become damper, ramp becomes a constant the caller is responsible for
 * re-sending as it slides, and everything else is passed through. Square and
 * triangle were downgraded to sine until RESEARCH.md A40 took both type codes
 * from the vendor's own table; effect.h and t150.h have said so since. Refusing
 * instead would be worse: a game that gets DIERR_UNSUPPORTED from
 * CreateEffect may switch force feedback off altogether.
 */
uint8_t	t150_effect_downgrade(uint8_t kind);

/*
 * Sine of a direction given in hundredths of a degree, as a signed Q15
 * fraction, so 9000 gives 32767 and 27000 gives -32767. This is the X
 * component of a DirectInput polar direction, which is the only component a
 * one-axis wheel can render.
 *
 * Note what this means for the DLL: a one-axis DirectInput effect carries
 * its side in the sign of the magnitude rather than in the direction, so the
 * DLL must normalize such effects to 9000 before they arrive here. A
 * direction of 0 is north, which projects onto no X at all, and would
 * correctly but uselessly produce no force.
 */
int32_t	t150_dir_sin(uint32_t direction);

#endif /* T150_ENCODE_H */
