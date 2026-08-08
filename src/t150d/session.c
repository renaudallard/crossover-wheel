/*
 * session.c - what the daemon does with a frame.
 *
 * No sockets and no clock of its own: every entry point is handed the
 * current time. That keeps the watchdog and the emitter testable in
 * simulated time rather than by waiting.
 *
 * A frame says what the game wants; t150_session_tick decides when the wheel
 * hears about it. Effect parameters are state, so they are coalesced: a slot
 * remembers the bytes it last put on the wire and a pass sends nothing when
 * they have not moved. Everything else is an event, and events are never
 * deferred and never merged, because the wheel starting, stopping or going
 * safe is a thing that happens rather than a value that holds.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>

#include "t150/encode.h"
#include "t150/t150.h"
#include "t150d.h"

#define PKT_MAX	T150_PKT_MAX

static void
reply_ok(struct t150_reply *rep)
{
	rep->op = T150_OP_OK;
	rep->len = 0;
}

static void
reply_err(struct t150_reply *rep, enum t150_proto_err err)
{
	rep->op = T150_OP_ERROR;
	rep->payload[0] = (uint8_t)(err & 0xff);
	rep->payload[1] = (uint8_t)(err >> 8);
	rep->len = 2;
}

/* Returns 0 if the whole packet reached the backend. */
static int
emit(struct t150_session *s, const uint8_t *buf, size_t len)
{
	if (len == 0)
		return -1;

	s->armed = 1;

	return s->be->write(s->be->priv, buf, len);
}

/*
 * A per-effect gain is DirectInput's, not the wheel's: the wheel has one
 * device gain and no notion of a gain per slot. Fold it into the magnitudes
 * before encoding, which is what the game asked for.
 */
static int32_t
apply_gain(int32_t v, uint32_t gain)
{
	if (gain >= (uint32_t)T150_DI_MAX)
		return v;

	return (int32_t)(((int64_t)v * (int32_t)gain) / T150_DI_MAX);
}

static void
scale_effect(struct t150_effect *ef)
{
	uint32_t g = ef->gain;

	if (g >= (uint32_t)T150_DI_MAX)
		return;

	/*
	 * The envelope rides on the same force, so it scales with it. Left
	 * alone, a halved effect kept a full strength attack and fade and
	 * pushed harder at the ends than in the middle.
	 */
	if (ef->envelope.present) {
		ef->envelope.attack_level =
		    apply_gain(ef->envelope.attack_level, g);
		ef->envelope.fade_level =
		    apply_gain(ef->envelope.fade_level, g);
	}

	switch (ef->kind) {
	case T150_EFFECT_CONSTANT:
		ef->u.constant.magnitude = apply_gain(ef->u.constant.magnitude, g);
		break;
	case T150_EFFECT_SQUARE:
	case T150_EFFECT_TRIANGLE:
	case T150_EFFECT_SINE:
	case T150_EFFECT_SAWTOOTH_UP:
	case T150_EFFECT_SAWTOOTH_DOWN:
		ef->u.periodic.magnitude = apply_gain(ef->u.periodic.magnitude, g);
		ef->u.periodic.offset = apply_gain(ef->u.periodic.offset, g);
		break;
	case T150_EFFECT_SPRING:
	case T150_EFFECT_DAMPER:
		/*
		 * The coefficients are the slope and the saturations only cap
		 * it, so scaling the caps alone left the wheel pushing at
		 * full rate everywhere below them. A condition's output is
		 * coefficient times displacement, clamped, and the gain
		 * belongs to the output.
		 */
		ef->u.condition.pos_coeff =
		    apply_gain(ef->u.condition.pos_coeff, g);
		ef->u.condition.neg_coeff =
		    apply_gain(ef->u.condition.neg_coeff, g);
		ef->u.condition.pos_saturation =
		    apply_gain(ef->u.condition.pos_saturation, g);
		ef->u.condition.neg_saturation =
		    apply_gain(ef->u.condition.neg_saturation, g);
		break;
	default:
		break;
	}
}

