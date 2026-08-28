/*
 * effect.c - one IDirectInputEffect, backed by a slot on the wheel.
 *
 * This is where DirectInput's idea of an effect becomes the normalized one
 * the daemon speaks. No wheel knowledge lives here and no DirectInput
 * knowledge lives past here.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "proxy.h"

struct effect_obj {
	IDirectInputEffect	 iface;
	LONG			 refs;
	struct t150_device	*dev;
	GUID			 guid;
	int			 slot;
	int			 playing;
	/*
	 * A start the game asked for that the daemon was never told about,
	 * because the upload it rides on failed or the start itself was
	 * refused. Nothing else would ever say it again: the reconnect replay
	 * in upload() wants playing, the continue wants paused, and the daemon
	 * replays only what it already believes it started, so one transient
	 * error used to cost that force for the rest of the run. The daemon
	 * keeps the same kind of record a slot at a time and calls it
	 * stop_owed.
	 *
	 * Sent by the first upload that reaches the daemon, if the game makes
	 * one, and dropped by everything that means the game no longer wants
	 * it: a stop, an unload, a device reset, a stop-everything and a
	 * pause. How many passes
	 * it is worth is in iterations below, which start_effect records
	 * before anything can fail for exactly this reason.
	 *
	 * Interlocked, like refs above, because there is no lock over one
	 * effect's own state: the registry lock covers the array, and nothing
	 * may hold it across a round trip to the daemon.
	 */
	LONG			 start_owed;
	/*
	 * What the game passed to Start, which nothing kept: the reconnect
	 * replay below built its own payload with a hard coded 1, so a rumble
	 * a game had asked to repeat twenty times came back as one pass of it
	 * after the daemon was restarted. The daemon keeps this per slot and
	 * uses it for its own replay, so the loss was entirely on this side.
	 */
	uint8_t			 iterations;
	/*
	 * Stopped by a pause rather than by the game, so a continue knows what
	 * to put back. DISFFC_PAUSE has no wire opcode, so the proxy sends a
	 * stop-everything for it, and that lost which effects had been running.
	 */
	int			 paused;
	/* When the last Start went, so a finite effect's own end can be known. */
	ULONGLONG		 started_ms;
	int			 logged;	/* the first Start is logged, not every one */
	unsigned int		 gen;		/* the connection this was uploaded to */
	/* What the daemon last acknowledged, and when. See upload(). */
	int			 sent_valid;
	ULONGLONG		 sent_ms;
	LONG			 sent_unload_gen;
	uint8_t			 sent[T150_PROTO_EFFECT_LEN];
	struct t150_effect	 ef;
};

static struct effect_obj *
from_iface(IDirectInputEffect *p)
{
	return (struct effect_obj *)p;
}

/*
 * Every effect object that is currently alive, indexed by the slot it holds.
 *
 * A slot is taken for exactly as long as its effect object exists and no two
 * live objects share one, so the slot map doubles as the registry and there
 * is no list to keep or lock. It exists because a device otherwise has no way
 * to reach the effects it created: dev_EnumCreatedEffectObjects had nothing
 * to walk and answered DI_OK without calling the callback once, which tells a
 * game that walks its effects to stop them that it has finished a job it
 * never started.
 *
 * Under a lock rather than an interlocked write, which is what this had. Two
 * game threads may create and release effects at once, and DirectInput permits
 * it; an atomic store makes the slot's publication indivisible and gives a
 * reader nothing, because a reader that has already loaded the pointer holds
 * one the writer knows nothing about. eff_Release clears the entry and frees
 * the object three lines later, so a walk that had just read that slot could
 * write into freed memory.
 *
 * The lock is only ever held around the array. Anything that has to talk to
 * the daemon takes a reference under it and lets go first, because
 * eff_Release makes a round trip of its own and the two locks would otherwise
 * be taken in both orders.
 */
static struct effect_obj *live[T150_SLOT_MAX];
static CRITICAL_SECTION registry;

void	t150_effect_init_lock(void);
void	t150_effect_free_lock(void);

void
t150_effect_init_lock(void)
{
	InitializeCriticalSection(&registry);
}

void
t150_effect_free_lock(void)
{
	DeleteCriticalSection(&registry);
}

static void
remember(struct effect_obj *e)
{
	if (e->slot < 0 || e->slot >= (int)T150_SLOT_MAX)
		return;
	EnterCriticalSection(&registry);
	live[e->slot] = e;
	LeaveCriticalSection(&registry);
}

/*
 * Drop one reference, and take the object out of the registry if that was the
 * last one.
 *
 * Both under the lock, because the registry is what hands references out.
 * Decrementing outside it left a window in which a walk could find the object,
 * take a reference to it, and go on using it while the thread that reached
 * zero was freeing it.
 */
static LONG
release_and_forget(struct effect_obj *e)
{
	LONG r;

	EnterCriticalSection(&registry);
	r = InterlockedDecrement(&e->refs);
	if (r == 0 && e->slot >= 0 && e->slot < (int)T150_SLOT_MAX &&
	    live[e->slot] == e)
		live[e->slot] = NULL;
	LeaveCriticalSection(&registry);

	return r;
}

/*
 * Everything live, each with a reference held, so the caller can work on them
 * with the lock let go. Returns how many it put in out, and every one of them
 * has to be released afterwards.
 */
static size_t
registry_snapshot(struct effect_obj **out, const struct t150_device *dev)
{
	size_t i, n = 0;

	EnterCriticalSection(&registry);
	for (i = 0; i < T150_SLOT_MAX; i++) {
		struct effect_obj *e = live[i];

		if (e == NULL || (dev != NULL && e->dev != dev))
			continue;
		(void)InterlockedIncrement(&e->refs);
		out[n++] = e;
	}
	LeaveCriticalSection(&registry);

	return n;
}

/*
 * Walk the live effects belonging to one device.
 *
 * DirectInput hands the callback the interface without adding a reference, so
 * a callback is free to release it: nothing is dereferenced after the call,
 * and the release itself is what clears the entry.
 */
