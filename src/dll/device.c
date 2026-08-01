/*
 * device.c - the wrapped IDirectInputDevice8.
 *
 * Only the wheel gets wrapped, and only its force feedback surface is
 * touched. Everything else is forwarded straight through, which is why axes,
 * pedals and buttons keep working exactly as they did without this DLL.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>

#include "proxy.h"
#include "t150/t150.h"

/* A DirectInput product GUID carries the USB ids in its first word. */
int
t150_is_wheel(const GUID *product)
{
	if (product == NULL)
		return 0;

	return product->Data1 == (DWORD)((T150_PID_FIRMWARE << 16) | T150_VID);
}

int
t150_device_slot_alloc(struct t150_device *dev)
{
	int i;

	for (i = 0; i < (int)T150_SLOT_MAX; i++) {
		if ((dev->slots & (1u << i)) == 0) {
			dev->slots |= (uint16_t)(1u << i);
			return i;
		}
	}

	return -1;
}

void
t150_device_slot_free(struct t150_device *dev, int slot)
{
	if (slot >= 0 && slot < (int)T150_SLOT_MAX)
		dev->slots &= (uint16_t)~(1u << slot);
}

static struct t150_device *
from_iface(IDirectInputDevice8W *p)
{
	return (struct t150_device *)p;
}

#define INNER(self) (from_iface(self)->inner)

/*
 * The effects we offer. Everything the daemon can render or downgrade is
 * listed, because a game told an effect is unsupported may give up on force
 * feedback entirely, and an approximated effect is better than none.
 */
struct effect_desc {
	const GUID	*guid;
	DWORD		 type;
	const char	*name;
};

#define FF_ENV	(DIEFT_FFATTACK | DIEFT_FFFADE)
#define FF_COND	(DIEFT_POSNEGCOEFFICIENTS | DIEFT_SATURATION | DIEFT_DEADBAND)

static const struct effect_desc effects[] = {
	{ &GUID_ConstantForce, DIEFT_CONSTANTFORCE | FF_ENV, "Constant Force" },
	{ &GUID_RampForce, DIEFT_RAMPFORCE | FF_ENV, "Ramp Force" },
	{ &GUID_Square, DIEFT_PERIODIC | FF_ENV, "Square Wave" },
	{ &GUID_Sine, DIEFT_PERIODIC | FF_ENV, "Sine Wave" },
	{ &GUID_Triangle, DIEFT_PERIODIC | FF_ENV, "Triangle Wave" },
	{ &GUID_SawtoothUp, DIEFT_PERIODIC | FF_ENV, "Sawtooth Up" },
	{ &GUID_SawtoothDown, DIEFT_PERIODIC | FF_ENV, "Sawtooth Down" },
	{ &GUID_Spring, DIEFT_CONDITION | FF_COND, "Spring" },
	{ &GUID_Damper, DIEFT_CONDITION | FF_COND, "Damper" },
	{ &GUID_Friction, DIEFT_CONDITION | FF_COND, "Friction" },
	{ &GUID_Inertia, DIEFT_CONDITION | FF_COND, "Inertia" }
};

#define EFFECT_PARAMS							\
	(DIEP_DURATION | DIEP_GAIN | DIEP_DIRECTION | DIEP_ENVELOPE |	\
	 DIEP_TYPESPECIFICPARAMS | DIEP_STARTDELAY)

static void
fill_info(const struct effect_desc *d, void *out, int wide)
{
	if (wide) {
		DIEFFECTINFOW *w = out;
		int i;

		w->guid = *d->guid;
		w->dwEffType = d->type;
		w->dwStaticParams = EFFECT_PARAMS;
		w->dwDynamicParams = EFFECT_PARAMS;
		for (i = 0; d->name[i] != '\0' && i < MAX_PATH - 1; i++)
			w->tszName[i] = (WCHAR)d->name[i];
		w->tszName[i] = L'\0';
	} else {
		DIEFFECTINFOA *a = out;

		a->guid = *d->guid;
		a->dwEffType = d->type;
		a->dwStaticParams = EFFECT_PARAMS;
		a->dwDynamicParams = EFFECT_PARAMS;
		(void)strncpy(a->tszName, d->name, MAX_PATH - 1);
		a->tszName[MAX_PATH - 1] = '\0';
	}
}

static HRESULT WINAPI
dev_QueryInterface(IDirectInputDevice8W *self, REFIID iid, void **out)
{
	if (out == NULL)
		return E_POINTER;

	if (IsEqualGUID(iid, &IID_IUnknown) ||
	    IsEqualGUID(iid, &IID_IDirectInputDevice8W) ||
	    IsEqualGUID(iid, &IID_IDirectInputDevice8A)) {
		IDirectInputDevice8_AddRef(self);
		*out = self;
		return S_OK;
	}

	/*
	 * Anything else is the builtin's business. A game that asks for an
	 * older interface gets the real one and loses force feedback, which
	 * is better than getting a wrapper whose vtable does not match what
	 * it asked for.
	 */
	return IDirectInputDevice8_QueryInterface(INNER(self), iid, out);
}

