/*
 * device.c - the wrapped IDirectInputDevice8.
 *
 * Only the wheel gets wrapped, and only its force feedback surface is
 * touched, with one measured exception: the pedals. The T150's descriptor
 * labels them backwards, Y is the brake and Rz the accelerator, where every
 * game and preset is built for the common convention of Y gas, Rz brake,
 * and its firmware rests a released pedal at logical max, so a stock game
 * brakes on the accelerator and drives itself on a throttle it reads as
 * held open. On Windows the vendor driver corrects both; here the wrap
 * swaps the two values and mirrors them inside the game's own axis range,
 * so a released pedal reads zero. T150_PEDALS takes "raw", "swap" or
 * "invert" to get less than both. Everything else is forwarded straight
 * through. RESEARCH.md A37 and A38 are the measurements.
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
 * The pedal transforms apply only to the two stock joystick formats,
 * recognised by their state sizes, where Y and Rz live at fixed offsets. A
 * game that built its own format gets its data untouched, because offsets
 * there mean whatever the game said they mean. Both stock structs start with
 * the same axis block, so the offsets are one pair.
 *
 * The swap corrects the descriptor's backwards labels, brake on Y and
 * accelerator on Rz where games expect the opposite. The inversion corrects
 * the rest position: the firmware reports a released pedal at logical max,
 * so a game reads the throttle as held open and the car drives itself,
 * which test 20's video shows happening. Together they present the pedals
 * the way the vendor driver does on Windows.
 */