/*
 * Put a slot's desired effect on the wheel, and only if it is not there
 * already.
 *
 * The three packets correlate through slot keys, and the only sequence any
 * wheel has been measured accepting is all three together (RESEARCH.md A43),
 * so any difference at all sends the set. Sending the one packet that moved
 * would be cheaper and is probably right, since PROTOCOL.md says the second
 * block's key depends only on the slot, but probably is not a thing to put
 * between a game and a wheel that pulls on someone's hands.
 *
 * What this saves is the common case, which is a game re-uploading an effect
 * it has not changed: that now costs a comparison instead of three writes.
 *
 * The comparison is of the encoded bytes rather than of the effect struct,
 * because the two are not the same question. A constant's level is one signed
 * byte, so sixty four DirectInput magnitudes share it, and direction is not
 * a field of the wire form at all for a periodic or a condition. Comparing
 * structs would send packets the wheel cannot tell apart, and would read
 * padding and the inactive tail of a union while doing it.
 *
 * Returns 0 when the wheel holds the effect, -1 when a write failed, and -2
 * when the effect cannot be encoded at all. The two failures are different:
 * a write may succeed next time and an encoding never will.
 */
static int
flush_slot(struct t150_session *s, struct t150_slot *sl)
{
	struct t150_wire pkt[3];
	size_t i;
	int changed = 0;

	memset(pkt, 0, sizeof(pkt));
	pkt[0].len = (uint8_t)t150_enc_ff_first(pkt[0].buf, sizeof(pkt[0].buf),
	    &sl->ef);
	pkt[1].len = (uint8_t)t150_enc_ff_update(pkt[1].buf, sizeof(pkt[1].buf),
	    &sl->ef);
	pkt[2].len = (uint8_t)t150_enc_ff_commit(pkt[2].buf, sizeof(pkt[2].buf),
	    &sl->ef);

	for (i = 0; i < 3; i++) {
		if (pkt[i].len == 0)
			return -2;
		if (pkt[i].len != sl->sent[i].len ||
		    memcmp(pkt[i].buf, sl->sent[i].buf, pkt[i].len) != 0)
			changed = 1;
	}
	if (!changed)
		return 0;

	/*
	 * Recorded per packet and only once its write has succeeded, so a
	 * failure part way through leaves the slot believing exactly what
	 * reached the wheel and the next pass sends the rest.
	 */
	for (i = 0; i < 3; i++) {
		if (emit(s, pkt[i].buf, pkt[i].len) != 0)
			return -1;
		sl->sent[i] = pkt[i];
	}

	return 0;
}

static int
control(struct t150_session *s, uint8_t slot, int play, uint8_t iterations)
{
	uint8_t pkt[PKT_MAX];
	size_t n;

	n = t150_enc_control(pkt, sizeof(pkt), slot, play, iterations);

	return emit(s, pkt, n);
}

void
t150_session_init(struct t150_session *s, struct t150_backend *be,
    const char *token)
{
	memset(s, 0, sizeof(*s));
	s->be = be;
	if (token != NULL) {
		strncpy(s->token, token, sizeof(s->token) - 1);
		s->token[sizeof(s->token) - 1] = '\0';
	}
}

/*
 * Whether this session ever reached the wheel. One that had the wheel's
 * input opened for it owes a close; one that emitted anything may have left
 * a force or an autocenter behind. A connection that did neither, which is
 * any process that opens the port and goes away again, touched nothing, and
 * undoing nothing is three HID writes the wheel should not receive on its
 * account.
 *
 * The open is tracked rather than inferred from hello, because a client that
 * says goodbye clears hello itself and the close is still owed at that
 * point. Inferring it left the wheel's input open after every graceful
 * disconnect, which is the failure the abrupt path was written to avoid.
 */
static int
session_touched_wheel(const struct t150_session *s)
{
	return s->input_open || s->armed;
}