static ULONG WINAPI
dev_AddRef(IDirectInputDevice8W *self)
{
	return (ULONG)InterlockedIncrement(&from_iface(self)->refs);
}

static ULONG WINAPI
dev_Release(IDirectInputDevice8W *self)
{
	struct t150_device *d = from_iface(self);
	LONG r = InterlockedDecrement(&d->refs);

	if (r == 0) {
		IDirectInputDevice8_Release(d->inner);
		free(d);
	}

	return (ULONG)r;
}

static HRESULT WINAPI
dev_GetCapabilities(IDirectInputDevice8W *self, LPDIDEVCAPS caps)
{
	HRESULT hr = IDirectInputDevice8_GetCapabilities(INNER(self), caps);

	if (FAILED(hr) || caps == NULL)
		return hr;

	/*
	 * This is the answer the whole project exists to change. Wine derives
	 * force feedback from a PID collection in the descriptor and the
	 * wheel has none, so without this the game never asks for anything
	 * else.
	 */
	caps->dwFlags |= DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE |
	    DIDC_POSNEGCOEFFICIENTS | DIDC_SATURATION | DIDC_DEADBAND;
	caps->dwFFSamplePeriod = 1000;
	caps->dwFFMinTimeResolution = 1000;
	caps->dwFFDriverVersion = 1;

	return hr;
}

static HRESULT WINAPI
dev_EnumObjects(IDirectInputDevice8W *self, LPDIENUMDEVICEOBJECTSCALLBACKW cb,
    LPVOID ref, DWORD flags)
{
	return IDirectInputDevice8_EnumObjects(INNER(self), cb, ref, flags);
}

static HRESULT WINAPI
dev_GetProperty(IDirectInputDevice8W *self, REFGUID prop, LPDIPROPHEADER hdr)
{
	struct t150_device *d = from_iface(self);

	if (hdr != NULL && hdr->dwSize >= sizeof(DIPROPDWORD)) {
		DIPROPDWORD *dw = (DIPROPDWORD *)hdr;

		if (prop == DIPROP_FFGAIN) {
			dw->dwData = d->gain;
			return DI_OK;
		}
		if (prop == DIPROP_AUTOCENTER) {
			dw->dwData = d->autocenter;
			return DI_OK;
		}
	}

	return IDirectInputDevice8_GetProperty(INNER(self), prop, hdr);
}

static HRESULT WINAPI
dev_SetProperty(IDirectInputDevice8W *self, REFGUID prop, LPCDIPROPHEADER hdr)
{
	struct t150_device *d = from_iface(self);

	if (hdr != NULL && hdr->dwSize >= sizeof(DIPROPDWORD)) {
		const DIPROPDWORD *dw = (const DIPROPDWORD *)hdr;
		uint8_t arg[4];
		uint32_t v;

		if (prop == DIPROP_FFGAIN || prop == DIPROP_AUTOCENTER) {
			if (prop == DIPROP_FFGAIN) {
				d->gain = dw->dwData;
				v = dw->dwData;
			} else {
				d->autocenter = dw->dwData;
				/* DirectInput's autocenter is on or off. */
				v = dw->dwData == DIPROPAUTOCENTER_OFF ? 0 :
				    (uint32_t)T150_DI_MAX;
			}
			arg[0] = (uint8_t)(v & 0xff);
			arg[1] = (uint8_t)((v >> 8) & 0xff);
			arg[2] = (uint8_t)((v >> 16) & 0xff);
			arg[3] = (uint8_t)((v >> 24) & 0xff);

			if (t150_client_call(prop == DIPROP_FFGAIN ?
			    T150_OP_SET_GAIN : T150_OP_SET_AUTOCENTER, arg,
			    sizeof(arg)) != 0)
				return DIERR_INPUTLOST;

			return DI_OK;
		}
	}

	return IDirectInputDevice8_SetProperty(INNER(self), prop, hdr);
}

static HRESULT WINAPI
dev_Acquire(IDirectInputDevice8W *self)
{
	return IDirectInputDevice8_Acquire(INNER(self));
}

static HRESULT WINAPI
dev_Unacquire(IDirectInputDevice8W *self)
{
	/*
	 * DirectInput's contract is that unacquiring stops force feedback.
	 * The daemon's watchdog would get there eventually, but only after
	 * half a second of the wheel still pulling.
	 */
	(void)t150_client_call(T150_OP_RESET, NULL, 0);

	return IDirectInputDevice8_Unacquire(INNER(self));
}