HRESULT
t150_effect_enum(struct t150_device *dev,
    LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref)
{
	struct effect_obj *seen[T150_SLOT_MAX];
	size_t n, i;
	int stop = 0;

	n = registry_snapshot(seen, dev);

	for (i = 0; i < n; i++) {
		if (!stop && cb(&seen[i]->iface, ref) == DIENUM_STOP)
			stop = 1;
		IDirectInputEffect_Release(&seen[i]->iface);
	}

	return DI_OK;
}

/*
 * Every effect this process holds is stopped, as far as the game is concerned.
 *
 * The device level commands stop everything on the wheel without naming a
 * single effect, so nothing could clear the flag that says an object is
 * playing: an effect stopped by DISFFC_RESET, DISFFC_STOPALL or an unacquire
 * stayed marked as playing, and the replay that follows a reconnect then
 * started it again. A game that had deliberately stopped its forces could be
 * handed a pulling wheel by a daemon restart. It also had GetEffectStatus
 * answer that a stopped effect was still running.
 *
 * Every effect rather than the ones the device that took the command created,
 * because the daemon keeps one slot table per connection and the proxy opens
 * one connection per process: the stop it sends reaches every slot, whichever
 * wrapped device asked for it. SDL makes exactly that shape, a second device
 * for the same wheel, and scoping this to one of them left the other's effects
 * believing they still played.
 */
void
t150_effect_all_stopped(void)
{
	size_t i;

	EnterCriticalSection(&registry);
	for (i = 0; i < T150_SLOT_MAX; i++) {
		struct effect_obj *e = live[i];

		if (e == NULL)
			continue;
		e->playing = 0;
		/* A reset or a stop-all is not a pause: nothing is owed back. */
		e->paused = 0;
		/*
		 * And no start is left waiting to go out on it. The next
		 * upload would carry one, which is a force arriving on a wheel
		 * the game had just turned off.
		 */
		(void)InterlockedExchange(&e->start_owed, 0);
	}
	LeaveCriticalSection(&registry);
}

/*
 * The same, but remembering what was running so a continue can restore it.
 *
 * DISFFC_PAUSE has no opcode of its own, so the proxy sends a stop-everything
 * and answers DI_OK. That much is honest, because answering DI_OK while the
 * wheel carried on pulling is the worse way to be wrong. What was missing is
 * the other half: DISFFC_CONTINUE sent nothing and restarted nothing, so a
 * game that paused had no force feedback for the rest of its run. SDL's
 * SDL_HapticPause and SDL_HapticUnpause are exactly this pair, and a plain
 * DirectInput game that pauses on its menu behaves the same.
 */
void
t150_effect_all_paused(void)
{
	size_t i;

	EnterCriticalSection(&registry);
	for (i = 0; i < T150_SLOT_MAX; i++) {
		struct effect_obj *e = live[i];

		if (e == NULL)
			continue;
		/*
		 * A start that never reached the wheel is not the continue's
		 * to put back: only what was really playing is. Before the
		 * test below rather than after it, because a start the wheel
		 * never heard is not playing and would otherwise keep its debt
		 * across the pause and have the next upload start a force on a
		 * wheel the game has just quietened.
		 */
		(void)InterlockedExchange(&e->start_owed, 0);
		if (!e->playing)
			continue;
		/*
		 * Only from playing to paused, never the other way. Reading
		 * the flag it had just cleared is what a second pause did, and
		 * DirectInput puts no restriction on sending one: a game that
		 * loses focus and sends DISFFC_PAUSE and then
		 * DISFFC_SETACTUATORSOFF, which this file handles as one
		 * branch, went through here twice and the second pass threw
		 * away everything the continue was meant to restore.
		 */
		e->paused = 1;
		e->playing = 0;
	}
	LeaveCriticalSection(&registry);
}

/*
 * Tell the daemon to play this slot, and keep this object's idea of what is
 * playing in step with the answer.
 *
 * Four places built this payload and each decided for itself what a refusal
 * meant, and they did not agree. The reconnect replay in upload() threw the
 * answer away altogether and left the object certain it was playing, which is
 * the one thing a refusal proves it is not.
 *
 * The daemon either refused the slot or never heard the frame, and
 * t150_client_call does not distinguish the two. Both mean the wheel is not
 * rendering it, so playing goes back to zero and what upload() believes the
 * daemon holds is no longer safe to skip against.
 *
 * The count comes off the object rather than from a parameter, because the one
 * caller that has it from the game records it before anything can fail and the
 * other two are saying a start again on the game's behalf.
 */
static int
send_start(struct effect_obj *e)
{
	uint8_t start[2];

	if (e->iterations == 0)
		e->iterations = 1;
	start[0] = (uint8_t)e->slot;
	start[1] = e->iterations;

	if (t150_client_call(T150_OP_EFFECT_START, start, 2) != 0) {
		e->playing = 0;
		e->sent_valid = 0;
		return -1;
	}

	e->playing = 1;
	/*
	 * The clock goes with it. The wheel plays a resumed effect from its
	 * beginning, which is what the daemon's own do_start does to the slot,
	 * so a finite effect's window has to be measured from here. Measured
	 * from the original start, GetEffectStatus declared a resumed effect
	 * finished the moment it came back.
	 */
	e->started_ms = GetTickCount64();

	return 0;
}

void
t150_effect_all_continued(void)
{
	struct effect_obj *seen[T150_SLOT_MAX];
	size_t n, i;

	n = registry_snapshot(seen, NULL);

	for (i = 0; i < n; i++) {
		struct effect_obj *e = seen[i];

		if (!e->paused) {
			IDirectInputEffect_Release(&e->iface);
			continue;
		}
		e->paused = 0;

		/*
		 * The effects were left downloaded, which is the whole
		 * difference between a pause and a reset, so a start is all
		 * this takes. A refusal means the daemon does not hold the
		 * slot after all, and clearing sent_valid is what lets the
		 * game's own Download go rather than being skipped. Nothing
		 * else would say this start again, because the game sent one
		 * continue and has no reason to send another, so it is left
		 * for the next upload to carry.
		 */
		if (send_start(e) != 0)
			(void)InterlockedExchange(&e->start_owed, 1);

		IDirectInputEffect_Release(&e->iface);
	}
}