static void
session_safe_state(struct t150_session *s, const char *why)
{
	uint8_t pkt[PKT_MAX];
	size_t i, n;

	if (s->verbose && why != NULL)
		fprintf(stderr, "t150d: safe state: %s\n", why);

	/*
	 * The memset is load-bearing beyond forgetting the effect: it clears
	 * the slot's dirty flag and its record of what the wheel holds, so a
	 * safe state also cancels every pending emission. Moving either of
	 * those out of the slot would quietly leave a pass ready to write a
	 * force to a wheel that has just been made safe.
	 */
	for (i = 0; i < T150_SLOT_MAX; i++) {
		if (s->slots[i].used && s->slots[i].playing)
			(void)control(s, (uint8_t)i, 0, 0);
		memset(&s->slots[i], 0, sizeof(s->slots[i]));
	}
	s->io_err = 0;
	s->emit_failed = 0;

	/*
	 * Release the autocenter last. A wheel that has been left holding a
	 * force should end up limp, not fighting whoever grabs it next.
	 *
	 * It takes the force, not the enable flag. 0x04 only says whether the
	 * autocenter survives an application opening the wheel's input, and
	 * the effect is active whenever none has, so clearing it releases
	 * nothing. This code sent only 0x04 and therefore never made the
	 * wheel safe. See PROTOCOL.md and RESEARCH.md A15, which cost six
	 * hardware sessions to learn.
	 */
	n = t150_enc_autocenter_force(pkt, sizeof(pkt), 0);
	(void)s->be->write(s->be->priv, pkt, n);
	n = t150_enc_autocenter_enable(pkt, sizeof(pkt), 0);
	(void)s->be->write(s->be->priv, pkt, n);

	s->armed = 0;
}

/*
 * Everything the watchdog does, and then close the wheel's input so it goes
 * back to holding its own autocenter rather than waiting for effects nobody
 * will send.
 */
static void
session_release(struct t150_session *s, const char *why, int force)
{
	uint8_t pkt[PKT_MAX];
	size_t n;

	session_safe_state(s, why);

	if (force || s->input_open) {
		n = t150_enc_input_close(pkt, sizeof(pkt));
		(void)s->be->write(s->be->priv, pkt, n);
	}
	s->input_open = 0;
	s->hello = 0;
}

/* The client is gone for good, as opposed to merely quiet. */
void
t150_session_end(struct t150_session *s, const char *why)
{
	if (!session_touched_wheel(s))
		return;

	session_release(s, why, 0);
}

/*
 * We are the ones going away. This one is unconditional: the backend opens
 * and closes the wheel's input on its own account as well, so the last thing
 * the daemon does is state the safe state outright rather than reason about
 * who owed it.
 */
void
t150_session_shutdown(struct t150_session *s, const char *why)
{
	session_release(s, why, 1);
}

void
t150_session_panic(struct t150_session *s, const char *why)
{
	if (!session_touched_wheel(s))
		return;

	session_safe_state(s, why);
}

/* Constant-time enough for a token that is not a security boundary anyway. */
static int
token_ok(const struct t150_session *s, const uint8_t *payload, size_t len)
{
	unsigned char diff = 0;
	size_t i;

	if (len != T150_TOKEN_LEN)
		return 0;
	for (i = 0; i < T150_TOKEN_LEN; i++)
		diff |= (unsigned char)(payload[i] ^ (unsigned char)s->token[i]);

	return diff == 0;
}

static void
do_upload(struct t150_session *s, const uint8_t *payload, size_t len,
    struct t150_reply *rep)
{
	struct t150_effect ef;
	struct t150_slot *sl;
	struct t150_wire sent[3];
	uint64_t started_ms;
	uint8_t want, was_playing, iterations;

	if (len < T150_PROTO_EFFECT_LEN ||
	    t150_proto_unpack_effect(payload, len, &ef) != 0) {
		reply_err(rep, T150_ERR_BAD_FRAME);
		return;
	}
	if (ef.slot >= T150_SLOT_MAX) {
		reply_err(rep, T150_ERR_BAD_SLOT);
		return;
	}

	sl = &s->slots[ef.slot];
	want = ef.kind;

	/*
	 * Downgrade rather than refuse. A game that gets DIERR_UNSUPPORTED
	 * back from CreateEffect may turn force feedback off altogether, so
	 * an approximation beats an honest no.
	 */
	ef.kind = t150_effect_downgrade(want);
	if (s->verbose && ef.kind != want)
		fprintf(stderr, "t150d: slot %u: effect %u sent as %u\n",
		    ef.slot, want, ef.kind);

	scale_effect(&ef);

