/*
 * session.c - what the daemon does with a frame.
 *
 * No sockets and no clock of its own: every entry point is handed the
 * current time. That keeps the watchdog and the ramp slicer testable in
 * simulated time rather than by waiting.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>

#include "t150/encode.h"
#include "t150/t150.h"
#include "t150d.h"

#define PKT_MAX	16

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

	switch (ef->kind) {
	case T150_EFFECT_CONSTANT:
		ef->u.constant.magnitude = apply_gain(ef->u.constant.magnitude, g);
		break;
	case T150_EFFECT_SINE:
	case T150_EFFECT_SAWTOOTH_UP:
	case T150_EFFECT_SAWTOOTH_DOWN:
		ef->u.periodic.magnitude = apply_gain(ef->u.periodic.magnitude, g);
		ef->u.periodic.offset = apply_gain(ef->u.periodic.offset, g);
		break;
	case T150_EFFECT_SPRING:
	case T150_EFFECT_DAMPER:
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
 * Send the three packets that upload one effect. They correlate through slot
 * keys, so a failure part way through leaves the wheel with an incomplete
 * effect: the caller answers DEVICE_IO and the slot stays unusable until the
 * game uploads it again.
 */
static int
upload(struct t150_session *s, const struct t150_effect *ef)
{
	uint8_t pkt[PKT_MAX];
	size_t n;

	if ((n = t150_enc_ff_first(pkt, sizeof(pkt), ef)) == 0)
		return -1;
	if (emit(s, pkt, n) != 0)
		return -1;
	if ((n = t150_enc_ff_update(pkt, sizeof(pkt), ef)) == 0)
		return -1;
	if (emit(s, pkt, n) != 0)
		return -1;
	if ((n = t150_enc_ff_commit(pkt, sizeof(pkt), ef)) == 0)
		return -1;

	return emit(s, pkt, n);
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

void
t150_session_panic(struct t150_session *s, const char *why)
{
	uint8_t pkt[PKT_MAX];
	size_t i, n;

	if (s->verbose && why != NULL)
		fprintf(stderr, "t150d: safe state: %s\n", why);

	for (i = 0; i < T150_SLOT_MAX; i++) {
		if (s->slots[i].used && s->slots[i].playing)
			(void)control(s, (uint8_t)i, 0, 0);
		memset(&s->slots[i], 0, sizeof(s->slots[i]));
	}

	/*
	 * Release the autocenter last. A wheel that has been left holding a
	 * force should end up limp, not fighting whoever grabs it next.
	 */
	n = t150_enc_autocenter_enable(pkt, sizeof(pkt), 0);
	(void)s->be->write(s->be->priv, pkt, n);

	s->armed = 0;
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
	uint8_t want;

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

	memset(sl, 0, sizeof(*sl));
	sl->source_kind = want;
	if (want == T150_EFFECT_RAMP) {
		struct t150_effect raw;

		if (t150_proto_unpack_effect(payload, len, &raw) == 0)
			sl->ramp = raw.u.ramp;
		ef.u.constant.magnitude = apply_gain(sl->ramp.start, ef.gain);
	}
	sl->ef = ef;
	sl->last_level = ef.u.constant.magnitude;

	if (upload(s, &ef) != 0) {
		memset(sl, 0, sizeof(*sl));
		reply_err(rep, T150_ERR_DEVICE_IO);
		return;
	}

	sl->used = 1;
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
		/* Zero means off, anything else is a strength and an enable. */
		if (v == 0) {
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

int
t150_session_frame(struct t150_session *s, uint8_t op, const uint8_t *payload,
    size_t len, uint64_t now_ms, struct t150_reply *rep)
{
	s->last_frame_ms = now_ms;

	if (op == T150_OP_HELLO) {
		if (!token_ok(s, payload, len)) {
			reply_err(rep, T150_ERR_BAD_TOKEN);
			return 0;
		}
		s->hello = 1;
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

unsigned int
t150_session_tick(struct t150_session *s, uint64_t now_ms)
{
	unsigned int next = T150_WATCHDOG_MS;
	uint64_t quiet;
	size_t i;

	for (i = 0; i < T150_SLOT_MAX; i++) {
		struct t150_slot *sl = &s->slots[i];
		uint8_t pkt[PKT_MAX];
		int32_t level;
		size_t n;

		if (!sl->used || !sl->playing ||
		    sl->source_kind != T150_EFFECT_RAMP)
			continue;

		next = T150_RAMP_TICK_MS;

		level = apply_gain(ramp_level(sl, now_ms), sl->ef.gain);
		if (level == sl->last_level)
			continue;

		sl->last_level = level;
		sl->ef.u.constant.magnitude = level;
		n = t150_enc_ff_update(pkt, sizeof(pkt), &sl->ef);
		(void)emit(s, pkt, n);
	}

	if (!s->hello || !s->armed)
		return next;

	quiet = now_ms - s->last_frame_ms;
	if (quiet >= T150_WATCHDOG_MS) {
		t150_session_panic(s, "no frame within the watchdog");
		return T150_WATCHDOG_MS;
	}

	if (next > T150_WATCHDOG_MS - (unsigned int)quiet)
		next = T150_WATCHDOG_MS - (unsigned int)quiet;

	return next;
}
