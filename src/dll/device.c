/*
 * device.c - the wrapped IDirectInputDevice8.
 *
 * Only the wheel gets wrapped, and only its force feedback surface is
 * touched. Input is forwarded untouched, which is what makes this safe to
 * leave installed.
 *
 * The pedals are the one thing that could justify an exception and do not.
 * The T150's descriptor labels them backwards, the brake first and the
 * accelerator second where the common convention is the reverse, and its
 * firmware
 * rests a released pedal at logical maximum. A game that binds pedals by
 * asking the player to press them resolves both by itself, and correcting
 * them underneath such a game re-crosses what it got right, which is
 * measured twice over in RESEARCH.md A39 and A41. So T150_PEDALS is opt-in
 * for a game that assumes the convention and cannot rebind: swap for the
 * labels, invert for the rest position, full for both. A37 is the
 * measurement of what is actually wrong with the descriptor.
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
 * recognised by their state sizes, where Y and Z live at fixed offsets. A
 * game that built its own format gets its data untouched, because offsets
 * there mean whatever the game said they mean. Both stock structs start with
 * the same axis block, so the offsets are one pair.
 *
 * The swap corrects the descriptor's backwards labels, brake on Y and
 * accelerator on Z where games expect the opposite. The inversion corrects
 * the rest position: the firmware reports a released pedal at logical max,
 * so a game reads the throttle as held open and the car drives itself,
 * which test 20's video shows happening. Together they present the pedals
 * the way the vendor driver does on Windows.
 */
/*
 * Where the two pedals sit in the state struct, which is not where the
 * descriptor's usage names would put them. A37 measured the accelerator as
 * usage Rz and called it DirectInput axis 2; those are not the same place,
 * and only the second one is true in a bottle. Wine maps that axis to
 * DIJOFS_Z, offset 8, which test 27 confirmed on hardware: this wheel
 * publishes X, Y, Z and Rx and nothing at all at offset 20. Aiming the
 * corrections at Rz meant they mirrored the brake, left the accelerator
 * exactly as it was, and drove an axis the wheel does not have to its
 * maximum. RESEARCH.md A43 and A44.
 */
#define STATE_DIJOYSTATE	80u
#define STATE_DIJOYSTATE2	272u

/* The shortest buffered event DirectInput accepts: DIDEVICEOBJECTDATA_DX3. */
#define EVENT_DX3		(4u * sizeof(DWORD))
#define OFS_Y			4u
#define OFS_Z			8u
#define PEDAL_Y			0
#define PEDAL_Z			1

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

	/*
	 * dwFlags is at offset 4 and exists in every version of this struct,
	 * but the three force feedback fields sit at 24, 28 and 40, past the
	 * end of the 24-byte DIDEVCAPS_DX3 a caller is allowed to pass and
	 * which the builtin accepts. Writing them unguarded put twelve bytes
	 * up to twenty past such a caller's object.
	 */
	if (caps->dwSize >= sizeof(DIDEVCAPS)) {
		caps->dwFFSamplePeriod = 1000;
		caps->dwFFMinTimeResolution = 1000;
		caps->dwFFDriverVersion = 1;
	}

	t150_log("GetCapabilities, size %lu\n", (unsigned long)caps->dwSize);

	return hr;
}

/*
 * Say which axis the force goes to.
 *
 * Claiming DIDC_FORCEFEEDBACK on the device while forwarding EnumObjects
 * untouched leaves a device that has force feedback and no axis to apply it
 * to, which is a shape no real wheel has and one that callers handle badly.
 * RESEARCH.md B13 measured the cost in SDL: it counts only axes carrying
 * DIDOI_FFACTUATOR, finds none, and then builds a condition effect with
 * cAxes zero, so its parameter block is zero bytes, the size gate in
 * effect.c discards it, and a spring uploads with every coefficient zero
 * while every call returns DI_OK. The same zero also loses the direction of
 * every constant and periodic effect, and in SDL 2.30.12, the version
 * CrossOver 26 bundles, it walks a 24-byte memset into a one-byte
 * allocation in the game's own heap.
 *
 * The wheel is X, which A43 confirmed on hardware, and it is the only thing
 * this project can move. Marking it costs one bit in two fields and is what
 * a PID descriptor would have said if the T150 carried one.
 *
 * dwSize, guidType, dwOfs, dwType and dwFlags sit at the same offsets in
 * the A and W forms of this struct, so the marking is the same work for
 * both and the name that follows them is copied verbatim.
 */