	/*
	 * Re-uploading a slot that is already playing must not forget that it
	 * is. A game updating a running force is the commonest thing there
	 * is, and nothing here stops the effect on the wheel, so clearing the
	 * flag would hide the slot from both the ramp slicer and the
	 * watchdog's stop loop. The wheel would then keep pushing after the
	 * game died, which is the one outcome the watchdog exists to prevent.
	 */
	was_playing = sl->used ? sl->playing : 0;
	started_ms = sl->started_ms;
	iterations = sl->iterations;
	/*
	 * What the wheel already holds has to survive this, or every upload
	 * would look like the first one and the comparison in flush_slot
	 * would never match. It is the one field here that describes the
	 * wheel rather than the game's wishes.
	 */
	memcpy(sent, sl->sent, sizeof(sent));

	memset(sl, 0, sizeof(*sl));
	sl->playing = was_playing;
	sl->started_ms = started_ms;
	sl->iterations = iterations;
	memcpy(sl->sent, sent, sizeof(sl->sent));
	sl->source_kind = want;
	if (want == T150_EFFECT_RAMP) {
		struct t150_effect raw;

		if (t150_proto_unpack_effect(payload, len, &raw) == 0)
			sl->ramp = raw.u.ramp;
		ef.u.constant.magnitude = apply_gain(sl->ramp.start, ef.gain);
	}
	sl->ef = ef;
	sl->used = 1;
	sl->dirty = 1;
	/*
	 * A pass that failed stops shortening the poll timeout so it cannot
	 * spin against an absent wheel; a fresh upload is reason enough to
	 * try again promptly.
	 */
	s->emit_failed = 0;

	/*
	 * The effect is accepted, and the tick will have it on the wheel
	 * within one emit period. That leaves nowhere for a write error to
	 * be reported except the next upload, which is where it goes: the
	 * state above is stored first, so the frame that carries the bad
	 * news is not also the frame that gets thrown away.
	 *
	 * It is reported here and on nothing else on purpose. The proxy
	 * drops its socket on any error reply to a keepalive, and nothing
	 * reconnects, so one unplugged wheel would cost the game its force
	 * feedback for the life of the process.
	 */
	if (s->io_err) {
		s->io_err = 0;
		reply_err(rep, T150_ERR_DEVICE_IO);
		return;
	}

	reply_ok(rep);
}

static void
do_start(struct t150_session *s, const uint8_t *payload, size_t len,
    uint64_t now_ms, struct t150_reply *rep)
{
	struct t150_slot *sl;

	if (len < 2) {
		reply_err(rep, T150_ERR_BAD_FRAME);
		return;
	}
	if (payload[0] >= T150_SLOT_MAX || !s->slots[payload[0]].used) {
		reply_err(rep, T150_ERR_BAD_SLOT);
		return;
	}

	sl = &s->slots[payload[0]];

	/*
	 * Start is an event and goes out now, but it must not overtake the
	 * parameters it starts, so the slot is flushed first. This is also
	 * what keeps the game's own ordering: a game that uploads and starts
	 * in one burst gets both on the wheel before it hears about either.
	 */
	if (sl->dirty) {
		if (flush_slot(s, sl) != 0) {
			/* Reported here, so the next upload does not repeat it. */
			s->io_err = 0;
			reply_err(rep, T150_ERR_DEVICE_IO);
			return;
		}
		sl->dirty = 0;
		s->next_emit_ms = now_ms + T150_EMIT_MS;
	}

	if (control(s, payload[0], 1, payload[1]) != 0) {
		reply_err(rep, T150_ERR_DEVICE_IO);
		return;
	}

	sl->playing = 1;
	sl->iterations = payload[1];
	sl->started_ms = now_ms;
	reply_ok(rep);
}

static void
do_stop(struct t150_session *s, const uint8_t *payload, size_t len, int destroy,
    struct t150_reply *rep)
{
	struct t150_slot *sl;

	if (len < 1) {
		reply_err(rep, T150_ERR_BAD_FRAME);
		return;
	}
	if (payload[0] >= T150_SLOT_MAX || !s->slots[payload[0]].used) {
		reply_err(rep, T150_ERR_BAD_SLOT);
		return;
	}

	sl = &s->slots[payload[0]];
	if (sl->playing && control(s, payload[0], 0, 0) != 0) {
		reply_err(rep, T150_ERR_DEVICE_IO);
		return;
	}

	sl->playing = 0;
	/*
	 * Drop whatever was waiting to be written to this slot. Nothing says
	 * a parameter packet is inert on a stopped slot, no wheel has ever
	 * been given one, and a pass that fired just after a stop would be
	 * asking that question of a wheel someone is holding.
	 */
	sl->dirty = 0;
	if (destroy)
		memset(sl, 0, sizeof(*sl));