#define STATE_DIJOYSTATE	80u
#define STATE_DIJOYSTATE2	272u
#define OFS_Y			4u
#define OFS_RZ			20u
#define PEDAL_Y			0
#define PEDAL_RZ		1

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

	/*
	 * The pedal inversion mirrors values inside the range the game set,
	 * so remember it. Device-wide sets cover both pedals; a per-offset
	 * set names one, and its offsets are only meaningful under the stock
	 * formats, which is the only time the transform runs anyway.
	 */
	if (prop == DIPROP_RANGE && hdr != NULL &&
	    hdr->dwSize >= sizeof(DIPROPRANGE)) {
		const DIPROPRANGE *r = (const DIPROPRANGE *)hdr;
		struct t150_device *dd = from_iface(self);

		if (hdr->dwHow == DIPH_DEVICE) {
			dd->range_min[PEDAL_Y] = r->lMin;
			dd->range_max[PEDAL_Y] = r->lMax;
			dd->range_min[PEDAL_RZ] = r->lMin;
			dd->range_max[PEDAL_RZ] = r->lMax;
		} else if (hdr->dwHow == DIPH_BYOFFSET &&
		    hdr->dwObj == OFS_Y) {
			dd->range_min[PEDAL_Y] = r->lMin;
			dd->range_max[PEDAL_Y] = r->lMax;
		} else if (hdr->dwHow == DIPH_BYOFFSET &&
		    hdr->dwObj == OFS_RZ) {
			dd->range_min[PEDAL_RZ] = r->lMin;
			dd->range_max[PEDAL_RZ] = r->lMax;
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

static int
pedals_apply(const struct t150_device *d, DWORD len)
{
	return (d->pedal_swap || d->pedal_invert) && len == d->df_size &&
	    (len == STATE_DIJOYSTATE || len == STATE_DIJOYSTATE2);
}

/* Mirror a value inside the range the game configured for that axis. */
static LONG
pedal_flip(const struct t150_device *d, int axis, LONG v)
{
	return d->range_min[axis] + d->range_max[axis] - v;
}

static HRESULT WINAPI
dev_GetDeviceState(IDirectInputDevice8W *self, DWORD len, LPVOID data)
{
	struct t150_device *d = from_iface(self);
	HRESULT hr;

	hr = IDirectInputDevice8_GetDeviceState(d->inner, len, data);
	if (hr == DI_OK && pedals_apply(d, len)) {
		LONG *y = (LONG *)((char *)data + OFS_Y);
		LONG *rz = (LONG *)((char *)data + OFS_RZ);

		if (d->pedal_swap) {
			LONG tmp = *y;

			*y = *rz;
			*rz = tmp;
		}
		if (d->pedal_invert) {
			*y = pedal_flip(d, PEDAL_Y, *y);
			*rz = pedal_flip(d, PEDAL_RZ, *rz);
		}
	}

	return hr;
}

static HRESULT WINAPI
dev_GetDeviceData(IDirectInputDevice8W *self, DWORD len,
    LPDIDEVICEOBJECTDATA data, LPDWORD inout, DWORD flags)
{
	struct t150_device *d = from_iface(self);
	HRESULT hr;
	DWORD i;

	hr = IDirectInputDevice8_GetDeviceData(d->inner, len, data, inout,
	    flags);
	if (hr != DI_OK && hr != DI_BUFFEROVERFLOW)
		return hr;

	/*
	 * Buffered events carry the offset the value belongs to, so the swap
	 * here is a relabel, a change on Y reported as a change on Rz and the
	 * other way round, and the inversion mirrors the value inside the
	 * destination axis's range, matching what GetDeviceState returns.
	 */
	if (data != NULL && inout != NULL &&
	    pedals_apply(d, d->df_size)) {
		for (i = 0; i < *inout; i++) {
			int axis;

			if (data[i].dwOfs == OFS_Y)
				axis = PEDAL_Y;
			else if (data[i].dwOfs == OFS_RZ)
				axis = PEDAL_RZ;
			else
				continue;

			if (d->pedal_swap) {
				axis = axis == PEDAL_Y ? PEDAL_RZ : PEDAL_Y;
				data[i].dwOfs = axis == PEDAL_Y ? OFS_Y :
				    OFS_RZ;
			}
			if (d->pedal_invert)
				data[i].dwData = (DWORD)pedal_flip(d, axis,
				    (LONG)data[i].dwData);
		}
	}

	return hr;
}

static HRESULT WINAPI
dev_SetDataFormat(IDirectInputDevice8W *self, LPCDIDATAFORMAT df)
{
	struct t150_device *d = from_iface(self);
	HRESULT hr;

	hr = IDirectInputDevice8_SetDataFormat(d->inner, df);
	if (hr == DI_OK && df != NULL) {
		d->df_size = df->dwDataSize;
		if (d->pedal_swap && df->dwDataSize != STATE_DIJOYSTATE &&
		    df->dwDataSize != STATE_DIJOYSTATE2)
			t150_log("custom data format (%lu bytes), pedals "
			    "left as the wheel labels them\n",
			    (unsigned long)df->dwDataSize);
	}

	return hr;
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

	/*
	 * These mean different things and used to be sent as the same op.
	 * RESET stops and releases every effect; STOPALL stops them and
	 * leaves them downloaded, so a game that pauses with STOPALL can
	 * start the same effects again. Sending STOPALL as a reset destroyed
	 * the slots underneath it.
	 */
	if (flags & DISFFC_RESET) {
		if (t150_client_call(T150_OP_RESET, NULL, 0) != 0)
			return DIERR_INPUTLOST;
	} else if (flags & DISFFC_STOPALL) {
		if (t150_client_call(T150_OP_STOP_ALL, NULL, 0) != 0)
			return DIERR_INPUTLOST;
	}

	/*
	 * Pause and actuators-off have no wire opcode of their own, and
	 * answering DI_OK while the wheel carried on pulling was the wrong
	 * way to be wrong: a game that pauses believes the wheel went quiet.
	 * Stop everything instead, which is the closest honest thing the
	 * protocol can do and errs towards a still wheel. The effects stay
	 * downloaded, so a continue only has to start them again.
	 */
	if (flags & (DISFFC_PAUSE | DISFFC_SETACTUATORSOFF)) {
		if (t150_client_call(T150_OP_STOP_ALL, NULL, 0) != 0)
			return DIERR_INPUTLOST;
	}

	/* Continue and actuators-on need nothing: the slots are still there. */
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

	/*
	 * Both pedal corrections are on unless asked off: the raw labels and
	 * the rest-at-max are the measured mistakes, so raw is the opt-out
	 * rather than the default. T150_PEDALS takes "raw" for neither, and
	 * "swap" or "invert" for just one, which exists for diagnosis.
	 */
	{
		char v[8];
		DWORD n = GetEnvironmentVariableA("T150_PEDALS", v, sizeof(v));

		d->pedal_swap = 1;
		d->pedal_invert = 1;
		if (n > 0 && n < sizeof(v)) {
			if (strcmp(v, "raw") == 0)
				d->pedal_swap = d->pedal_invert = 0;
			else if (strcmp(v, "swap") == 0)
				d->pedal_invert = 0;
			else if (strcmp(v, "invert") == 0)
				d->pedal_swap = 0;
		}
	}

	/* Until the game sets its own, DirectInput's default axis range. */
	d->range_min[PEDAL_Y] = 0;
	d->range_max[PEDAL_Y] = 65535;
	d->range_min[PEDAL_RZ] = 0;
	d->range_max[PEDAL_RZ] = 65535;

	/*
	 * One line that says which build is in the bottle and what it is
	 * doing to the pedals, because test 20 was spent unable to tell two
	 * proxy versions apart from their logs.
	 */
	t150_log("proxy %s, pedal swap %s, invert %s\n", T150_PROXY_VERSION,
	    d->pedal_swap ? "on" : "off", d->pedal_invert ? "on" : "off");

	*out = d;

	return DI_OK;
}