struct enum_objects {
	LPDIENUMDEVICEOBJECTSCALLBACKW	 cb;
	void				*ref;
	int				 only_ff;
};

static int
is_wheel_axis(const DIDEVICEOBJECTINSTANCEW *o)
{
	return IsEqualGUID(&o->guidType, &GUID_XAxis) != 0;
}

static void
mark_actuator(DIDEVICEOBJECTINSTANCEW *o)
{
	o->dwType |= DIDFT_FFACTUATOR;
	o->dwFlags |= DIDOI_FFACTUATOR;
}

static BOOL CALLBACK
enum_objects_thunk(LPCDIDEVICEOBJECTINSTANCEW o, LPVOID ref)
{
	struct enum_objects *c = ref;
	DIDEVICEOBJECTINSTANCEW tmp;

	if (!is_wheel_axis(o))
		return c->only_ff ? DIENUM_CONTINUE : c->cb(o, c->ref);

	/* Anything larger than this build knows about is passed as it came. */
	if (o->dwSize > sizeof(tmp))
		return c->cb(o, c->ref);

	memcpy(&tmp, o, o->dwSize);
	mark_actuator(&tmp);

	return c->cb(&tmp, c->ref);
}

static HRESULT WINAPI
dev_EnumObjects(IDirectInputDevice8W *self, LPDIENUMDEVICEOBJECTSCALLBACKW cb,
    LPVOID ref, DWORD flags)
{
	struct enum_objects c;
	DWORD ask = flags;

	if (cb == NULL)
		return IDirectInputDevice8_EnumObjects(INNER(self), cb, ref,
		    flags);

	c.cb = cb;
	c.ref = ref;

	/*
	 * A caller asking only for actuators would get nothing from the
	 * device below, which has none, so ask it for the axes instead and
	 * let the thunk keep the one that is.
	 */
	c.only_ff = (flags & DIDFT_FFACTUATOR) != 0;
	if (c.only_ff)
		ask = (flags & ~(DWORD)DIDFT_FFACTUATOR) | DIDFT_AXIS;

	return IDirectInputDevice8_EnumObjects(INNER(self), enum_objects_thunk,
	    &c, ask);
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
	 * A range change invalidates the mirror's cached ranges, however the
	 * game addressed the axis; the next read re-asks DirectInput.
	 */
	if (prop == DIPROP_RANGE)
		d->ranges_stale = 1;

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

/*
 * Only the two stock joystick formats put the pedals at a known offset. In
 * a format the game built, an offset means whatever the game said it means,
 * so neither the transform nor the question of whether the axes are there
 * has an answer worth asking.
 */
static int
stock_format(DWORD len)
{
	return len == STATE_DIJOYSTATE || len == STATE_DIJOYSTATE2;
}

static int
pedals_wanted(const struct t150_device *d)
{
	return d->pedal_swap || d->pedal_invert;
}

static int
pedals_apply(const struct t150_device *d, DWORD len)
{
	return pedals_wanted(d) && d->pedals_found && len == d->df_size &&
	    stock_format(len);
}

/*
 * The mirror must use the range DirectInput actually applies, so ask it
 * rather than shadow the game's SetProperty calls: a game that sets ranges
 * by object id slipped past the shadowing and the mirror then produced
 * values outside the axis entirely, both pedals pinned at the pressed end,
 * which is the 0.1.6 regression a tester caught within the hour.
 */
static void
pedal_ranges_refresh(struct t150_device *d)
{
	static const DWORD ofs[2] = { OFS_Y, OFS_Z };
	DIPROPRANGE r;
	int i, found;

	/*
	 * The same question answers both halves. An axis that is not there
	 * has no range to report, so a failure here says the pedal this
	 * build expects is absent, and the honest response is to forward
	 * the wheel untouched and say so rather than transform a field
	 * nothing writes. That is the shape of the bug this replaced.
	 *
	 * The answer is decided fresh every time, never latched. This asks
	 * by offset, and an offset means whatever the current data format
	 * says it means, so a game that sets a custom format and then a
	 * stock one gets a different and equally correct answer each time.
	 * A latch would carry the first answer into the second format and
	 * disable the corrections for the life of the process.
	 */
	found = 1;
	for (i = 0; i < 2; i++) {
		memset(&r, 0, sizeof(r));
		r.diph.dwSize = sizeof(r);
		r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
		r.diph.dwHow = DIPH_BYOFFSET;
		r.diph.dwObj = ofs[i];
		if (SUCCEEDED(IDirectInputDevice8_GetProperty(d->inner,
		    DIPROP_RANGE, &r.diph))) {
			d->range_min[i] = r.lMin;
			d->range_max[i] = r.lMax;
			continue;
		}
		found = 0;
	}

	/*
	 * Say it once per change of answer rather than once per poll, and
	 * only for a format where the question was meaningful: a custom
	 * format has its own message from SetDataFormat and offset 4 there
	 * means whatever the game said, not a missing pedal.
	 */
	if (found != d->pedals_found && stock_format(d->df_size))
		t150_log("pedal axes %s at offsets %u and %u, corrections "
		    "%s\n", found ? "found" : "absent", OFS_Y, OFS_Z,
		    found ? "on" : "off");
	d->pedals_found = found;
	d->ranges_stale = 0;
}

/* Mirror a value inside the range DirectInput reports for that axis. */
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
	if (hr == DI_OK && d->ranges_stale && pedals_wanted(d) &&
	    stock_format(d->df_size))
		pedal_ranges_refresh(d);
	if (hr == DI_OK && pedals_apply(d, len)) {
		LONG *y = (LONG *)((char *)data + OFS_Y);
		LONG *z = (LONG *)((char *)data + OFS_Z);

		if (d->pedal_swap) {
			LONG tmp = *y;

			*y = *z;
			*z = tmp;
		}
		if (d->pedal_invert) {
			*y = pedal_flip(d, PEDAL_Y, *y);
			*z = pedal_flip(d, PEDAL_Z, *z);
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
	 * here is a relabel, a change on Y reported as a change on Z and the
	 * other way round, and the inversion mirrors the value inside the
	 * destination axis's range, matching what GetDeviceState returns.
	 */
	if (d->ranges_stale && pedals_wanted(d) && stock_format(d->df_size))
		pedal_ranges_refresh(d);

	/*
	 * The caller chose the element size and DirectInput accepts the DX3
	 * one, four DWORDs with no uAppData, which is eight bytes shorter
	 * than the struct this parameter is typed as. Indexing that type
	 * would step past the end of the caller's array on the second event
	 * and write there, so the stride is the size the caller gave. dwOfs
	 * and dwData are the first two fields of both forms.
	 */
	if (data != NULL && inout != NULL && len >= EVENT_DX3 &&
	    pedals_apply(d, d->df_size)) {
		for (i = 0; i < *inout; i++) {
			DWORD *ofs = (DWORD *)((char *)data + (size_t)i * len);
			DWORD *val = ofs + 1;
			int axis;

			if (*ofs == OFS_Y)
				axis = PEDAL_Y;
			else if (*ofs == OFS_Z)
				axis = PEDAL_Z;
			else
				continue;

			if (d->pedal_swap) {
				axis = axis == PEDAL_Y ? PEDAL_Z : PEDAL_Y;
				*ofs = axis == PEDAL_Y ? OFS_Y : OFS_Z;
			}
			if (d->pedal_invert)
				*val = (DWORD)pedal_flip(d, axis, (LONG)*val);
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
		d->ranges_stale = 1;
		if (d->pedal_swap && !stock_format(df->dwDataSize))
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
	HRESULT hr;

	hr = IDirectInputDevice8_GetObjectInfo(INNER(self), obj, how, flags);

	/* The same axis EnumObjects marks, for a caller that asks directly. */
	if (hr == DI_OK && obj != NULL && is_wheel_axis(obj))
		mark_actuator(obj);

	return hr;
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
	HRESULT hr;

	if (outer != NULL)
		return CLASS_E_NOAGGREGATION;
	if (out == NULL)
		return E_POINTER;

	/*
	 * The line that says which of the four possible failures is happening.
	 * cAxes and cbTypeSpecificParams are the two fields that decide
	 * whether a condition effect arrives with real coefficients or with
	 * the zeros a caller produces when it believes the device has no
	 * force feedback axis. RESEARCH.md B13.
	 */
	if (eff != NULL)
		t150_log("CreateEffect %08lx flags 0x%08lx cAxes %lu "
		    "typeparams %lu\n", (unsigned long)guid->Data1,
		    (unsigned long)eff->dwFlags, (unsigned long)eff->cAxes,
		    (unsigned long)eff->cbTypeSpecificParams);
	else
		t150_log("CreateEffect %08lx with no parameters\n",
		    (unsigned long)guid->Data1);

	hr = t150_effect_create(from_iface(self), guid, eff, out);
	if (FAILED(hr))
		t150_log("CreateEffect refused, hr 0x%08lx\n",
		    (unsigned long)hr);

	return hr;
}

static HRESULT WINAPI
dev_EnumEffects(IDirectInputDevice8W *self, LPDIENUMEFFECTSCALLBACKW cb,
    LPVOID ref, DWORD type)
{
	struct t150_device *d = from_iface(self);
	BOOL (WINAPI *fn)(const void *, void *) = (void *)cb;
	size_t i;
	DWORD want;

	if (cb == NULL)
		return E_POINTER;

	/*
	 * Mask before comparing, the way Wine does, and compare the masked
	 * value: the reduction was added but the test below still used the
	 * raw type, so a caller asking for DIEFT_ALL with any flag bit set,
	 * DIEFT_FFATTACK say, failed "not DIEFT_ALL" and then matched
	 * nothing, because no entry has a type nibble of zero. That is an
	 * empty enumeration returned as DI_OK. SDL treats no supported
	 * effects as a fatal error and abandons force feedback for the
	 * device, so it was a silent way to lose everything. No caller is
	 * known to pass that combination today.
	 */
	want = DIEFT_GETTYPE(type);
	t150_log("EnumEffects type 0x%08lx\n", (unsigned long)type);

	for (i = 0; i < sizeof(effects) / sizeof(effects[0]); i++) {
		union {
			DIEFFECTINFOW w;
			DIEFFECTINFOA a;
		} info;

		if (want != DIEFT_ALL && DIEFT_GETTYPE(effects[i].type) != want)
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
	/*
	 * A stop that could not be delivered because the wheel is not there
	 * is a stop that has already happened: an absent wheel renders
	 * nothing. Answering DIERR_INPUTLOST instead sent a game that was
	 * merely tidying up into a recovery it cannot complete, and these
	 * three are exactly the calls a game makes while reinitialising
	 * force feedback after being told its input was lost.
	 */
	if (flags & DISFFC_RESET) {
		(void)t150_client_call(T150_OP_RESET, NULL, 0);
	} else if (flags & DISFFC_STOPALL) {
		(void)t150_client_call(T150_OP_STOP_ALL, NULL, 0);
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
		(void)t150_client_call(T150_OP_STOP_ALL, NULL, 0);
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
	 * Both corrections are off by default, and that is a decision taken
	 * twice the hard way. The swap and the mirror each shipped on, and
	 * each regressed a working setup: a game that binds pedals by asking
	 * the player to press them has already resolved both the labels and
	 * the direction, and rewriting its input underneath re-crosses what it
	 * got right. Assetto Corsa bound this wheel correctly against the raw
	 * layout and every default here has made it worse.
	 *
	 * So the proxy stays what it says it is, a force feedback proxy that
	 * forwards input untouched, and the corrections are there for a game
	 * that assumes the common convention and offers no way to rebind:
	 * swap for the labels, invert for the rest position, full for both.
	 * RESEARCH.md A41.
	 */
	{
		char v[8];
		DWORD n = GetEnvironmentVariableA("T150_PEDALS", v, sizeof(v));

		d->pedal_swap = 0;
		d->pedal_invert = 0;
		if (n > 0 && n < sizeof(v)) {
			if (strcmp(v, "swap") == 0)
				d->pedal_swap = 1;
			else if (strcmp(v, "invert") == 0)
				d->pedal_invert = 1;
			else if (strcmp(v, "full") == 0)
				d->pedal_swap = d->pedal_invert = 1;
		}
	}

	/* DirectInput's default axis range, replaced by asking dinput. */
	d->ranges_stale = 1;
	d->pedals_found = 1;
	d->range_min[PEDAL_Y] = 0;
	d->range_max[PEDAL_Y] = 65535;
	d->range_min[PEDAL_Z] = 0;
	d->range_max[PEDAL_Z] = 65535;

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