	reply_ok(rep);
}

static void
do_setting(struct t150_session *s, uint8_t op, const uint8_t *payload,
    size_t len, struct t150_reply *rep)
{
	uint8_t pkt[PKT_MAX];
	uint32_t v;
	size_t n;

	if (len < 4) {
		reply_err(rep, T150_ERR_BAD_FRAME);
		return;
	}

	v = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
	    ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);

	switch (op) {
	case T150_OP_SET_GAIN:
		n = t150_enc_gain(pkt, sizeof(pkt), v);
		if (emit(s, pkt, n) != 0) {
			reply_err(rep, T150_ERR_DEVICE_IO);
			return;
		}
		break;
	case T150_OP_SET_RANGE:
		n = t150_enc_range(pkt, sizeof(pkt), (unsigned int)v);
		if (emit(s, pkt, n) != 0) {
			reply_err(rep, T150_ERR_DEVICE_IO);
			return;
		}
		break;
	case T150_OP_SET_AUTOCENTER:
		/*
		 * Zero means off, anything else is a strength and an enable.
		 * Off is the force, not the enable flag: clearing 0x04 alone
		 * leaves the wheel gripped, which is PROTOCOL.md's warning
		 * and was measured on hardware.
		 */
		if (v == 0) {
			n = t150_enc_autocenter_force(pkt, sizeof(pkt), 0);
			if (emit(s, pkt, n) != 0) {
				reply_err(rep, T150_ERR_DEVICE_IO);
				return;
			}
			n = t150_enc_autocenter_enable(pkt, sizeof(pkt), 0);
			if (emit(s, pkt, n) != 0) {
				reply_err(rep, T150_ERR_DEVICE_IO);
				return;
			}
			break;
		}
		n = t150_enc_autocenter_force(pkt, sizeof(pkt), v);
		if (emit(s, pkt, n) != 0) {
			reply_err(rep, T150_ERR_DEVICE_IO);
			return;
		}
		n = t150_enc_autocenter_enable(pkt, sizeof(pkt), 1);
		if (emit(s, pkt, n) != 0) {
			reply_err(rep, T150_ERR_DEVICE_IO);
			return;
		}
		break;
	default:
		reply_err(rep, T150_ERR_BAD_FRAME);
		return;
	}

	reply_ok(rep);
}

static void
do_reset(struct t150_session *s, struct t150_reply *rep)
{
	size_t i;

	/*
	 * DirectInput's reset stops and releases every effect. It says
	 * nothing about the autocenter, which the game sets separately, so
	 * unlike the watchdog this leaves it alone.
	 */
	for (i = 0; i < T150_SLOT_MAX; i++) {
		if (s->slots[i].used && s->slots[i].playing)
			(void)control(s, (uint8_t)i, 0, 0);
		memset(&s->slots[i], 0, sizeof(s->slots[i]));
	}

	reply_ok(rep);
}

/*
 * DirectInput draws a line between its two commands and so must we.
 * DISFFC_RESET stops and releases; DISFFC_STOPALL stops and leaves every
 * effect downloaded, so a game that pauses with STOPALL can start the same
 * effects again afterwards. Sending both as a reset destroyed the slots and
 * the game's next Start referred to something that no longer existed.
 */
static void
do_stop_all(struct t150_session *s, struct t150_reply *rep)
{
	size_t i;

	for (i = 0; i < T150_SLOT_MAX; i++) {
		if (!s->slots[i].used || !s->slots[i].playing)
			continue;
		if (control(s, (uint8_t)i, 0, 0) != 0) {
			reply_err(rep, T150_ERR_DEVICE_IO);
			return;
		}
		s->slots[i].playing = 0;
		/* As in do_stop: nothing goes to a slot that just stopped. */
		s->slots[i].dirty = 0;
	}

	reply_ok(rep);
}

