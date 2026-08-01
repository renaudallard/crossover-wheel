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
	struct t150_effect	 ef;
};

static struct effect_obj *
from_iface(IDirectInputEffect *p)
{
	return (struct effect_obj *)p;
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

/*
 * A direction in hundredths of a degree, north being zero, which is what the
 * daemon's encoder projects onto the wheel's single axis.
 *
 * A one-axis effect is forced to due east. DirectInput carries the side of a
 * one-axis effect in the sign of the magnitude, and its direction array is
 * degenerate there: taking it literally would hand the encoder a northward
 * direction, which projects onto no sideways force at all, and the game
 * would feel nothing while everything reported success.
 */
static uint32_t
direction_of(const DIEFFECT *p)
{
	double angle;
	LONG x, y;

	if (p->cAxes <= 1 || p->rglDirection == NULL) {
		if (p->cAxes == 1 && p->rglDirection != NULL &&
		    (p->dwFlags & DIEFF_CARTESIAN) && p->rglDirection[0] < 0)
			return 27000;
		return 9000;
	}

	if (p->dwFlags & DIEFF_POLAR)
		return (uint32_t)(((p->rglDirection[0] % 36000) + 36000) % 36000);

	if (p->dwFlags & DIEFF_SPHERICAL) {
		LONG a = p->rglDirection[0] + 9000;

		return (uint32_t)(((a % 36000) + 36000) % 36000);
	}

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
	if (flags & DIEP_STARTDELAY)
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

static int
upload(struct effect_obj *e)
{
	uint8_t buf[T150_PROTO_EFFECT_LEN];

	if (t150_proto_pack_effect(buf, sizeof(buf), &e->ef) == 0)
		return -1;

	return t150_client_call(T150_OP_EFFECT_UPLOAD, buf, sizeof(buf));
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
		t150_device_slot_free(e->dev, e->slot);
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
	if (flags & DIEP_STARTDELAY)
		p->dwStartDelay = e->ef.start_delay;
	if (flags & DIEP_GAIN)
		p->dwGain = e->ef.gain;

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
		default:
			break;
		}
	}

	return DI_OK;
}

static HRESULT WINAPI
eff_SetParameters(IDirectInputEffect *self, const DIEFFECT *p, DWORD flags)
{
	struct effect_obj *e = from_iface(self);

	t150_effect_convert(&e->ef, p, flags);

	if ((flags & DIEP_NODOWNLOAD) == 0 && upload(e) != 0)
		return DIERR_INPUTLOST;

	if (flags & DIEP_START) {
		uint8_t start[2] = { (uint8_t)e->slot, 1 };

		if (t150_client_call(T150_OP_EFFECT_START, start, 2) != 0)
			return DIERR_INPUTLOST;
		e->playing = 1;
	}

	return DI_OK;
}

static HRESULT WINAPI
eff_Start(IDirectInputEffect *self, DWORD iterations, DWORD flags)
{
	struct effect_obj *e = from_iface(self);
	uint8_t start[2];

	(void)flags;

	start[0] = (uint8_t)e->slot;
	start[1] = iterations >= 255 ? 255 : (uint8_t)iterations;
	if (start[1] == 0)
		start[1] = 1;

	if (t150_client_call(T150_OP_EFFECT_START, start, 2) != 0)
		return DIERR_INPUTLOST;
	e->playing = 1;

	return DI_OK;
}

static HRESULT WINAPI
eff_Stop(IDirectInputEffect *self)
{
	struct effect_obj *e = from_iface(self);
	uint8_t slot = (uint8_t)e->slot;

	if (t150_client_call(T150_OP_EFFECT_STOP, &slot, 1) != 0)
		return DIERR_INPUTLOST;
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
	return upload(from_iface(self)) == 0 ? DI_OK : DIERR_INPUTLOST;
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
	if ((slot = t150_device_slot_alloc(dev)) < 0)
		return DIERR_DEVICEFULL;
	if ((e = calloc(1, sizeof(*e))) == NULL) {
		t150_device_slot_free(dev, slot);
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

	if (params != NULL) {
		t150_effect_convert(&e->ef, params, params->dwFlags | DIEP_DURATION |
		    DIEP_STARTDELAY | DIEP_GAIN | DIEP_DIRECTION |
		    DIEP_ENVELOPE | DIEP_TYPESPECIFICPARAMS);
		if (upload(e) != 0) {
			IDirectInputEffect_Release(&e->iface);
			return DIERR_INPUTLOST;
		}
	}

	*out = &e->iface;

	return DI_OK;
}