/*
 * How many times everything this process holds has been released from the
 * daemon. An effect records which of those it uploaded under, so an upload
 * that is skipped can only be skipped against the same one.
 *
 * A counter rather than a sweep over the live effects, which is what this was.
 * A sweep clears a flag that the effect's own upload sets a moment later, and
 * the two are on different threads: a reset arriving between another thread's
 * EFFECT_UPLOAD returning and its bookkeeping was lost outright, leaving that
 * effect certain the daemon held a slot the reset had just released. Nothing
 * then re-uploaded it, a start was refused, and the game's own recovery, which
 * is to call Download again, was answered with a skip and DI_OK. The counter
 * closes that: the value is read before the call and recorded after it, so a
 * reset that lands in between leaves the record behind the counter and the
 * next upload really goes.
 */
static volatile LONG unload_gen;

/*
 * How long an effect may be assumed to still be on the daemon. Two separate
 * things bound this and the smaller wins.
 *
 * The daemon's watchdog clears its slots with the connection still up, after
 * T150_WATCHDOG_MS of silence from this process, so any bound under that
 * proves no safe state has happened since the acknowledgement. A tenth of it
 * leaves the margin between two processes' clocks irrelevant.
 *
 * A write the wheel refuses is the tighter one. The daemon has no frame to
 * answer when an emission pass fails, so it holds the error for the next
 * EFFECT_UPLOAD and reports it on nothing else, deliberately: an error
 * answered to a keepalive would make this proxy drop its connection. Skipping
 * uploads therefore delays the only news of a failed write there is, and this
 * is how long that delay can be. Fifty milliseconds is three frames of a 60 Hz
 * game, which is the slowest this is meant to serve, and still removes fifteen
 * of every sixteen redundant uploads at Assetto Corsa's 333 Hz.
 */
#define ASSUME_MS	50u

/*
 * A device level reset releases every slot without naming one, so nothing
 * else can tell an effect object that the copy the daemon held no longer
 * exists. Only a reset: DISFFC_STOPALL and a pause stop what is playing and
 * leave it downloaded, which is a different thing and not this.
 *
 * Every effect rather than one device's, for the reason t150_effect_all_stopped
 * gives above: the slots are per connection and there is one connection per
 * process.
 */
void
t150_effect_all_unloaded(void)
{
	(void)InterlockedIncrement(&unload_gen);
}

uint8_t
t150_kind_from_guid(REFGUID guid)
{
	if (IsEqualGUID(guid, &GUID_ConstantForce))
		return T150_EFFECT_CONSTANT;
	if (IsEqualGUID(guid, &GUID_RampForce))
		return T150_EFFECT_RAMP;
	if (IsEqualGUID(guid, &GUID_Square))
		return T150_EFFECT_SQUARE;
	if (IsEqualGUID(guid, &GUID_Sine))
		return T150_EFFECT_SINE;
	if (IsEqualGUID(guid, &GUID_Triangle))
		return T150_EFFECT_TRIANGLE;
	if (IsEqualGUID(guid, &GUID_SawtoothUp))
		return T150_EFFECT_SAWTOOTH_UP;
	if (IsEqualGUID(guid, &GUID_SawtoothDown))
		return T150_EFFECT_SAWTOOTH_DOWN;
	if (IsEqualGUID(guid, &GUID_Spring))
		return T150_EFFECT_SPRING;
	if (IsEqualGUID(guid, &GUID_Damper))
		return T150_EFFECT_DAMPER;
	if (IsEqualGUID(guid, &GUID_Friction))
		return T150_EFFECT_FRICTION;
	if (IsEqualGUID(guid, &GUID_Inertia))
		return T150_EFFECT_INERTIA;

	return T150_EFFECT_NONE;
}

static uint32_t
wrap_angle(LONG a)
{
	return (uint32_t)(((a % 36000) + 36000) % 36000);
}

/*
 * A direction in hundredths of a degree, north being zero, which is what the
 * daemon's encoder projects onto the wheel's single axis.
 *
 * Only the cartesian form is degenerate on one axis: its array holds one
 * component, so the side lives in the sign and there is no angle to read.
 * Taking that component literally would hand the encoder a northward
 * direction, which projects onto no sideways force at all, and the game
 * would feel nothing while everything reported success.
 *
 * Polar and spherical are not degenerate. They carry an absolute angle that
 * means the same thing however many axes the effect names, and discarding it
 * on a one-axis effect is how every force ends up on the same side. That was
 * hidden while nothing marked an axis as an actuator, because SDL then sent
 * no direction at all; the moment one is marked SDL sends one axis and a
 * real polar angle, which is the shape this now reads. RESEARCH.md B13.
 */
static uint32_t
direction_of(const DIEFFECT *p)
{
	double angle;
	LONG x, y;

	if (p->cAxes == 0 || p->rglDirection == NULL)
		return 9000;

	if (p->dwFlags & DIEFF_POLAR)
		return wrap_angle(p->rglDirection[0]);

	if (p->dwFlags & DIEFF_SPHERICAL)
		return wrap_angle(p->rglDirection[0] + 9000);

	if (p->cAxes == 1)
		return p->rglDirection[0] < 0 ? 27000 : 9000;

	/*
	 * Cartesian. DirectInput puts X to the right and Y towards the
	 * player, while a polar zero points away from them, so the angle is
	 * measured from the negative Y axis.
	 */
	x = p->rglDirection[0];
	y = p->rglDirection[1];
	if (x == 0 && y == 0)
		return 9000;

	angle = atan2((double)x, -(double)y) * 18000.0 / 3.14159265358979323846;
	if (angle < 0)
		angle += 36000.0;

	return (uint32_t)angle % 36000;
}

/*
 * A stored direction back out in the system a caller asked for, which is the
 * inverse of what direction_of reads.
 *
 * Exposed for the reason t150_effect_convert is: between them they are the
 * fiddliest code in the proxy and the only part of it that can be checked
 * without a wheel. Writes only what the named system needs, and never more
 * than naxes.
 */