static HRESULT WINAPI
dev_GetDeviceState(IDirectInputDevice8W *self, DWORD len, LPVOID data)
{
	return IDirectInputDevice8_GetDeviceState(INNER(self), len, data);
}

static HRESULT WINAPI
dev_GetDeviceData(IDirectInputDevice8W *self, DWORD len,
    LPDIDEVICEOBJECTDATA data, LPDWORD inout, DWORD flags)
{
	return IDirectInputDevice8_GetDeviceData(INNER(self), len, data, inout,
	    flags);
}

static HRESULT WINAPI
dev_SetDataFormat(IDirectInputDevice8W *self, LPCDIDATAFORMAT df)
{
	return IDirectInputDevice8_SetDataFormat(INNER(self), df);
}

static HRESULT WINAPI
dev_SetEventNotification(IDirectInputDevice8W *self, HANDLE ev)
{
	return IDirectInputDevice8_SetEventNotification(INNER(self), ev);
}

static HRESULT WINAPI
dev_SetCooperativeLevel(IDirectInputDevice8W *self, HWND hwnd, DWORD flags)
{
	return IDirectInputDevice8_SetCooperativeLevel(INNER(self), hwnd, flags);
}

static HRESULT WINAPI
dev_GetObjectInfo(IDirectInputDevice8W *self, LPDIDEVICEOBJECTINSTANCEW obj,
    DWORD how, DWORD flags)
{
	return IDirectInputDevice8_GetObjectInfo(INNER(self), obj, how, flags);
}

static HRESULT WINAPI
dev_GetDeviceInfo(IDirectInputDevice8W *self, LPDIDEVICEINSTANCEW inst)
{
	return IDirectInputDevice8_GetDeviceInfo(INNER(self), inst);
}

static HRESULT WINAPI
dev_RunControlPanel(IDirectInputDevice8W *self, HWND owner, DWORD flags)
{
	return IDirectInputDevice8_RunControlPanel(INNER(self), owner, flags);
}

static HRESULT WINAPI
dev_Initialize(IDirectInputDevice8W *self, HINSTANCE inst, DWORD ver,
    REFGUID guid)
{
	return IDirectInputDevice8_Initialize(INNER(self), inst, ver, guid);
}

static HRESULT WINAPI
dev_CreateEffect(IDirectInputDevice8W *self, REFGUID guid, LPCDIEFFECT eff,
    LPDIRECTINPUTEFFECT *out, LPUNKNOWN outer)
{
	if (outer != NULL)
		return CLASS_E_NOAGGREGATION;
	if (out == NULL)
		return E_POINTER;

	return t150_effect_create(from_iface(self), guid, eff, out);
}

static HRESULT WINAPI
dev_EnumEffects(IDirectInputDevice8W *self, LPDIENUMEFFECTSCALLBACKW cb,
    LPVOID ref, DWORD type)
{
	struct t150_device *d = from_iface(self);
	BOOL (WINAPI *fn)(const void *, void *) = (void *)cb;
	size_t i;

	if (cb == NULL)
		return E_POINTER;

	for (i = 0; i < sizeof(effects) / sizeof(effects[0]); i++) {
		union {
			DIEFFECTINFOW w;
			DIEFFECTINFOA a;
		} info;

		if (type != DIEFT_ALL &&
		    DIEFT_GETTYPE(effects[i].type) != DIEFT_GETTYPE(type))
			continue;

		memset(&info, 0, sizeof(info));
		if (d->wide)
			info.w.dwSize = sizeof(DIEFFECTINFOW);
		else
			info.a.dwSize = sizeof(DIEFFECTINFOA);
		fill_info(&effects[i], &info, d->wide);

		if (fn(&info, ref) == DIENUM_STOP)
			break;
	}

	return DI_OK;
}

static HRESULT WINAPI
dev_GetEffectInfo(IDirectInputDevice8W *self, LPDIEFFECTINFOW info, REFGUID guid)
{
	struct t150_device *d = from_iface(self);
	size_t i;

	if (info == NULL)
		return E_POINTER;

	for (i = 0; i < sizeof(effects) / sizeof(effects[0]); i++) {
		if (!IsEqualGUID(effects[i].guid, guid))
			continue;
		fill_info(&effects[i], info, d->wide);
		return DI_OK;
	}

	return DIERR_DEVICENOTREG;
}