int
t150_session_frame(struct t150_session *s, uint8_t op, const uint8_t *payload,
    size_t len, uint64_t now_ms, struct t150_reply *rep)
{
	uint8_t pkt[PKT_MAX];
	size_t n;

	s->last_frame_ms = now_ms;

	if (op == T150_OP_HELLO) {
		if (!token_ok(s, payload, len)) {
			if (s->verbose)
				fprintf(stderr, "t150d: port %u offered the "
				    "wrong token\n", s->peer_port);
			reply_err(rep, T150_ERR_BAD_TOKEN);
			return 0;
		}
		s->hello = 1;
		if (s->verbose)
			fprintf(stderr, "t150d: port %u said hello, the wheel "
			    "is its own now\n", s->peer_port);
		/*
		 * Open the wheel's input. The firmware renders no effect at
		 * all until something does, which is what cost this project
		 * eleven sessions, and nothing on macOS opens it on our
		 * behalf. RESEARCH.md A28.
		 */
		n = t150_enc_input_open(pkt, sizeof(pkt));
		(void)emit(s, pkt, n);
		s->input_open = 1;
		reply_ok(rep);
		return 0;
	}

	if (!s->hello) {
		reply_err(rep, T150_ERR_BAD_TOKEN);
		return 0;
	}

	switch (op) {
	case T150_OP_BYE:
		t150_session_panic(s, "client said goodbye");
		s->hello = 0;
		reply_ok(rep);
		return -1;
	case T150_OP_EFFECT_UPLOAD:
		do_upload(s, payload, len, rep);
		break;
	case T150_OP_EFFECT_START:
		do_start(s, payload, len, now_ms, rep);
		break;
	case T150_OP_EFFECT_STOP:
		do_stop(s, payload, len, 0, rep);
		break;
	case T150_OP_EFFECT_DESTROY:
		do_stop(s, payload, len, 1, rep);
		break;
	case T150_OP_SET_GAIN:
	case T150_OP_SET_AUTOCENTER:
	case T150_OP_SET_RANGE:
		do_setting(s, op, payload, len, rep);
		break;
	case T150_OP_RESET:
		do_reset(s, rep);
		break;
	case T150_OP_STOP_ALL:
		do_stop_all(s, rep);
		break;
	case T150_OP_KEEPALIVE:
		reply_ok(rep);
		break;
	default:
		reply_err(rep, T150_ERR_UNSUPPORTED);
		break;
	}

	return 0;
}

/*
 * Where a ramp is on its slide right now. Held at the start value before it
 * begins and at the end value after, so a ramp that outlives its duration
 * does not wrap round to the beginning.
 */
static int32_t
ramp_level(const struct t150_slot *sl, uint64_t now_ms)
{
	uint64_t elapsed, total;
	int64_t span;

	if (sl->ef.duration == T150_DURATION_INFINITE || sl->ef.duration == 0)
		return sl->ramp.start;

	total = sl->ef.duration / 1000;
	if (total == 0)
		return sl->ramp.end;

	elapsed = now_ms - sl->started_ms;
	if (elapsed >= total)
		return sl->ramp.end;

	span = (int64_t)sl->ramp.end - sl->ramp.start;

	return (int32_t)(sl->ramp.start + (span * (int64_t)elapsed) / (int64_t)total);
}

/* Forget what the wheel was believed to hold, and mean to teach it again. */
static void
session_forget_wheel(struct t150_session *s)
{
	size_t i;

	for (i = 0; i < T150_SLOT_MAX; i++) {
		memset(s->slots[i].sent, 0, sizeof(s->slots[i].sent));
		if (s->slots[i].used)
			s->slots[i].dirty = 1;
	}
}

static int
slots_dirty(const struct t150_session *s)
{
	size_t i;

	for (i = 0; i < T150_SLOT_MAX; i++) {
		if (s->slots[i].used && s->slots[i].dirty)
			return 1;
	}

	return 0;
}

/*
 * Write out what changed, newest slot state only, at most T150_EMIT_SLOTS
 * slots and no sooner than T150_EMIT_MS after the last pass. Resumes where
 * the previous pass stopped so a storm on the low slots cannot starve the
 * high ones.
 */