void
t150_direction_out(uint32_t direction, DWORD system, LONG *out, DWORD naxes)
{
	if (out == NULL || naxes < 1)
		return;

	if (system == DIEFF_CARTESIAN) {
		double a;

		/* One axis carries no angle at all, only the side. */
		if (naxes == 1) {
			out[0] = direction >= 18000 ? -10000 : 10000;
			return;
		}
		/* The inverse of direction_of's atan2(x, -y). */
		a = (double)direction * 3.14159265358979323846 / 18000.0;
		out[0] = (LONG)(sin(a) * 10000.0);
		out[1] = (LONG)(-cos(a) * 10000.0);
		return;
	}

	if (system == DIEFF_SPHERICAL) {
		/* Spherical zero is due east where polar zero is north. */
		out[0] = (LONG)((direction + 36000 - 9000) % 36000);
		return;
	}

	out[0] = (LONG)direction;
}

void
t150_effect_convert(struct t150_effect *ef, const DIEFFECT *p, DWORD flags)
{
	if (p == NULL)
		return;

	if (flags & DIEP_DURATION)
		ef->duration = p->dwDuration;
	/*
	 * dwStartDelay is the one member DirectInput 6 added to DIEFFECT, so
	 * a caller built against the older headers passes a struct that ends
	 * before it. dwSize is how it says which it has, and reading the
	 * member without checking reads past the end of the caller's object.
	 */
	if ((flags & DIEP_STARTDELAY) && p->dwSize >= sizeof(DIEFFECT))
		ef->start_delay = p->dwStartDelay;
	if (flags & DIEP_GAIN)
		ef->gain = p->dwGain;
	if (flags & DIEP_DIRECTION)
		ef->direction = direction_of(p);

	if (flags & DIEP_ENVELOPE) {
		if (p->lpEnvelope != NULL) {
			ef->envelope.present = 1;
			ef->envelope.attack_time = p->lpEnvelope->dwAttackTime;
			ef->envelope.attack_level =
			    (int32_t)p->lpEnvelope->dwAttackLevel;
			ef->envelope.fade_time = p->lpEnvelope->dwFadeTime;
			ef->envelope.fade_level =
			    (int32_t)p->lpEnvelope->dwFadeLevel;
		} else {
			memset(&ef->envelope, 0, sizeof(ef->envelope));
		}
	}

	if ((flags & DIEP_TYPESPECIFICPARAMS) == 0 ||
	    p->lpvTypeSpecificParams == NULL)
		return;

	switch (ef->kind) {
	case T150_EFFECT_CONSTANT:
		if (p->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) {
			const DICONSTANTFORCE *c = p->lpvTypeSpecificParams;

			ef->u.constant.magnitude = c->lMagnitude;
		}
		break;
	case T150_EFFECT_RAMP:
		if (p->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) {
			const DIRAMPFORCE *r = p->lpvTypeSpecificParams;

			ef->u.ramp.start = r->lStart;
			ef->u.ramp.end = r->lEnd;
		}
		break;
	case T150_EFFECT_SQUARE:
	case T150_EFFECT_SINE:
	case T150_EFFECT_TRIANGLE:
	case T150_EFFECT_SAWTOOTH_UP:
	case T150_EFFECT_SAWTOOTH_DOWN:
		if (p->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
			const DIPERIODIC *q = p->lpvTypeSpecificParams;

			ef->u.periodic.magnitude = (int32_t)q->dwMagnitude;
			ef->u.periodic.offset = q->lOffset;
			ef->u.periodic.phase = q->dwPhase;
			ef->u.periodic.period = q->dwPeriod;
		}
		break;
	case T150_EFFECT_SPRING:
	case T150_EFFECT_DAMPER:
	case T150_EFFECT_FRICTION:
	case T150_EFFECT_INERTIA:
		if (p->cbTypeSpecificParams >= sizeof(DICONDITION)) {
			const DICONDITION *c = p->lpvTypeSpecificParams;

			ef->u.condition.center = c->lOffset;
			ef->u.condition.pos_coeff = c->lPositiveCoefficient;
			ef->u.condition.neg_coeff = c->lNegativeCoefficient;
			ef->u.condition.pos_saturation =
			    (int32_t)c->dwPositiveSaturation;
			ef->u.condition.neg_saturation =
			    (int32_t)c->dwNegativeSaturation;
			ef->u.condition.deadband = c->lDeadBand;
		}
		break;
	default:
		break;
	}
}

/*
 * Send the effect, and put it back together if the daemon changed underneath.
 *
 * A restarted t150d is a new daemon with an empty slot table, and the game
 * has no idea: from its side nothing failed, so it never re-creates or
 * re-starts anything. The proxy is the only thing that knows both what the
 * game asked for and that the connection is new, so it says it again on the
 * first call after a reconnect. A game that keeps updating a force, which is
 * every game, gets its force feedback back within a frame.
 *
 * The start goes with it, because a slot the daemon has never heard of is not
 * playing however sure the game is that it is.
 */
