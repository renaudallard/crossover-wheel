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

/*
 * What to call an effect in the log. One table, because the parameter line
 * and the start and stop lines all want it and two tables would drift apart.
 * The name is the kind the wheel is given, after any downgrade, which is
 * reported on its own line when it happens.
 */
static const char *
kind_name(uint8_t kind)
{
	switch (kind) {
	case T150_EFFECT_CONSTANT:	return "constant";
	case T150_EFFECT_RAMP:		return "ramp";
	case T150_EFFECT_SQUARE:	return "square";
	case T150_EFFECT_SINE:		return "sine";
	case T150_EFFECT_TRIANGLE:	return "triangle";
	case T150_EFFECT_SAWTOOTH_UP:	return "sawtooth up";
	case T150_EFFECT_SAWTOOTH_DOWN:	return "sawtooth down";
	case T150_EFFECT_SPRING:	return "spring";
	case T150_EFFECT_DAMPER:	return "damper";
	case T150_EFFECT_FRICTION:	return "friction";
	case T150_EFFECT_INERTIA:	return "inertia";
	default:			return "nothing";
	}
}

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
 * The three packets correlate through slot keys. This used to send all
 * three on any difference, justified by a claim that the only sequence a
 * wheel had been measured accepting was the set, cited to RESEARCH.md A43.
 * That citation was wrong: A43 is the end to end force delivery test and
 * says nothing about packet sequences. The vendor's own DirectInput capture
 * says the opposite, and is in this tree.
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
static int control(struct t150_session *s, uint8_t slot, int play,
	    uint8_t iterations);

/* Where a ramp has slid to. Defined with the slicer, needed by do_upload. */
static int32_t ramp_level(const struct t150_slot *sl, uint64_t now_ms);

/*
 * Returns 0 when the wheel already holds the effect and nothing was sent,
 * 1 when only the update went, 2 when the whole set went, -1 when a write
 * failed and -2 when the effect cannot be encoded at all. The caller needs
 * to tell 1 from 2: a modulated constant is re-played after a bare update
 * and must not be re-played after a full upload, which carries its own.
 */
static int
flush_slot(struct t150_session *s, struct t150_slot *sl)
{
	struct t150_wire pkt[3];
	size_t i, first, last;
	int moved[3];

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
		moved[i] = pkt[i].len != sl->sent[i].len ||
		    memcmp(pkt[i].buf, sl->sent[i].buf, pkt[i].len) != 0;
	}
	if (!moved[0] && !moved[1] && !moved[2])
		return 0;

	/*
	 * Which of the three actually go.
	 *
	 * A level that has moved and nothing else is one packet, because that
	 * is what the vendor's own driver sends. Its DirectInput capture,
	 * tmp/oldffb/directX_constforce.pcapng, uploads the effect once and
	 * then modulates it with twenty four bare ff_update packets and never
	 * re-states the pair around them. Re-sending all three cost this
	 * daemon two writes in every three, and every write is a synchronous
	 * IOKit call on the one thread the game is waiting on.
	 *
	 * A change to ff_first or ff_commit is a different thing: those carry
	 * the envelope, the duration and the effect's type, so the effect is
	 * being redefined rather than moved, and then the whole set goes in
	 * order. That also keeps the rule that a bare ff_first or a bare
	 * ff_commit is never sent, which is a sequence no wheel has been seen
	 * receiving.
	 *
	 * -t restores the old behaviour for anyone who needs to compare, and
	 * exists because this changes the hot path of something that works.
	 */
	if (s->always_triple || moved[0] || moved[2]) {
		first = 0;
		last = 2;
	} else {
		first = 1;
		last = 1;
	}

	/*
	 * Recorded per packet and only once its write has succeeded, so a
	 * failure part way through leaves the slot believing exactly what
	 * reached the wheel and the next pass sends the rest.
	 */
	for (i = first; i <= last; i++) {
		if (emit(s, pkt[i].buf, pkt[i].len) != 0)
			return -1;
		sl->sent[i] = pkt[i];
	}

	return first == last ? 1 : 2;
}

static int
control(struct t150_session *s, uint8_t slot, int play, uint8_t iterations)
{
	uint8_t pkt[PKT_MAX];
	size_t n;

	n = t150_enc_control(pkt, sizeof(pkt), slot, play, iterations);

	return emit(s, pkt, n);
}

/*
 * Stop one slot, and separate the two facts a stop used to conflate.
 *
 * The game's intent goes down whether or not the wheel heard, because a stop
 * the wheel refused is still a stop the game asked for. Leaving playing set
 * was what let session_replay_starts, which reads it as "the game wants this
 * running", start the effect again after the wheel came back.
 *
 * What the refusal leaves is stop_owed, which keeps the slot alive so the
 * next pass and the watchdog can try again. Every release path in this file
 * goes through here, because the three that open-coded it disagreed: two
 * checked the write and refused to clear the slot, and the other two ignored
 * it and cleared the slot anyway, which forgot a wheel that was still pulling.
 *
 * Returns 0 when the wheel took the stop.
 */
static int
slot_stop(struct t150_session *s, uint8_t slot)
{
	struct t150_slot *sl = &s->slots[slot];
	int r = 0;

	if (sl->playing || sl->stop_owed)
		r = control(s, slot, 0, 0);

	sl->playing = 0;
	/*
	 * Nothing says a parameter packet is inert on a stopped slot, no wheel
	 * has ever been given one, and a pass that fired just after a stop
	 * would be asking that question of a wheel someone is holding.
	 */
	sl->dirty = 0;
	sl->stop_owed = r != 0;

	return r;
}