static HRESULT WINAPI
dev_GetForceFeedbackState(IDirectInputDevice8W *self, LPDWORD out)
{
	(void)self;

	if (out == NULL)
		return E_POINTER;

	*out = t150_client_online() ?
	    (DIGFFS_POWERON | DIGFFS_ACTUATORSON | DIGFFS_EMPTY) :
	    (DIGFFS_POWEROFF | DIGFFS_ACTUATORSOFF);

	return DI_OK;
}

static HRESULT WINAPI
dev_SendForceFeedbackCommand(IDirectInputDevice8W *self, DWORD flags)
{
	(void)self;

	if (flags & (DISFFC_RESET | DISFFC_STOPALL)) {
		if (t150_client_call(T150_OP_RESET, NULL, 0) != 0)
			return DIERR_INPUTLOST;
	}

	/* Pause, continue and the actuator switches have no wire opcode. */
	return DI_OK;
}

static HRESULT WINAPI
dev_EnumCreatedEffectObjects(IDirectInputDevice8W *self,
    LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD fl)
{
	(void)self;
	(void)cb;
	(void)ref;
	(void)fl;

	return DI_OK;
}

static HRESULT WINAPI
dev_Escape(IDirectInputDevice8W *self, LPDIEFFESCAPE esc)
{
	return IDirectInputDevice8_Escape(INNER(self), esc);
}

static HRESULT WINAPI
dev_Poll(IDirectInputDevice8W *self)
{
	return IDirectInputDevice8_Poll(INNER(self));
}

static HRESULT WINAPI
dev_SendDeviceData(IDirectInputDevice8W *self, DWORD len,
    LPCDIDEVICEOBJECTDATA data, LPDWORD inout, DWORD fl)
{
	return IDirectInputDevice8_SendDeviceData(INNER(self), len, data, inout,
	    fl);
}

static HRESULT WINAPI
dev_EnumEffectsInFile(IDirectInputDevice8W *self, LPCWSTR file,
    LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID ref, DWORD flags)
{
	return IDirectInputDevice8_EnumEffectsInFile(INNER(self), file, cb, ref,
	    flags);
}

static HRESULT WINAPI
dev_WriteEffectToFile(IDirectInputDevice8W *self, LPCWSTR file, DWORD entries,
    LPDIFILEEFFECT eft, DWORD flags)
{
	return IDirectInputDevice8_WriteEffectToFile(INNER(self), file, entries,
	    eft, flags);
}

static HRESULT WINAPI
dev_BuildActionMap(IDirectInputDevice8W *self, LPDIACTIONFORMATW fmt,
    LPCWSTR user, DWORD flags)
{
	return IDirectInputDevice8_BuildActionMap(INNER(self), fmt, user, flags);
}

static HRESULT WINAPI
dev_SetActionMap(IDirectInputDevice8W *self, LPDIACTIONFORMATW fmt,
    LPCWSTR user, DWORD flags)
{
	return IDirectInputDevice8_SetActionMap(INNER(self), fmt, user, flags);
}

static HRESULT WINAPI
dev_GetImageInfo(IDirectInputDevice8W *self, LPDIDEVICEIMAGEINFOHEADERW hdr)
{
	return IDirectInputDevice8_GetImageInfo(INNER(self), hdr);
}

static const IDirectInputDevice8WVtbl device_vtbl = {
	dev_QueryInterface,
	dev_AddRef,
	dev_Release,
	dev_GetCapabilities,
	dev_EnumObjects,
	dev_GetProperty,
	dev_SetProperty,
	dev_Acquire,
	dev_Unacquire,
	dev_GetDeviceState,
	dev_GetDeviceData,
	dev_SetDataFormat,
	dev_SetEventNotification,
	dev_SetCooperativeLevel,
	dev_GetObjectInfo,
	dev_GetDeviceInfo,
	dev_RunControlPanel,
	dev_Initialize,
	dev_CreateEffect,
	dev_EnumEffects,
	dev_GetEffectInfo,
	dev_GetForceFeedbackState,
	dev_SendForceFeedbackCommand,
	dev_EnumCreatedEffectObjects,
	dev_Escape,
	dev_Poll,
	dev_SendDeviceData,
	dev_EnumEffectsInFile,
	dev_WriteEffectToFile,
	dev_BuildActionMap,
	dev_SetActionMap,
	dev_GetImageInfo
};

HRESULT
t150_device_wrap(IDirectInputDevice8W *inner, int wide, void **out)
{
	struct t150_device *d;

	if ((d = calloc(1, sizeof(*d))) == NULL)
		return E_OUTOFMEMORY;

	d->vtbl = &device_vtbl;
	d->inner = inner;
	d->refs = 1;
	d->wide = wide;
	d->gain = T150_DI_MAX;
	d->autocenter = DIPROPAUTOCENTER_ON;
	*out = d;

	return DI_OK;
}