static int
upload(struct effect_obj *e)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];
	unsigned int gen;
	LONG ugen, owed;
	int up;

	if (t150_proto_pack_effect(buf, sizeof(buf), &e->ef) == 0)
		return -1;

	/*
	 * The counter is read twice on purpose and the two readings answer
	 * different questions. This one is what the upload below is stamped
	 * with, and it has to be taken before the call so that a reset landing
	 * during it leaves the stamp behind the counter. The skip test takes
	 * its own, as late as it can, because deciding on this one would be
	 * deciding on a value read before a wait: t150_client_state takes the
	 * same lock that a reset's own round trip holds for its whole length,
	 * so a thread that arrives here just before another calls Unacquire
	 * can sit in that lock for the duration of the reset and come out
	 * still believing what it read on the way in.
	 */
	ugen = unload_gen;

	/*
	 * A game that re-uploads an effect it has not changed pays a round
	 * trip on the thread it draws from, and the daemon answers by
	 * comparing the same bytes and sending the wheel nothing. Compare
	 * them here instead. Every Start does an upload first, so a game that
	 * starts an effect as often as it plays a frame pays two round trips
	 * per frame for one of them to do anything.
	 *
	 * The packed form rather than the struct, which is the discipline
	 * flush_slot uses in the daemon and for the same reason: the struct
	 * carries padding and the inactive tail of a union, neither of which
	 * is a difference the wheel could tell.
	 *
	 * Skipping is only sound while the daemon's slot still holds what this
	 * object last sent and owes it nothing, and there are five ways that
	 * stops being true:
	 *
	 * - The connection went. t150_client_call is what reconnects, so
	 *   skipping it while the socket is down would leave a game that keeps
	 *   sending the same force with no force feedback and nothing to
	 *   restore it. Hence the online test, which is also what makes the
	 *   reconnect reachable.
	 * - The daemon was restarted. That is the generation, and the block
	 *   below already exists to say the effect again over a new one.
	 * - A start the game asked for has not reached the daemon. That is the
	 *   debt below, which this function is the only thing that pays, so an
	 *   upload carrying one must not be skipped. Every path that leaves a
	 *   debt behind has just had a call refused and so has cleared the
	 *   acknowledgement as well, but one load here is worth more than an
	 *   argument that has to be re-derived every time this list is read.
	 * - The game reset the device, which releases every slot without
	 *   naming one. That is the unload counter above, which
	 *   t150_effect_all_unloaded moves.
	 * - The daemon's watchdog fired and made the wheel safe, which clears
	 *   its slots with the connection still up, or an emission pass failed
	 *   and the daemon is holding the news for the next upload. Both are
	 *   bounded by how old the acknowledgement is: see ASSUME_MS.
	 *
	 * A ramp is never skipped. The wheel has no ramp, so the daemon
	 * renders one as a constant it re-computes on its own clock, and for a
	 * ramp the bytes being equal therefore does not mean the daemon's slot
	 * is. Skipping the upload left a ramp that was started again playing
	 * whatever level it had slid to: measured against the session code, a
	 * ramp from 0 to 10000 over 300 ms restarted after it had run sent the
	 * play packet alone, so the wheel replayed the 0x40 it was still
	 * holding, which is full scale, until the slicer corrected it up to
	 * T150_RAMP_TICK_MS later.
	 *
	 * The daemon no longer relies on this: do_start rewinds a ramp itself,
	 * so a skip cannot produce that any more. The exemption stays because
	 * the two halves of this stack ship separately and are updated
	 * separately, so a proxy this new may well be talking to a daemon that
	 * still needs the upload, and one round trip on an effect kind almost
	 * nothing uses is not worth the risk of finding out. Ramps are the
	 * only kind it applies to: session.c's tick touches no other slot's
	 * effect.
	 */
	gen = t150_client_state(&up);
	if (up && e->sent_valid && !e->start_owed && e->gen == gen &&
	    e->sent_unload_gen == unload_gen &&
	    e->ef.kind != T150_EFFECT_RAMP &&
	    GetTickCount64() - e->sent_ms < ASSUME_MS &&
	    memcmp(e->sent, buf, sizeof(buf)) == 0)
		return 0;

	if (t150_client_call(T150_OP_EFFECT_UPLOAD, buf, sizeof(buf)) != 0) {
		/* What the daemon has is now anyone's guess. */
		e->sent_valid = 0;
		return -1;
	}
	memcpy(e->sent, buf, sizeof(buf));
	e->sent_ms = GetTickCount64();
	e->sent_unload_gen = ugen;
	e->sent_valid = 1;

	gen = t150_client_state(NULL);
	if (e->gen != gen) {
		e->gen = gen;
		/*
		 * The device settings first: they scale everything the wheel
		 * renders, so re-uploading an effect to a daemon that has
		 * forgotten the gain would put it back at full strength.
		 */
		t150_device_replay_props(e->dev);
		if (e->playing) {
			t150_log("the daemon is a new one, starting slot %d "
			    "again\n", e->slot);
			/*
			 * Owed rather than sent from here, so it goes out
			 * through the one place that starts a slot and the one
			 * place that is right about what a refusal means. This
			 * built its own payload and threw the answer away, so
			 * a start a new daemon had refused was reported to the
			 * game as a running effect, and every later Download
			 * was skipped against a slot that held nothing.
			 */
			(void)InterlockedExchange(&e->start_owed, 1);
		}
	}

	/*
	 * Whatever start is owed, now that the daemon has the parameters it
	 * would play. Claimed with an exchange rather than tested and cleared,
	 * so two threads uploading at once cannot both send it and so a debt
	 * the reconnect above has just raised is folded into one already
	 * outstanding rather than sent twice.
	 *
	 * Owed again if it fails, because for two of the three debts this is
	 * the only thing that will ever say the start. start_effect's has the
	 * game behind it, which was told DIERR_NOTDOWNLOADED and may ask
	 * again; the reconnect replay above and the continue were both
	 * answered DI_OK and have no second asker, and the replay cannot raise
	 * it a second time because send_start has just cleared the playing
	 * flag it tests. Cleared on the way out and put back, so a Stop
	 * landing during the call still takes it.
	 *
	 * Nothing spins against a dead socket: an upload that cannot reach the
	 * daemon has already returned -1 above, and start_effect clears any
	 * older debt before its own upload.
	 *
	 * Dropping the answer here is not the mistake the replay above made
	 * with it. send_start has already recorded what a refusal means, and a
	 * start cannot change the one thing this function answers, which is
	 * whether the daemon has these parameters: returning -1 for it would
	 * answer DIERR_NOTDOWNLOADED to a Download that really did download,
	 * and the game's answer to that is to call Download again.
	 */
	owed = InterlockedExchange(&e->start_owed, 0);
	if (owed != 0 && send_start(e) != 0)
		(void)InterlockedExchange(&e->start_owed, owed);

	return 0;
}

static HRESULT WINAPI
eff_QueryInterface(IDirectInputEffect *self, REFIID iid, void **out)
{
	if (out == NULL)
		return E_POINTER;
	if (IsEqualGUID(iid, &IID_IUnknown) ||
	    IsEqualGUID(iid, &IID_IDirectInputEffect)) {
		IDirectInputEffect_AddRef(self);
		*out = self;
		return S_OK;
	}
	*out = NULL;

	return E_NOINTERFACE;
}

static ULONG WINAPI
eff_AddRef(IDirectInputEffect *self)
{
	return (ULONG)InterlockedIncrement(&from_iface(self)->refs);
}

static ULONG WINAPI
eff_Release(IDirectInputEffect *self)
{
	struct effect_obj *e = from_iface(self);
	LONG r = release_and_forget(e);

	if (r == 0) {
		uint8_t slot = (uint8_t)e->slot;

		(void)t150_client_call(T150_OP_EFFECT_DESTROY, &slot, 1);
		t150_slot_free(e->slot);
		/* Balances the reference taken in t150_effect_create. */
		IDirectInputDevice8_Release((IDirectInputDevice8W *)e->dev);
		free(e);
	}

	return (ULONG)r;
}