/* Whether any slot is still waiting for a stop the wheel would not take. */
static int
stops_owed(const struct t150_session *s)
{
	size_t i;

	for (i = 0; i < T150_SLOT_MAX; i++) {
		if (s->slots[i].used && s->slots[i].stop_owed)
			return 1;
	}

	return 0;
}

void
t150_session_init(struct t150_session *s, struct t150_backend *be,
    const char *token)
{
	memset(s, 0, sizeof(*s));
	s->be = be;
	/*
	 * Agreeing with the backend from the start. A new session has nothing
	 * on the wheel to reconcile, so beginning at 0 against a backend that
	 * has already acquired it made every session's first tick look like a
	 * re-acquire: a forget, a replay armed for slots that do not exist,
	 * and a second statement of the device settings the hello had only
	 * just made. A displaced session inherits that too, which is where it
	 * reached the wheel.
	 */
	s->epoch = atomic_load(&be->epoch);
	s->lost = atomic_load(&be->lost);
	s->gain = T150_DI_MAX;
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

/* Both packets, because the enable flag alone releases nothing. See A15. */
static void
emit_autocenter(struct t150_session *s, uint32_t force)
{
	uint8_t pkt[PKT_MAX];
	size_t n;

	n = t150_enc_autocenter_force(pkt, sizeof(pkt), force);
	(void)s->be->write(s->be->priv, pkt, n);
	n = t150_enc_autocenter_enable(pkt, sizeof(pkt), force != 0);
	(void)s->be->write(s->be->priv, pkt, n);
}

static void
session_safe_state(struct t150_session *s, const char *why)
{
	uint8_t stopped[T150_SLOT_MAX];
	unsigned int lost0 = atomic_load(&s->be->lost);
	size_t i;

	if (s->verbose && why != NULL)
		fprintf(stderr, "t150d: safe state: %s\n", why);

	/*
	 * Every stop goes out first and nothing is forgotten until they have
	 * all been answered for, because with a writer the answer to one of
	 * them is not known until the queue behind it has drained.
	 */
	for (i = 0; i < T150_SLOT_MAX; i++)
		stopped[i] = slot_stop(s, (uint8_t)i) == 0;

	/*
	 * With a writer thread those answers only say the packets were queued.
	 * Wait for the queue before believing any of them.
	 *
	 * A backend that writes on this thread reports the packet that failed
	 * and leaves the hook NULL, so this costs it nothing. One that answers
	 * at queue time cannot, and a stop merely queued and then refused let
	 * the loop below erase a slot the wheel was still rendering: armed
	 * went to 0 with it, so the watchdog never looked at that slot again
	 * and nothing anywhere could stop the force. Measured against this
	 * file, ten seconds of ticks later the stop had still not been sent.
	 */
	if (s->be->drain != NULL &&
	    (s->be->drain(s->be->priv) != 0 ||
	     atomic_load(&s->be->lost) != lost0))
		memset(stopped, 0, sizeof(stopped));

	/*
	 * The memset is load-bearing beyond forgetting the effect: it clears
	 * the slot's dirty flag and its record of what the wheel holds, so a
	 * safe state also cancels every pending emission. Moving either of
	 * those out of the slot would quietly leave a pass ready to write a
	 * force to a wheel that has just been made safe.
	 *
	 * A slot is only forgotten once its stop has actually reached the
	 * wheel. Forgetting one whose stop was refused is how this used to
	 * give up on the first failed write: used went to 0, so no later pass
	 * and no later safe state would ever look at that slot again, and the
	 * wheel kept pulling with nothing left in the daemon that could stop
	 * it. Anything else keeps the debt, which is what holds armed below.
	 */
	for (i = 0; i < T150_SLOT_MAX; i++) {
		if (stopped[i])
			memset(&s->slots[i], 0, sizeof(s->slots[i]));
		else if (s->slots[i].used)
			s->slots[i].stop_owed = 1;
	}
	s->io_err = 0;
	s->emit_failed = 0;

	/*
	 * The autocenter last. A wheel that has been left holding a force
	 * should end up as the person asked rather than fighting whoever grabs
	 * it next, and what they asked for is -a: zero by default, which is
	 * limp, and a spring for someone running a game that sends no forces
	 * at all. Writing a hard zero here undid -a on the first client to go
	 * away and nothing put it back until the wheel was replugged.
	 *
	 * It takes the force, not the enable flag. 0x04 only says whether the
	 * autocenter survives an application opening the wheel's input, and
	 * the effect is active whenever none has, so clearing it releases
	 * nothing. This code sent only 0x04 and therefore never made the
	 * wheel safe. See PROTOCOL.md and RESEARCH.md A15, which cost six
	 * hardware sessions to learn.
	 */
	emit_autocenter(s, s->autocenter);

	/*
	 * Disarming is what stops the watchdog evaluating, so it may only
	 * happen once the wheel is actually safe. While a stop is still owed
	 * the wheel may be holding a force, and staying armed is what brings
	 * the watchdog back to try again.
	 */
	s->armed = stops_owed(s);
}

/*
 * Everything the watchdog does, and then let the session go.
 *
 * The wheel's input is closed only when the daemon itself is leaving, which
 * is what force means here. It used to be closed on every client disconnect
 * as well, on the reasoning that a client which opened it owed the close, and
 * that contradicted the rule the backend states and this page documents: the
 * daemon holds the input open for as long as it holds the wheel. With the
 * input shut the firmware rests both pedals at maximum, so the next game to
 * enumerate the wheel calibrates them fully pressed and reads every press
 * backwards. Quitting one game should not do that to the next one.
 */
static void
session_release(struct t150_session *s, const char *why, int force)
{
	uint8_t pkt[PKT_MAX];
	size_t n;

	session_safe_state(s, why);

	if (force) {
		n = t150_enc_input_close(pkt, sizeof(pkt));
		(void)s->be->write(s->be->priv, pkt, n);
		s->input_open = 0;
	}
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

/*
 * Hand an outgoing session's unpaid stops to the one taking its place.
 *
 * A stop the wheel refused is a slot it may still be rendering, and the
 * newcomer's own table knows nothing about it. A displacement threw that debt
 * away with the session holding it, so the one thing that knew the wheel might
 * still be pulling went at the moment the wheel changed hands.
 *
 * Only the debt travels. The effect itself does not: the newcomer owns the
 * slot from here and will upload its own, and what is owed is a stop rather
 * than anything to play.
 */
void
t150_session_inherit_stops(struct t150_session *to,
    const struct t150_session *from)
{
	size_t i;

	for (i = 0; i < T150_SLOT_MAX; i++) {
		if (!from->slots[i].used || !from->slots[i].stop_owed)
			continue;
		to->slots[i].used = 1;
		to->slots[i].stop_owed = 1;
	}
}

/*
 * The device settings a client inherits rather than asks for.
 *
 * The wheel keeps a device gain of its own and nothing here ever set it, so
 * every force was scaled by whatever the wheel powered up with or whatever
 * the last process left behind. DirectInput's device gain defaults to full
 * and a game that wants less says so with DIPROP_FFGAIN, so full is the
 * honest starting point: it means do not attenuate, and it leaves the
 * strength where the game's own settings put it.
 *
 * It is the gain the client last asked for that goes back, not the literal
 * full scale this used to send. The proxy sends DIPROP_FFGAIN once and has no
 * path to send it again, so writing full here was the end of the driver's
 * chosen strength: a wheel re-acquired mid race, or a daemon restarted under
 * a running game, put every force back to full and nothing said so.
 *
 * The rotation range has no DirectInput property at all. On Windows it is
 * set in the vendor's control panel and a game merely assumes the wheel is
 * already at the number in its settings, so a game asking for 900 degrees
 * reaches nothing. -r is that control panel, and leaving it unset leaves the
 * wheel's own range alone.
 *
 * Both are sent on hello and again whenever the wheel has been re-acquired,
 * because a wheel that has been away has forgotten them.
 */
static void
session_apply_settings(struct t150_session *s)
{
	uint8_t pkt[PKT_MAX];
	size_t n;

	/*
	 * Both writes are reported rather than discarded. A tester asked why
	 * setting the range changed nothing and no log anywhere could say
	 * whether the packet had reached the wheel, been refused, or never
	 * been sent because the option was not passed. Three different faults
	 * with one silence between them is not a diagnosis anybody can make.
	 */
	if ((n = t150_enc_gain(pkt, sizeof(pkt), s->gain)) > 0) {
		int r = emit(s, pkt, n);

		if (s->verbose)
			fprintf(stderr, "t150d: device gain set to %u of %u: "
			    "%s\n", s->gain, (unsigned)T150_DI_MAX,
			    r == 0 ? "sent" : "the write failed");
	}
	/*
	 * The centring spring goes back too, but only when the client chose
	 * one: it set it once and has no reason to say it again, and a wheel
	 * that has been away has forgotten it. The -a value is not re-stated
	 * here, because the backend applies that on every acquire and the safe
	 * state restores it, so saying it again would be two packets telling
	 * the wheel what it already holds.
	 */
	if (s->client_set_autocenter)
		emit_autocenter(s, s->client_autocenter);

	if (s->range_deg == 0) {
		if (s->verbose)
			fprintf(stderr, "t150d: no rotation range given, the "
			    "wheel keeps its own. See -r\n");
	} else if ((n = t150_enc_range(pkt, sizeof(pkt), s->range_deg)) > 0) {
		int r = emit(s, pkt, n);

		if (s->verbose)
			fprintf(stderr, "t150d: rotation range set to %u "
			    "degrees: %s\n", s->range_deg,
			    r == 0 ? "sent" : "the write failed");
	}
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
	uint8_t want, was_playing, was_owed, iterations;

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

	/*
	 * Refused here, where there is a frame to answer with, rather than in
	 * an emission pass.
	 *
	 * proto.c already tells clients to expect this: an unknown kind is not
	 * a frame error, the daemon answers T150_ERR_UNSUPPORTED. Nothing sent
	 * it. The effect was stored instead, and the pass that discovered it
	 * could not be encoded had no frame left, so it set io_err and the
	 * refusal was charged to whichever later upload came next. Worse, that
	 * pass was the one release path in this file that did not go through
	 * slot_stop.
	 */
	if (!t150_effect_encodable(ef.kind)) {
		if (s->verbose)
			fprintf(stderr, "t150d: slot %u: effect %u is not one "
			    "this wheel can be given\n", ef.slot, want);
		reply_err(rep, T150_ERR_UNSUPPORTED);
		return;
	}

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
	 * A stop the wheel refused is owed whatever the game does next, so it
	 * survives here too. Re-using a slot does not settle that debt: what
	 * the wheel may still be rendering is the effect that was there
	 * before. Clearing it left nothing anywhere able to stop the wheel,
	 * because the emitter's retry and every safe state both ask whether a
	 * stop is owed before they send one.
	 */
	was_owed = sl->used ? sl->stop_owed : 0;
	/*
	 * What the wheel already holds has to survive this, or every upload
	 * would look like the first one and the comparison in flush_slot
	 * would never match. It is the one field here that describes the
	 * wheel rather than the game's wishes.
	 */
	memcpy(sent, sl->sent, sizeof(sent));

	memset(sl, 0, sizeof(*sl));
	sl->playing = was_playing;
	sl->stop_owed = was_owed;
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
	 * A ramp already running keeps the level it has slid to.
	 *
	 * The wheel has no ramp, so the daemon walks it and this is the only
	 * place the rendered level is set from anything but the slicer.
	 * Rewinding it to the ramp's beginning is right for the upload a
	 * restart carries, and do_start does that now. It is wrong for the
	 * bare SetParameters a game makes on a force it is animating, which
	 * reaches here just the same and cannot be told apart: the slide had
	 * not restarted, so the wheel was thrown back to the start value and
	 * the slicer walked it forward again up to T150_RAMP_TICK_MS later.
	 *
	 * Measured against this file, a ramp releasing from full scale to
	 * nothing with SetParameters at 60 Hz went back UP on twenty four of
	 * its forty nine writes, to full scale every time, while the game
	 * believed it was easing the force off.
	 */
	if (want == T150_EFFECT_RAMP && was_playing)
		sl->ef.u.constant.magnitude =
		    apply_gain(ramp_level(sl, s->last_frame_ms), sl->ef.gain);

	/*
	 * What the game is actually asking for, at most once a second so a
	 * game updating at its physics rate does not drown the terminal.
	 *
	 * Nothing has ever recorded this. A wheel that hunts around dead
	 * centre and settles the moment it is touched is a force that changes
	 * sign across zero with nothing to stop it, so the deadband and the
	 * centre are the numbers worth seeing, and neither has been looked at
	 * once in this project's life.
	 */
	/* t150_session_frame stamped last_frame_ms with now on the way in. */
	if (s->verbose &&
	    s->last_frame_ms - s->last_param_log_ms >= 1000) {
		s->last_param_log_ms = s->last_frame_ms;
		switch (ef.kind) {
		case T150_EFFECT_SPRING:
		case T150_EFFECT_DAMPER:
			/*
			 * Which one, and not just "condition". A spring
			 * resists displacement from a centre and a damper
			 * resists velocity, so only one of them can produce a
			 * vibration anchored to a position. The tester
			 * reported exactly that, at dead centre and again at
			 * about 135 degrees, and this line said "condition"
			 * for both kinds and could not tell them apart.
			 *
			 * The kind after any downgrade, which is what the
			 * wheel is actually given. A game asking for friction
			 * or inertia gets a damper and says so on its own
			 * line above.
			 */
			fprintf(stderr, "t150d: slot %u %s: centre %d, "
			    "coeff %d/%d, saturation %d/%d, deadband %d\n",
			    ef.slot, kind_name(ef.kind),
			    ef.u.condition.center,
			    ef.u.condition.pos_coeff, ef.u.condition.neg_coeff,
			    ef.u.condition.pos_saturation,
			    ef.u.condition.neg_saturation,
			    ef.u.condition.deadband);
			break;
		case T150_EFFECT_CONSTANT:
			fprintf(stderr, "t150d: slot %u constant: magnitude %d, "
			    "direction %u, gain %u\n", ef.slot,
			    ef.u.constant.magnitude, ef.direction, ef.gain);
			break;
		default:
			fprintf(stderr, "t150d: slot %u effect kind %u\n",
			    ef.slot, ef.kind);
			break;
		}
	}
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
	 *
	 * Unconditionally, not only when the slot is marked dirty. dirty says
	 * a pass is owed, and a stop clears it deliberately so that nothing
	 * follows an effect that has just stopped; nothing but an upload ever
	 * sets it again. A game that uploads, stops before the pass has run,
	 * and then starts - which is what the proxy sends whenever a game
	 * passes DIES_NODOWNLOAD - therefore had the wheel told to play
	 * parameters it had never been given.
	 *
	 * flush_slot writes nothing when the encoded bytes already match what
	 * the slot last put on the wire, so the common path costs a comparison
	 * rather than three packets.
	 *
	 * The emit deadline is deliberately not pushed here. It is one
	 * deadline for the whole session, and pushing it from a frame starves
	 * every slot the frame did not touch: a game whose physics runs faster
	 * than the emit period moves the deadline further away than its own
	 * next frame, so the pass never runs and every other slot keeps
	 * whatever the wheel was last told. Assetto Corsa at 333 Hz against a
	 * 4 ms period did exactly that, and its damper went half a second
	 * without an update while its constant force was started on every
	 * frame. Only an emission pass moves the deadline.
	 */
	/*
	 * A ramp begins again from its beginning, and this is where that
	 * happens rather than in do_upload.
	 *
	 * The daemon walks the slide, so nothing else can put the level back.
	 * do_upload used to, which served the upload a restart carries and
	 * broke every bare SetParameters on a running ramp; a start is the
	 * event that actually means begin again, and it is the one the proxy
	 * always sends before it expects the ramp from the top. The clock goes
	 * back with the level, before the flush, so the pass below writes the
	 * start value rather than one the slicer has already moved.
	 */
	if (sl->source_kind == T150_EFFECT_RAMP) {
		int32_t level = apply_gain(sl->ramp.start, sl->ef.gain);

		if (sl->ef.u.constant.magnitude != level) {
			sl->ef.u.constant.magnitude = level;
			sl->dirty = 1;
		}
		sl->started_ms = now_ms;
	}

	if (flush_slot(s, sl) < 0) {
		/* Reported here, so the next upload does not repeat it. */
		s->io_err = 0;
		reply_err(rep, T150_ERR_DEVICE_IO);
		return;
	}
	sl->dirty = 0;

	/*
	 * The intent is recorded before the write, not after it, which is the
	 * same rule hid_darwin.c applies to the wheel's input state and for
	 * the same reason. A start refused because the wheel is off the bus
	 * used to be forgotten entirely: the flag stayed clear, the
	 * re-acquire below had nothing to replay, and the game never asked
	 * again because from its side the effect was already running. The
	 * wheel came back with every effect loaded and stopped, and force
	 * feedback was gone for the rest of the session. Test 35's proxy log
	 * is that failure: two refused starts, then a game that carried on
	 * happily and was never told anything was wrong.
	 */
	/*
	 * Said once, when the slot actually changes state. Assetto Corsa
	 * starts an already playing slot on every frame, so logging the call
	 * rather than the transition would put 333 lines a second into a
	 * report. What a reader needs is whether a slot was ever started at
	 * all: an effect uploaded and never started renders nothing, and
	 * nothing here could tell that apart from one that plays badly.
	 */
	if (s->verbose && !sl->playing)
		fprintf(stderr, "t150d: slot %u %s started\n", payload[0],
		    kind_name(sl->ef.kind));

	sl->playing = 1;
	/*
	 * A start also settles any stop still owed on this slot. Both flags say
	 * the same thing, that the wheel may be rendering it, and every release
	 * path acts on either, so nothing is lost by clearing this one. Left
	 * standing it was acted on: the proxy uploads before it starts, so a
	 * game playing an effect again sends stop, upload and start in one
	 * burst, and the next emission pass stopped and released the very
	 * effect the game had just started.
	 */
	sl->stop_owed = 0;
	sl->iterations = payload[1];
	sl->started_ms = now_ms;

	if (control(s, payload[0], 1, payload[1]) != 0) {
		reply_err(rep, T150_ERR_DEVICE_IO);
		return;
	}

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

	/* The transition only, for the reason do_start says. */
	if (s->verbose && sl->playing)
		fprintf(stderr, "t150d: slot %u %s stopped\n", payload[0],
		    kind_name(sl->ef.kind));

	if (slot_stop(s, payload[0]) != 0) {
		reply_err(rep, T150_ERR_DEVICE_IO);
		return;
	}

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
		/*
		 * Remembered, because the wheel forgets it and nothing else
		 * can put it back. See session_apply_settings.
		 */
		s->gain = v > (uint32_t)T150_DI_MAX ? (uint32_t)T150_DI_MAX : v;
		break;
	case T150_OP_SET_RANGE:
		n = t150_enc_range(pkt, sizeof(pkt), (unsigned int)v);
		if (emit(s, pkt, n) != 0) {
			reply_err(rep, T150_ERR_DEVICE_IO);
			return;
		}
		/*
		 * Remembered like the two beside it, so a wheel that has been
		 * away comes back at what this client asked for rather than at
		 * whatever -r started the daemon with. Nothing sends this
		 * today, because DirectInput has no property for a rotation
		 * range, but the op exists and dropping what it carried made it
		 * the one device setting a re-acquire did not put back.
		 */
		s->range_deg = (unsigned int)v;
		break;
	case T150_OP_SET_AUTOCENTER:
		/*
		 * Zero means off, anything else is a strength and an enable.
		 * Off is the force, not the enable flag: clearing 0x04 alone
		 * leaves the wheel gripped, which is PROTOCOL.md's warning
		 * and was measured on hardware.
		 */
		n = t150_enc_autocenter_force(pkt, sizeof(pkt), v);
		if (emit(s, pkt, n) != 0) {
			reply_err(rep, T150_ERR_DEVICE_IO);
			return;
		}
		n = t150_enc_autocenter_enable(pkt, sizeof(pkt), v != 0);
		if (emit(s, pkt, n) != 0) {
			reply_err(rep, T150_ERR_DEVICE_IO);
			return;
		}
		/* Remembered, so a re-acquired wheel gets it back. */
		s->client_autocenter = v > (uint32_t)T150_DI_MAX ?
		    (uint32_t)T150_DI_MAX : v;
		s->client_set_autocenter = 1;
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

	int failed = 0;

	/*
	 * DirectInput's reset stops and releases every effect. It says
	 * nothing about the autocenter, which the game sets separately, so
	 * unlike the watchdog this leaves it alone.
	 *
	 * Every slot is tried before anything is reported, so one refusal does
	 * not leave the rest playing, and a slot whose stop was refused keeps
	 * its record: releasing it here while the wheel still rendered it left
	 * nothing in the daemon able to stop it, and answered OK while doing
	 * so.
	 */
	for (i = 0; i < T150_SLOT_MAX; i++) {
		if (slot_stop(s, (uint8_t)i) != 0)
			failed = 1;
		else
			memset(&s->slots[i], 0, sizeof(s->slots[i]));
	}

	if (failed) {
		reply_err(rep, T150_ERR_DEVICE_IO);
		return;
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

	int failed = 0;

	/*
	 * Every slot, then the answer. Returning from inside the loop left
	 * every slot above the one that failed still playing, which is the
	 * opposite of what STOPALL was asked to do.
	 */
	for (i = 0; i < T150_SLOT_MAX; i++) {
		if (!s->slots[i].used)
			continue;
		if (slot_stop(s, (uint8_t)i) != 0)
			failed = 1;
	}

	if (failed) {
		reply_err(rep, T150_ERR_DEVICE_IO);
		return;
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
		if (s->pending)
			s->settings_owed = 1;
		else
			session_apply_settings(s);
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

/*
 * Whether a slot's own duration has already run out.
 *
 * The wheel is given a length in its commit and ends the effect itself, so
 * nothing here has to stop one. What this exists for is the re-play in the
 * emitter: a play packet restarts that countdown, and a ramp is slid by the
 * daemon right up to its end, so the last recompute lands at or just after
 * the moment the wheel was going to stop. Re-playing there held the force for
 * another whole duration.
 */
static int
slot_expired(const struct t150_slot *sl, uint64_t now_ms)
{
	uint64_t total;

	if (sl->ef.duration == T150_DURATION_INFINITE || sl->ef.duration == 0)
		return 0;

	/*
	 * Everything the wheel was told, not the duration alone.
	 *
	 * The commit carries a start delay and the play packet carries an
	 * iteration count, so the window the wheel renders in begins at
	 * started_ms + start_delay and lasts iterations durations. This
	 * measured one duration from the start, which cost nothing while the
	 * only reader was the re-play guard: being early there merely skipped
	 * a re-play. It stopped being free when the tick began stopping a slot
	 * on this answer. A game asking for a three second delay had its
	 * effect stopped two seconds before the wheel would have begun it, and
	 * one asking for three iterations had two of them cut off.
	 *
	 * The iteration count is read the way t150_enc_control writes it: zero
	 * means one pass.
	 */
	total = (uint64_t)(sl->ef.start_delay / 1000) +
	    (uint64_t)(sl->iterations > 0 ? sl->iterations : 1) *
	    (uint64_t)(sl->ef.duration / 1000);

	return now_ms - sl->started_ms >= total;
}

/* Forget what the wheel was believed to hold, and mean to teach it again. */
static void
session_forget_wheel(struct t150_session *s)
{
	size_t i;

	for (i = 0; i < T150_SLOT_MAX; i++) {
		memset(s->slots[i].sent, 0, sizeof(s->slots[i].sent));
		/*
		 * Only a slot that holds something to teach. A slot inherited
		 * from a displaced session carries nothing but that session's
		 * unpaid stop, and marking it dirty is read by session_emit as
		 * "the game has put a new effect here", which is what keeps it
		 * from releasing the slot once the stop is paid. The pass then
		 * cannot encode a kind that was never set, and the write error
		 * that follows is answered to the next EFFECT_UPLOAD, which had
		 * succeeded.
		 */
		if (s->slots[i].used &&
		    s->slots[i].ef.kind != T150_EFFECT_NONE)
			s->slots[i].dirty = 1;
	}
}

/*
 * A packet the backend took and then dropped means the wheel is not holding
 * what this session believes, for some slot it cannot name.
 *
 * Not the same as a re-acquire, and it took a review to see the difference.
 * A re-acquire scrubs the wheel, so every loaded slot has to be taught again;
 * a dropped packet leaves the wheel exactly as it was minus one write. Sending
 * it through session_forget_wheel therefore re-dirtied slots a stop had
 * deliberately un-dirtied, and the next pass wrote effect parameters to a slot
 * that had just been stopped - which slot_stop's own comment rules out, since
 * no wheel has ever been given one and a pass firing just after a stop asks
 * that question of a wheel somebody is holding.
 *
 * So only the record of what the wheel holds goes. A slot that is playing is
 * marked for teaching again, because nothing else will; one that is not is
 * left alone, and do_start flushes it unconditionally when the game plays it.
 */
static void
session_forget_wire(struct t150_session *s)
{
	size_t i;

	for (i = 0; i < T150_SLOT_MAX; i++) {
		memset(s->slots[i].sent, 0, sizeof(s->slots[i].sent));
		if (s->slots[i].used && s->slots[i].playing &&
		    s->slots[i].ef.kind != T150_EFFECT_NONE)
			s->slots[i].dirty = 1;
	}
}

/*
 * Put back what the game asked for, after the wheel has been away.
 *
 * The parameters are re-taught by the pass, because forgetting the wire
 * state marks every used slot dirty. A start is not a parameter and no pass
 * will send one, so this does, for exactly the slots the game had asked to
 * play. It deliberately did not, once, on the reasoning that a wheel which
 * begins pushing by itself after a replug is not an improvement. That
 * reasoning was wrong in the case that actually happens: the game believes
 * its effect is still running, so it never starts it again, and the choice
 * is not between a quiet wheel and a pushing one but between force feedback
 * that comes back and force feedback that is gone until the game restarts.
 *
 * The order matters. The parameters have to be on the wheel before it is
 * told to play them, so this runs after the emission pass rather than with
 * the invalidation above.
 */
static int
session_replay_starts(struct t150_session *s)
{
	size_t i;
	int failed = 0;

	for (i = 0; i < T150_SLOT_MAX; i++) {
		struct t150_slot *sl = &s->slots[i];

		if (!sl->used || !sl->playing || sl->dirty)
			continue;
		if (s->verbose)
			fprintf(stderr, "t150d: slot %u was playing before the "
			    "wheel went, starting it again\n", (unsigned)i);
		if (control(s, (uint8_t)i, 1, sl->iterations) != 0)
			failed = 1;
	}

	return failed;
}

/*
 * Whether a pass may run ahead of its deadline.
 *
 * T150_EMIT_MS is a floor on how often the wheel is written to, and it was
 * put there when every write was a synchronous IOKit call on the thread the
 * game was waiting for: pacing the daemon was the only thing keeping a game's
 * frame out of a burst of USB transfers. A backend with a writer thread paces
 * itself instead, and says so by answering this, so the floor buys nothing
 * while that thread has no backlog and costs up to a whole period of
 * latency on a force the wheel could take right now.
 *
 * It reasserts itself the moment the writer falls behind, which is the case
 * the floor was really for: a queue with anything in it is a wheel already
 * taking packets as fast as it can, and a pass built now would only be
 * coalesced into one already waiting.
 *
 * A pass that failed is never brought forward. The deadline is what stops a
 * wheel that has gone turning the retry into a spin, and emit_failed is how
 * session_emit says that happened.
 *
 * Off unless -E asks for it. It was the default in 0.2.2, 0.2.3 and 0.2.4 and
 * is not now: it is the only change to how this daemon drives the wheel in
 * about thirty releases, and force feedback began stopping mid session in the
 * release that introduced it, with the wheel found back at its boot identity.
 * Nothing proves the two are connected, and nobody has ever felt this help
 * either, so the suspicion is not worth carrying by default. RESEARCH.md A51.
 */
static int
emit_now(const struct t150_session *s)
{
	return s->early_pass && !s->emit_failed && s->be->idle != NULL &&
	    s->be->idle(s->be->priv);
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
 * slots and not before the deadline, which is T150_EMIT_MS after the last
 * pass and which emit_now above may bring forward. Resumes where the previous
 * pass stopped so a storm on the low slots cannot starve the high ones.
 */
static void
session_emit(struct t150_session *s, uint64_t now_ms)
{
	size_t i, done = 0;

	s->emit_failed = 0;

	/*
	 * A stop the wheel would not take comes first, and on the emitter's
	 * cadence rather than every tick, so a wheel that has gone does not
	 * turn this into a spin. Nothing else retries one: the watchdog only
	 * fires when the client has gone quiet, and a game that carries on
	 * talking would keep it silent for as long as it ran.
	 */
	for (i = 0; i < T150_SLOT_MAX; i++) {
		struct t150_slot *owing = &s->slots[i];
		int reloaded;

		if (!owing->used || !owing->stop_owed)
			continue;
		/*
		 * Whether the game has put a new effect in this slot since the
		 * stop was refused. The stop still has to go, because it is the
		 * effect before it that the wheel may be rendering, but
		 * slot_stop clears the flag that says the new parameters are
		 * owed to the wheel, and they still are.
		 */
		reloaded = owing->dirty;
		if (slot_stop(s, (uint8_t)i) != 0) {
			s->emit_failed = 1;
			continue;
		}
		/*
		 * The slot is left exactly as a stop that landed at once would
		 * have left it, which for a plain stop means loaded and ready
		 * to be started again. Releasing it here made a refused write
		 * the difference between a stop and a destroy: the game's next
		 * start, if it trusted the effect to still be downloaded, was
		 * answered with a slot that no longer existed. Whoever wanted
		 * the slot gone releases it on its own path, and a safe state
		 * clears anything left behind.
		 *
		 * Except a slot that holds no effect, which is what a debt
		 * inherited from a displaced session is: there is nothing in it
		 * to keep once the stop it existed for has gone. Left behind,
		 * the next re-acquire marks it dirty like any other used slot,
		 * the pass cannot encode a kind that was never set, and the
		 * write error that follows is answered to the next upload,
		 * which had succeeded.
		 */
		if (reloaded)
			owing->dirty = 1;
		else if (owing->ef.kind == T150_EFFECT_NONE)
			memset(owing, 0, sizeof(*owing));
	}

	for (i = 0; i < T150_SLOT_MAX && done < T150_EMIT_SLOTS; i++) {
		size_t k = (s->next_slot + i) % T150_SLOT_MAX;
		struct t150_slot *sl = &s->slots[k];
		int r;

		if (!sl->used || !sl->dirty)
			continue;

		done++;
		if ((r = flush_slot(s, sl)) >= 0) {
			sl->dirty = 0;
			/*
			 * A constant force whose level has just moved is
			 * played again, because that is what the wheel is
			 * given by the driver that works. Thrustmaster's
			 * DirectInput capture,
			 * tmp/oldffb/directX_constforce.pcapng, pairs 24 of
			 * its 25 bare updates with 41 xx 41 01 immediately
			 * after, and the tester felt the difference when this
			 * daemon sent the update alone: the force was there
			 * and worse. Its control panel captures for a spring
			 * and a sine pair none at all, so this is deliberately
			 * limited to the constant, which is also the only
			 * shape any capture here shows being modulated.
			 *
			 * Only after a bare update. A full upload ends in its
			 * own commit and the game's own start follows it.
			 */
			if (r == 1 && sl->playing &&
			    sl->ef.kind == T150_EFFECT_CONSTANT &&
			    !slot_expired(sl, now_ms))
				(void)control(s, (uint8_t)k, 1,
				    sl->iterations > 0 ? sl->iterations : 1);
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
		 *
		 * Through slot_stop, like every other release in this file.
		 * Open-coding it threw away the one answer that matters: a
		 * refused stop left the slot forgotten while the wheel went on
		 * rendering it, which is the failure slot_stop exists to
		 * prevent. A refusal keeps the debt now, and clearing the kind
		 * is what lets the retry loop release the slot once the stop
		 * lands, since nothing here is worth keeping.
		 *
		 * do_upload refuses an unencodable kind at the door, so this
		 * is defence rather than a path anything reaches today.
		 */
		if (slot_stop(s, (uint8_t)k) == 0) {
			memset(sl, 0, sizeof(*sl));
		} else {
			sl->ef.kind = T150_EFFECT_NONE;
			sl->source_kind = T150_EFFECT_NONE;
		}
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
	 * the wheel again, and then replay the starts: see
	 * session_replay_starts for why a wheel that begins pushing again
	 * after a replug is the outcome to want rather than the one to avoid.
	 */
	if (s->epoch != s->be->epoch) {
		s->epoch = s->be->epoch;
		session_forget_wheel(s);
		s->replay_starts = 1;
		/*
		 * A wheel that has been away has forgotten its gain and its
		 * rotation range as well as its effects, and only a session
		 * that said hello is owed them back.
		 */
		if (s->hello)
			session_apply_settings(s);
	}

	/*
	 * A packet the backend dropped. Nothing here can say which, so what
	 * this can do is stop believing the wheel holds what it was told, and
	 * owe the error to the next upload the way a synchronous refusal does.
	 */
	if (s->lost != atomic_load(&s->be->lost)) {
		s->lost = atomic_load(&s->be->lost);
		session_forget_wire(s);
		/*
		 * And the starts, because the packet that went missing may
		 * have been one: a play is answered 0 at queue time like any
		 * other, so the game believes its effect is running and will
		 * never ask again. Re-teaching the parameters alone would
		 * leave the wheel loaded and silent. A start it is already
		 * playing costs one packet, which is what every re-acquire
		 * and the emitter's own re-play both already spend.
		 */
		s->replay_starts = 1;
		s->io_err = 1;
	}

	/*
	 * What a displacing client's hello could not say yet: it proved its
	 * token before the incumbent had been made safe, so its settings
	 * waited for the caller to do that.
	 */
	if (s->settings_owed) {
		s->settings_owed = 0;
		session_apply_settings(s);
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
	 * An effect whose own duration has run out is stopped here rather than
	 * left to the wheel.
	 *
	 * The wheel is given a length in its commit and does end the effect by
	 * itself, so for a long time this was nobody's job. What made it one is
	 * the re-play in the emitter below: a play packet is understood to
	 * restart that countdown, so an effect re-played while its level moved
	 * ends one whole duration after the last re-play rather than after its
	 * own start. Measured against this file, a one second ramp had its last
	 * re-play at 980 ms, carrying full scale, which would hold the wheel
	 * there until 1980.
	 *
	 * Ending it when the game asked is right whether or not a play restarts
	 * anything. If it does, this is what stops the force on time. If it does
	 * not, the wheel stopped by itself already and this is a stop for a slot
	 * that is no longer playing, which costs one packet and changes nothing.
	 * RESEARCH.md has never measured which of the two is true, and this is
	 * the answer that does not need it.
	 *
	 * Stopped rather than released, like every other stop: the effect stays
	 * downloaded and the game may start it again.
	 */
	for (i = 0; i < T150_SLOT_MAX; i++) {
		struct t150_slot *sl = &s->slots[i];

		if (!sl->used || !sl->playing || !slot_expired(sl, now_ms))
			continue;
		if (s->verbose)
			fprintf(stderr, "t150d: slot %u %s reached the end of "
			    "its duration\n", (unsigned)i, kind_name(sl->ef.kind));
		(void)slot_stop(s, (uint8_t)i);
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

	if ((slots_dirty(s) || stops_owed(s)) &&
	    (now_ms >= s->next_emit_ms || emit_now(s)))
		session_emit(s, now_ms);

	/*
	 * Once the parameters are actually on the wheel, and not before.
	 * A slot whose emission failed stays dirty and is skipped, so a
	 * wheel that is still going away is not told to play anything.
	 */
	/*
	 * The flag is only cleared once the wheel has taken the starts. It
	 * carries the one piece of knowledge nothing else holds - that the
	 * game believes effects are running which the wheel no longer has -
	 * and clearing it before the write threw that away on the first
	 * refusal. do_start records the same intent before its own write, for
	 * the same reason and citing the same hardware session.
	 */
	if (s->replay_starts && !slots_dirty(s) && !session_replay_starts(s))
		s->replay_starts = 0;

	/*
	 * The timeout is the soonest thing that has to happen. Work that is
	 * only pending does not shorten it, which is what keeps an idle
	 * daemon sleeping until the watchdog rather than waking to find
	 * nothing to do.
	 */
	if (sliding && s->next_ramp_ms > now_ms &&
	    next > (unsigned int)(s->next_ramp_ms - now_ms))
		next = (unsigned int)(s->next_ramp_ms - now_ms);
	if ((slots_dirty(s) || stops_owed(s)) && !s->emit_failed &&
	    s->next_emit_ms > now_ms &&
	    next > (unsigned int)(s->next_emit_ms - now_ms))
		next = (unsigned int)(s->next_emit_ms - now_ms);

	return next;
}