static void
session_emit(struct t150_session *s, uint64_t now_ms)
{
	size_t i, done = 0;

	s->emit_failed = 0;
	for (i = 0; i < T150_SLOT_MAX && done < T150_EMIT_SLOTS; i++) {
		size_t k = (s->next_slot + i) % T150_SLOT_MAX;
		struct t150_slot *sl = &s->slots[k];
		int r;

		if (!sl->used || !sl->dirty)
			continue;

		done++;
		if ((r = flush_slot(s, sl)) == 0) {
			sl->dirty = 0;
			continue;
		}

		s->io_err = 1;
		if (r == -1) {
			/*
			 * A write that failed may succeed next time, so the
			 * slot stays dirty. What must not happen is a retry
			 * every four milliseconds against a wheel that is not
			 * there, so this stops the deadline shortening the
			 * poll timeout: the retry rides on the next frame or
			 * the next watchdog wake instead, and the client's
			 * keepalive makes that at most 150 ms away.
			 */
			s->emit_failed = 1;
			continue;
		}

		/*
		 * An effect this build cannot encode will not encode next
		 * time either, so the slot goes rather than being retried for
		 * the life of the session. If it was playing then something
		 * on the wheel is still running and only the stop ends it.
		 */
		if (sl->playing)
			(void)control(s, (uint8_t)k, 0, 0);
		memset(sl, 0, sizeof(*sl));
	}

	s->next_slot = (uint8_t)((s->next_slot + i) % T150_SLOT_MAX);
	s->next_emit_ms = now_ms + T150_EMIT_MS;
}

unsigned int
t150_session_tick(struct t150_session *s, uint64_t now_ms)
{
	unsigned int next = T150_WATCHDOG_MS;
	uint64_t quiet;
	size_t i;
	int sliding = 0;

	/*
	 * The backend re-acquires the wheel on its own account, and acquiring
	 * scrubs every slot, so from that moment what this session believes
	 * the wheel holds is a lie. Forget it and let the pass below teach
	 * the wheel again. Nothing replays a start: the game asked for that
	 * before the wheel went away, and a wheel that begins pushing on its
	 * own after a replug is not an improvement.
	 */
	if (s->epoch != s->be->epoch) {
		s->epoch = s->be->epoch;
		session_forget_wheel(s);
	}

	/*
	 * The watchdog is evaluated before any writing this call does, which
	 * is the honest order: the client's silence is measured against the
	 * last frame, not against however long the wheel took afterwards.
	 */
	if (s->hello && s->armed) {
		quiet = now_ms - s->last_frame_ms;
		if (quiet >= T150_WATCHDOG_MS) {
			t150_session_panic(s, "no frame within the watchdog");
			return T150_WATCHDOG_MS;
		}
		if (next > T150_WATCHDOG_MS - (unsigned int)quiet)
			next = T150_WATCHDOG_MS - (unsigned int)quiet;
	}

	/*
	 * A ramp is not a wheel effect, so the daemon walks it: recompute
	 * where it has slid to and leave that in the slot. Whether it is
	 * worth a packet is the emitter's question, and its answer is better
	 * than the one this used to give itself, which compared DirectInput
	 * magnitudes and so re-sent levels that encode to the same byte.
	 */
	for (i = 0; i < T150_SLOT_MAX; i++) {
		struct t150_slot *sl = &s->slots[i];
		int32_t level;

		if (!sl->used || !sl->playing ||
		    sl->source_kind != T150_EFFECT_RAMP)
			continue;

		sliding = 1;
		if (now_ms < s->next_ramp_ms)
			continue;

		level = apply_gain(ramp_level(sl, now_ms), sl->ef.gain);
		if (level != sl->ef.u.constant.magnitude) {
			sl->ef.u.constant.magnitude = level;
			sl->dirty = 1;
		}
	}
	if (sliding && now_ms >= s->next_ramp_ms)
		s->next_ramp_ms = now_ms + T150_RAMP_TICK_MS;

	if (slots_dirty(s) && now_ms >= s->next_emit_ms)
		session_emit(s, now_ms);

	/*
	 * The timeout is the soonest thing that has to happen. Work that is
	 * only pending does not shorten it, which is what keeps an idle
	 * daemon sleeping until the watchdog rather than waking to find
	 * nothing to do.
	 */
	if (sliding && s->next_ramp_ms > now_ms &&
	    next > (unsigned int)(s->next_ramp_ms - now_ms))
		next = (unsigned int)(s->next_ramp_ms - now_ms);
	if (slots_dirty(s) && !s->emit_failed && s->next_emit_ms > now_ms &&
	    next > (unsigned int)(s->next_emit_ms - now_ms))
		next = (unsigned int)(s->next_emit_ms - now_ms);

	return next;
}