static HRESULT WINAPI
eff_Initialize(IDirectInputEffect *self, HINSTANCE inst, DWORD ver, REFGUID guid)
{
	(void)self;
	(void)inst;
	(void)ver;
	(void)guid;

	return DI_OK;
}

static HRESULT WINAPI
eff_GetEffectGuid(IDirectInputEffect *self, GUID *out)
{
	if (out == NULL)
		return E_POINTER;
	*out = from_iface(self)->guid;

	return DI_OK;
}

static HRESULT WINAPI
eff_GetParameters(IDirectInputEffect *self, DIEFFECT *p, DWORD flags)
{
	struct effect_obj *e = from_iface(self);

	if (p == NULL)
		return E_POINTER;

	if (flags & DIEP_DURATION)
		p->dwDuration = e->ef.duration;
	/*
	 * dwStartDelay is the one field past the end of a DIEFFECT_DX5, which
	 * is what the guard on the way in at DIEP_STARTDELAY above is for.
	 * The way out needs the same guard, and did not have it.
	 */
	if ((flags & DIEP_STARTDELAY) && p->dwSize >= sizeof(DIEFFECT))
		p->dwStartDelay = e->ef.start_delay;
	if (flags & DIEP_GAIN)
		p->dwGain = e->ef.gain;
	if (flags & DIEP_SAMPLEPERIOD)
		p->dwSamplePeriod = 0;
	if (flags & DIEP_TRIGGERBUTTON) {
		p->dwTriggerButton = DIEB_NOTRIGGER;
		p->dwTriggerRepeatInterval = 0;
	}

	/*
	 * Direction goes back in the system the caller asked for.
	 *
	 * DirectInput's contract is that the caller names one in dwFlags and
	 * the device converts; Wine implements exactly that. This answered in
	 * polar whatever was asked and rewrote the caller's flags to say so,
	 * which breaks the read-modify-write that exists so the members a
	 * caller does not touch survive: a game that fills a DIEFFECT with
	 * DIEFF_CARTESIAN, reads every parameter, changes only the magnitude
	 * and writes it back handed direction_of a polar angle in a struct
	 * still flagged cartesian. On one axis that is read as a sign, so a
	 * force pointing left came back as a large positive number and the
	 * wheel pushed right.
	 *
	 * Polar when the caller named nothing, which is what this always did.
	 * Wine refuses that with DIERR_INVALIDPARAM; keeping it is the more
	 * forgiving of the two and costs nobody anything.
	 */
	if ((flags & DIEP_DIRECTION) && p->rglDirection != NULL &&
	    p->cAxes >= 1) {
		DWORD want = p->dwFlags &
		    (DIEFF_CARTESIAN | DIEFF_POLAR | DIEFF_SPHERICAL);

		if (want != DIEFF_CARTESIAN && want != DIEFF_SPHERICAL) {
			p->dwFlags = (p->dwFlags & ~(DWORD)(DIEFF_CARTESIAN |
			    DIEFF_SPHERICAL)) | DIEFF_POLAR;
			want = DIEFF_POLAR;
		}
		t150_direction_out(e->ef.direction, want, p->rglDirection,
		    p->cAxes);
	}

	if ((flags & DIEP_ENVELOPE) && p->lpEnvelope != NULL &&
	    p->lpEnvelope->dwSize >= sizeof(DIENVELOPE)) {
		if (e->ef.envelope.present) {
			p->lpEnvelope->dwAttackLevel =
			    (DWORD)e->ef.envelope.attack_level;
			p->lpEnvelope->dwAttackTime = e->ef.envelope.attack_time;
			p->lpEnvelope->dwFadeLevel =
			    (DWORD)e->ef.envelope.fade_level;
			p->lpEnvelope->dwFadeTime = e->ef.envelope.fade_time;
		} else {
			p->lpEnvelope = NULL;	/* the effect has none */
		}
	}

	if ((flags & DIEP_TYPESPECIFICPARAMS) && p->lpvTypeSpecificParams != NULL) {
		switch (e->ef.kind) {
		case T150_EFFECT_CONSTANT:
			if (p->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) {
				DICONSTANTFORCE *c = p->lpvTypeSpecificParams;

				c->lMagnitude = e->ef.u.constant.magnitude;
			}
			break;
		case T150_EFFECT_RAMP:
			if (p->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) {
				DIRAMPFORCE *r = p->lpvTypeSpecificParams;

				r->lStart = e->ef.u.ramp.start;
				r->lEnd = e->ef.u.ramp.end;
			}
			break;
		case T150_EFFECT_SQUARE:
		case T150_EFFECT_SINE:
		case T150_EFFECT_TRIANGLE:
		case T150_EFFECT_SAWTOOTH_UP:
		case T150_EFFECT_SAWTOOTH_DOWN:
			if (p->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
				DIPERIODIC *pe = p->lpvTypeSpecificParams;

				pe->dwMagnitude =
				    (DWORD)e->ef.u.periodic.magnitude;
				pe->lOffset = e->ef.u.periodic.offset;
				pe->dwPhase = e->ef.u.periodic.phase;
				pe->dwPeriod = e->ef.u.periodic.period;
			}
			break;
		case T150_EFFECT_SPRING:
		case T150_EFFECT_DAMPER:
		case T150_EFFECT_FRICTION:
		case T150_EFFECT_INERTIA:
			if (p->cbTypeSpecificParams >= sizeof(DICONDITION)) {
				DICONDITION *c = p->lpvTypeSpecificParams;

				c->lOffset = e->ef.u.condition.center;
				c->lPositiveCoefficient =
				    e->ef.u.condition.pos_coeff;
				c->lNegativeCoefficient =
				    e->ef.u.condition.neg_coeff;
				c->dwPositiveSaturation =
				    (DWORD)e->ef.u.condition.pos_saturation;
				c->dwNegativeSaturation =
				    (DWORD)e->ef.u.condition.neg_saturation;
				c->lDeadBand = e->ef.u.condition.deadband;
				p->cbTypeSpecificParams = sizeof(DICONDITION);
			}
			break;
		default:
			break;
		}
	}

	return DI_OK;
}

