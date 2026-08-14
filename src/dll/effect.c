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
	int			 logged;	/* the first Start is logged, not every one */
	unsigned int		 gen;		/* the connection this was uploaded to */
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
 * Written with an interlocked exchange for the same reason the slot map is:
 * two game threads may create and release effects at once.
 */
static struct effect_obj *volatile live[T150_SLOT_MAX];

static void
remember(struct effect_obj *e)
{
	if (e->slot >= 0 && e->slot < (int)T150_SLOT_MAX)
		(void)InterlockedExchangePointer((void *volatile *)&live[e->slot],
		    e);
}

static void
forget(struct effect_obj *e)
{
	if (e->slot >= 0 && e->slot < (int)T150_SLOT_MAX)
		(void)InterlockedExchangePointer((void *volatile *)&live[e->slot],
		    NULL);
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
	size_t i;

	for (i = 0; i < T150_SLOT_MAX; i++) {
		struct effect_obj *e = live[i];

		if (e == NULL || e->dev != dev)
			continue;
		if (cb(&e->iface, ref) == DIENUM_STOP)
			break;
	}

	return DI_OK;
}

/*
 * Every effect this device created is stopped, as far as the game is
 * concerned.
 *
 * The device level commands stop everything on the wheel without naming a
 * single effect, so nothing could clear the flag that says an object is
 * playing: an effect stopped by DISFFC_RESET, DISFFC_STOPALL or an unacquire
 * stayed marked as playing, and the replay that follows a reconnect then
 * started it again. A game that had deliberately stopped its forces could be
 * handed a pulling wheel by a daemon restart. It also had GetEffectStatus
 * answer that a stopped effect was still running.
 */
void
t150_effect_all_stopped(struct t150_device *dev)
{
	size_t i;

	for (i = 0; i < T150_SLOT_MAX; i++) {
		struct effect_obj *e = live[i];

		if (e != NULL && e->dev == dev)
			e->playing = 0;
	}
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

	if (t150_proto_pack_effect(buf, sizeof(buf), &e->ef) == 0)
		return -1;
	if (t150_client_call(T150_OP_EFFECT_UPLOAD, buf, sizeof(buf)) != 0)
		return -1;

	gen = t150_client_generation();
	if (e->gen != gen) {
		e->gen = gen;
		if (e->playing) {
			uint8_t start[2] = { (uint8_t)e->slot, 1 };

			t150_log("the daemon is a new one, starting slot %d "
			    "again\n", e->slot);
			(void)t150_client_call(T150_OP_EFFECT_START, start, 2);
		}
	}

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
	LONG r = InterlockedDecrement(&e->refs);

	if (r == 0) {
		uint8_t slot = (uint8_t)e->slot;

		(void)t150_client_call(T150_OP_EFFECT_DESTROY, &slot, 1);
		forget(e);
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
	 * Direction goes back the way it came in, in polar hundredths of a
	 * degree, which is the form direction_of() normalised it to.
	 */
	if ((flags & DIEP_DIRECTION) && p->rglDirection != NULL &&
	    p->cAxes >= 1) {
		p->dwFlags = (p->dwFlags & ~(DWORD)(DIEFF_CARTESIAN |
		    DIEFF_SPHERICAL)) | DIEFF_POLAR;
		p->rglDirection[0] = (LONG)e->ef.direction;
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
static HRESULT WINAPI
eff_SetParameters(IDirectInputEffect *self, const DIEFFECT *p, DWORD flags)
{
	struct effect_obj *e = from_iface(self);

	t150_effect_convert(&e->ef, p, flags);

	if ((flags & DIEP_NODOWNLOAD) == 0 && upload(e) != 0)
		return DIERR_NOTDOWNLOADED;

	if (flags & DIEP_START) {
		uint8_t start[2] = { (uint8_t)e->slot, 1 };

		if (t150_client_call(T150_OP_EFFECT_START, start, 2) != 0)
			return DIERR_NOTDOWNLOADED;
		e->playing = 1;
	}

	return DI_OK;
}

static HRESULT WINAPI
eff_Start(IDirectInputEffect *self, DWORD iterations, DWORD flags)
{
	struct effect_obj *e = from_iface(self);
	uint8_t start[2];

	/*
	 * Start downloads the effect first unless the caller says not to,
	 * which is the whole purpose of DIES_NODOWNLOAD. Skipping it meant
	 * that anything releasing the daemon's slots, an Unacquire or a
	 * SendForceFeedbackCommand(DISFFC_RESET), left the game's effect
	 * objects pointing at slots that no longer existed. Every later
	 * Start then failed and the game had no way to know why.
	 */
	/*
	 * Logged once per effect rather than on every call. A game may Start
	 * an effect as often as it updates it, and the log opens its file per
	 * line, so logging each one would cost more than the diagnosis is
	 * worth. What matters is whether a Start ever happens at all.
	 */
	if (!(flags & DIES_NODOWNLOAD) && upload(e) != 0) {
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

	start[0] = (uint8_t)e->slot;
	start[1] = iterations >= 255 ? 255 : (uint8_t)iterations;
	if (start[1] == 0)
		start[1] = 1;

	if (t150_client_call(T150_OP_EFFECT_START, start, 2) != 0)
		return DIERR_NOTDOWNLOADED;
	e->playing = 1;

	return DI_OK;
}

static HRESULT WINAPI
eff_Stop(IDirectInputEffect *self)
{
	struct effect_obj *e = from_iface(self);
	uint8_t slot = (uint8_t)e->slot;

	if (t150_client_call(T150_OP_EFFECT_STOP, &slot, 1) != 0) {
		/*
		 * The slot may simply not be downloaded, which is what the
		 * daemon says when a reset cleared it. DIERR_NOTDOWNLOADED
		 * describes that; DIERR_INPUTLOST claims the device went
		 * away and sends the game into a reacquire loop it cannot
		 * win. Either way the effect is not running.
		 */
		e->playing = 0;
		return DIERR_NOTDOWNLOADED;
	}
	e->playing = 0;

	return DI_OK;
}

static HRESULT WINAPI
eff_GetEffectStatus(IDirectInputEffect *self, DWORD *out)
{
	if (out == NULL)
		return E_POINTER;
	*out = from_iface(self)->playing ? DIEGES_PLAYING : 0;

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