/*
 * What a failed call to the daemon means to the game.
 *
 * Not that the device was lost, which is what this used to say at every one
 * of these sites. DIERR_INPUTLOST tells a game to call Acquire and try again,
 * and Acquire here forwards to a Wine device that never went anywhere and
 * cheerfully succeeds, so the documented recovery repairs nothing and the
 * game is invited to loop or to give up. eff_Stop worked this out first and
 * the comment there has said so for several releases; this is the rest of it.
 *
 * DIERR_NOTDOWNLOADED is the truth: the wheel does not have this effect. A
 * game that cares calls Download again, which is exactly the right thing to
 * do, and which succeeds the moment the wheel is back. Test 34 is why it
 * matters: a wheel unplugged mid race now recovers by itself, and force
 * feedback still did not come back, because the game had been told its input
 * was lost during the seconds the wheel was away and stopped asking.
 */
/*
 * A start the game asked for, wherever it asked from.
 *
 * Both doors lead here. SetParameters with DIEP_START is a start of exactly
 * one pass and nothing else, which is how Wine reads it as well: for that flag
 * dlls/dinput/joystick_hid.c calls Start(iface, 1, 0) rather than repeating
 * the work. eff_SetParameters had a copy of this of its own, and the copy was
 * the one that went wrong.
 *
 * Start downloads the effect first unless the caller says not to, which is the
 * whole purpose of DIES_NODOWNLOAD. Skipping it meant that anything releasing
 * the daemon's slots, an Unacquire or a
 * SendForceFeedbackCommand(DISFFC_RESET), left the game's effect objects
 * pointing at slots that no longer existed. Every later Start then failed and
 * the game had no way to know why.
 *
 * Logged once per effect rather than on every call. A game may Start an effect
 * as often as it updates it, and the log opens its file per line, so logging
 * each one would cost more than the diagnosis is worth. What matters is
 * whether a Start ever happens at all. Either door produces the line now, so a
 * game that only ever starts through DIEP_START is no longer silent here.
 */
static HRESULT
start_effect(struct effect_obj *e, DWORD iterations, DWORD flags)
{
	/*
	 * What the game asked for goes down before anything can fail, because
	 * the count is the one thing a later start has no other way to learn:
	 * a rumble asked to repeat twenty times came back as one pass of it
	 * once already, one step further along.
	 *
	 * Any older debt is settled by this call rather than added to, since
	 * the game is asking now. Without that, upload() below pays the debt
	 * and this function sends the same play packet again immediately
	 * after it, which against a daemon refusing starts is two identical
	 * frames for every frame the game draws.
	 */
	e->iterations = iterations >= 255 ? 255 : (uint8_t)iterations;
	(void)InterlockedExchange(&e->start_owed, 0);

	if (!(flags & DIES_NODOWNLOAD) && upload(e) != 0) {
		/*
		 * The start goes down with the upload it rides on, and the
		 * game is told so. Owed rather than dropped, because nothing
		 * else would ever say it again and a game has no reason to ask
		 * twice for something it was told had failed.
		 */
		(void)InterlockedExchange(&e->start_owed, 1);
		if (!e->logged) {
			e->logged = 1;
			t150_log("Start: upload failed, slot %d\n", e->slot);
		}
		return DIERR_NOTDOWNLOADED;
	}
	if (!e->logged) {
		e->logged = 1;
		t150_log("Start slot %d, %lu iteration(s)\n", e->slot,
		    (unsigned long)iterations);
	}

	/*
	 * A refused start is the daemon saying it does not hold this slot, so
	 * whatever upload() believes it has is wrong. send_start clears that,
	 * which is what makes the answer below mean anything:
	 * DIERR_NOTDOWNLOADED invites the game to call Download, and Download
	 * is upload(), which would otherwise skip and report DI_OK without
	 * having said a word. The start stays owed on top of that, for the
	 * game that does not take the invitation.
	 */
	if (send_start(e) != 0) {
		(void)InterlockedExchange(&e->start_owed, 1);
		return DIERR_NOTDOWNLOADED;
	}

	return DI_OK;
}

static HRESULT WINAPI
eff_SetParameters(IDirectInputEffect *self, const DIEFFECT *p, DWORD flags)
{
	struct effect_obj *e = from_iface(self);

	t150_effect_convert(&e->ef, p, flags);

	/*
	 * One of the three, which is what DirectInput defines and what Wine
	 * does: DIEP_NODOWNLOAD says send nothing, DIEP_START says start, and
	 * a start downloads on its way. Downloading here and starting
	 * separately afterwards is how the start came to be thrown away
	 * whenever the download failed, which is a force lost for the rest of
	 * the run on one refused upload, and it also sent a bare start for
	 * parameters the daemon was never given when DIEP_NODOWNLOAD was set
	 * as well.
	 *
	 * The flags are deliberately not handed on. DIEP_NODOWNLOAD and
	 * DIES_NODOWNLOAD are the same bit, so passing them through would send
	 * that bare start again by another route.
	 */
	if (flags & DIEP_NODOWNLOAD)
		return DI_OK;
	if (flags & DIEP_START)
		return start_effect(e, 1, 0);

	return upload(e) == 0 ? DI_OK : DIERR_NOTDOWNLOADED;
}

static HRESULT WINAPI
eff_Start(IDirectInputEffect *self, DWORD iterations, DWORD flags)
{
	return start_effect(from_iface(self), iterations, flags);
}

static HRESULT WINAPI
eff_Stop(IDirectInputEffect *self)
{
	struct effect_obj *e = from_iface(self);
	uint8_t slot = (uint8_t)e->slot;

	/*
	 * A start that never reached the wheel is dropped here, before the
	 * call so that it holds whichever answer comes back. A game that stops
	 * an effect the wheel never heard the start for has changed its mind,
	 * and the next upload would otherwise carry it.
	 */
	(void)InterlockedExchange(&e->start_owed, 0);

	if (t150_client_call(T150_OP_EFFECT_STOP, &slot, 1) != 0) {
		/*
		 * The slot may simply not be downloaded, which is what the
		 * daemon says when a reset cleared it. DIERR_NOTDOWNLOADED
		 * describes that; DIERR_INPUTLOST claims the device went
		 * away and sends the game into a reacquire loop it cannot
		 * win. Either way the effect is not running.
		 *
		 * And whatever upload() believes the daemon holds is no longer
		 * safe to skip against, for the same reason a refused start is
		 * not: this is the daemon saying it does not have the slot.
		 */
		e->playing = 0;
		e->paused = 0;
		e->sent_valid = 0;
		return DIERR_NOTDOWNLOADED;
	}
	e->playing = 0;
	/*
	 * And it is not owed back by a continue. The pause records what was
	 * running so DISFFC_CONTINUE can restore it, and a game is free to
	 * stop an effect while paused - on a menu, which is exactly when a
	 * game is paused. Leaving the record standing had the continue start
	 * a force the player had just turned off.
	 */
	e->paused = 0;

	return DI_OK;
}

static HRESULT WINAPI
eff_GetEffectStatus(IDirectInputEffect *self, DWORD *out)
{
	struct effect_obj *e = from_iface(self);

	if (out == NULL)
		return E_POINTER;

	/*
	 * An effect with a length of its own ends when that runs out, and
	 * nothing on the wire says so: the wheel is given the length and stops
	 * by itself, the daemon knows, and the protocol has no way to tell the
	 * proxy. So the same arithmetic is done here, from the Start this
	 * object made and the duration it holds.
	 *
	 * Without it the flag was set by Start and cleared only by Stop, an
	 * Unload or a device level command, so a finite effect said
	 * DIEGES_PLAYING for the rest of the process. That breaks the standard
	 * idiom exactly: a game that fires a kerb effect with "if it is not
	 * playing, start it" fires it once and is silent from then on.
	 */
	/*
	 * Everything the wheel was told, the way the daemon's slot_expired
	 * counts it: the commit carries a start delay and the play packet an
	 * iteration count, so the window begins at started_ms plus the delay
	 * and lasts that many durations. Counting the duration alone declared
	 * a delayed effect finished before the wheel had begun it, and a
	 * repeating one finished after its first pass - and the idiom this
	 * exists to serve, "if it is not playing, start it", would then have
	 * restarted it from the top on the very next frame.
	 */
	if (e->playing && e->ef.duration != T150_DURATION_INFINITE &&
	    e->ef.duration != 0) {
		ULONGLONG span = (ULONGLONG)(e->ef.start_delay / 1000) +
		    (ULONGLONG)(e->iterations > 0 ? e->iterations : 1) *
		    (e->ef.duration / 1000);

		if (GetTickCount64() - e->started_ms >= span)
			e->playing = 0;
	}

	*out = e->playing ? DIEGES_PLAYING : 0;

	return DI_OK;
}

static HRESULT WINAPI
eff_Download(IDirectInputEffect *self)
{
	return upload(from_iface(self)) == 0 ? DI_OK : DIERR_NOTDOWNLOADED;
}

static HRESULT WINAPI
eff_Unload(IDirectInputEffect *self)
{
	struct effect_obj *e = from_iface(self);
	uint8_t slot = (uint8_t)e->slot;

	(void)t150_client_call(T150_OP_EFFECT_DESTROY, &slot, 1);
	e->playing = 0;
	e->paused = 0;		/* nor after the game has unloaded it */
	/* And no start is left waiting for a slot being dropped. */
	(void)InterlockedExchange(&e->start_owed, 0);
	/* The daemon has no copy to compare the next upload against. */
	e->sent_valid = 0;

	return DI_OK;
}

static HRESULT WINAPI
eff_Escape(IDirectInputEffect *self, DIEFFESCAPE *esc)
{
	(void)self;
	(void)esc;

	return DIERR_UNSUPPORTED;
}

static const IDirectInputEffectVtbl effect_vtbl = {
	eff_QueryInterface,
	eff_AddRef,
	eff_Release,
	eff_Initialize,
	eff_GetEffectGuid,
	eff_GetParameters,
	eff_SetParameters,
	eff_Start,
	eff_Stop,
	eff_GetEffectStatus,
	eff_Download,
	eff_Unload,
	eff_Escape
};

HRESULT
t150_effect_create(struct t150_device *dev, REFGUID guid, const DIEFFECT *params,
    IDirectInputEffect **out)
{
	struct effect_obj *e;
	uint8_t kind;
	int slot;

	if (out == NULL)
		return E_POINTER;
	*out = NULL;

	if ((kind = t150_kind_from_guid(guid)) == T150_EFFECT_NONE)
		return DIERR_DEVICENOTREG;
	if ((slot = t150_slot_alloc()) < 0)
		return DIERR_DEVICEFULL;
	if ((e = calloc(1, sizeof(*e))) == NULL) {
		t150_slot_free(slot);
		return E_OUTOFMEMORY;
	}

	e->iface.lpVtbl = (IDirectInputEffectVtbl *)&effect_vtbl;
	e->refs = 1;
	e->dev = dev;
	/*
	 * An effect outliving its device is legal and would otherwise leave
	 * this pointing at freed memory, so hold a reference like any other
	 * COM object would.
	 */
	IDirectInputDevice8_AddRef((IDirectInputDevice8W *)dev);
	e->guid = *guid;
	e->slot = slot;
	e->ef.kind = kind;
	e->ef.slot = (uint8_t)slot;
	e->ef.gain = T150_DI_MAX;
	e->ef.duration = T150_DURATION_INFINITE;
	e->ef.direction = 9000;

	/*
	 * Reachable from the device before anything can be done to it, so a
	 * game that walks its effects mid-creation on another thread sees a
	 * complete object rather than a half filled one.
	 */
	remember(e);

	if (params != NULL) {
		t150_effect_convert(&e->ef, params, params->dwFlags | DIEP_DURATION |
		    DIEP_STARTDELAY | DIEP_GAIN | DIEP_DIRECTION |
		    DIEP_ENVELOPE | DIEP_TYPESPECIFICPARAMS);
		/*
		 * A wheel that is briefly absent must not cost the game its
		 * effect object. The daemon stores the slot either way and
		 * puts it on the wheel when it can, so the object is handed
		 * back and the game may Download or Start it later.
		 */
		(void)upload(e);
	}

	*out = &e->iface;

	return DI_OK;
}
