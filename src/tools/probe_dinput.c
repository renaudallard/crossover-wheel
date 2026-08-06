/*
 * probe_dinput - what the wheel looks like from inside the bottle.
 *
 * Every other probe in this project runs on macOS and asks the wheel
 * questions directly. This one runs in the CrossOver bottle and asks
 * DirectInput, which is the layer a game actually sees and the one every
 * confusing report has come from: a pedal that arrived as a button, an axis
 * that changed number, a value that rested at maximum. Those are all one
 * screen of output here, and none of them needs a game or a game's saved
 * configuration to be in any particular state.
 *
 * Three things it does:
 *
 *   the dump         what the device and every one of its objects declare
 *   -i               press each control and be told what moved and by how much
 *   -f               create a real force feedback effect and ask whether the
 *                    wheel moved, with no game involved at all
 *
 * The last is the measurement this project has been unable to make. A game
 * asking wrongly and a chain that does not deliver look identical from
 * outside; this separates them.
 *
 * It goes through whatever dinput8.dll the bottle resolves, so with the
 * proxy installed it measures the proxy. Running it again with the
 * wrapper's --dll dinput8=b forces CrossOver's builtin, and the difference
 * between the two runs is exactly what the proxy does to the device.
 *
 * Cross built with mingw-w64. Windows only, by definition.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS

#include <windows.h>
#include <objbase.h>
#include <dinput.h>

#include <stdio.h>
#include <string.h>

#include "t150/t150.h"

/* How long to watch for movement when asking for one control. */
#define WATCH_MS	6000
#define POLL_MS		20

/* A press has to move a good part of the range to count, not just jitter. */
#define MOVE_MIN	2000

struct axis_seen {
	DWORD	ofs;
	char	name[64];
	LONG	rest;
	LONG	low;
	LONG	high;
	int	is_ff;
};

struct button_seen {
	DWORD	ofs;
	char	name[MAX_PATH];	/* what DIDEVICEOBJECTINSTANCE can hand back */
};

static struct axis_seen axes[16];
static struct button_seen buttons[64];
static int naxes, nbuttons;

static IDirectInput8A *di;
static IDirectInputDevice8A *dev;

static const char *
guid_axis_name(const GUID *g)
{
	if (IsEqualGUID(g, &GUID_XAxis)) return "X";
	if (IsEqualGUID(g, &GUID_YAxis)) return "Y";
	if (IsEqualGUID(g, &GUID_ZAxis)) return "Z";
	if (IsEqualGUID(g, &GUID_RxAxis)) return "Rx";
	if (IsEqualGUID(g, &GUID_RyAxis)) return "Ry";
	if (IsEqualGUID(g, &GUID_RzAxis)) return "Rz";
	if (IsEqualGUID(g, &GUID_Slider)) return "Slider";
	if (IsEqualGUID(g, &GUID_POV)) return "POV";
	if (IsEqualGUID(g, &GUID_Button)) return "Button";
	return "?";
}

/*
 * DirectInput numbers axes by their offset in the state struct divided by
 * four, which is the number a game's configuration file usually records, so
 * printing it saves comparing hex offsets against a game's ini by hand.
 */
static unsigned
axle_number(DWORD ofs)
{
	return (unsigned)(ofs / 4);
}

static BOOL CALLBACK
on_object(const DIDEVICEOBJECTINSTANCEA *o, void *ctx)
{
	int ff = (o->dwType & DIDFT_FFACTUATOR) != 0;

	(void)ctx;

	if (o->dwType & DIDFT_AXIS) {
		DIPROPRANGE r;

		memset(&r, 0, sizeof(r));
		r.diph.dwSize = sizeof(r);
		r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
		r.diph.dwHow = DIPH_BYID;
		r.diph.dwObj = o->dwType;
		r.lMin = r.lMax = 0;
		(void)IDirectInputDevice8_GetProperty(dev, DIPROP_RANGE,
		    &r.diph);

		printf("  axis   ofs %3lu (axle %u)  %-6s  range %ld..%ld  "
		    "usage %02x/%02x  %s%s\n", (unsigned long)o->dwOfs,
		    axle_number(o->dwOfs), guid_axis_name(&o->guidType),
		    (long)r.lMin, (long)r.lMax, o->wUsagePage, o->wUsage,
		    o->tszName, ff ? "  [force feedback actuator]" : "");

		if (naxes < (int)(sizeof(axes) / sizeof(axes[0]))) {
			axes[naxes].ofs = o->dwOfs;
			axes[naxes].is_ff = ff;
			(void)snprintf(axes[naxes].name,
			    sizeof(axes[naxes].name), "%s",
			    guid_axis_name(&o->guidType));
			naxes++;
		}
	} else if (o->dwType & DIDFT_BUTTON) {
		printf("  button ofs %3lu (button %lu)  %s\n",
		    (unsigned long)o->dwOfs,
		    (unsigned long)(o->dwOfs - DIJOFS_BUTTON0), o->tszName);

		if (nbuttons < (int)(sizeof(buttons) / sizeof(buttons[0]))) {
			buttons[nbuttons].ofs = o->dwOfs;
			(void)snprintf(buttons[nbuttons].name,
			    sizeof(buttons[nbuttons].name), "%s", o->tszName);
			nbuttons++;
		}
	} else if (o->dwType & DIDFT_POV) {
		printf("  hat    ofs %3lu  %s\n", (unsigned long)o->dwOfs,
		    o->tszName);
	}

	return DIENUM_CONTINUE;
}

static BOOL CALLBACK
on_effect(const DIEFFECTINFOA *e, void *ctx)
{
	(void)ctx;
	printf("  effect %08lx  %s\n", (unsigned long)e->guid.Data1,
	    e->tszName);
	return DIENUM_CONTINUE;
}

static BOOL CALLBACK
on_device(const DIDEVICEINSTANCEA *inst, void *ctx)
{
	GUID *out = ctx;

	/* The wheel's product GUID carries its USB ids in the first word. */
	if (inst->guidProduct.Data1 ==
	    (DWORD)((T150_PID_FIRMWARE << 16) | T150_VID)) {
		*out = inst->guidInstance;
		return DIENUM_STOP;
	}

	return DIENUM_CONTINUE;
}

static LONG
axis_value(const DIJOYSTATE2 *st, DWORD ofs)
{
	return *(const LONG *)((const char *)st + ofs);
}

static int
poll_state(DIJOYSTATE2 *st)
{
	if (FAILED(IDirectInputDevice8_Poll(dev)))
		(void)IDirectInputDevice8_Acquire(dev);

	return SUCCEEDED(IDirectInputDevice8_GetDeviceState(dev,
	    sizeof(*st), st)) ? 0 : -1;
}

static void
dump_caps(void)
{
	DIDEVCAPS caps;
	DIDEVICEINSTANCEA inst;

	memset(&caps, 0, sizeof(caps));
	caps.dwSize = sizeof(caps);
	if (SUCCEEDED(IDirectInputDevice8_GetCapabilities(dev, &caps))) {
		printf("device type 0x%08lx   axes %lu  buttons %lu  povs %lu\n",
		    (unsigned long)caps.dwDevType, (unsigned long)caps.dwAxes,
		    (unsigned long)caps.dwButtons, (unsigned long)caps.dwPOVs);
		printf("force feedback %s\n",
		    (caps.dwFlags & DIDC_FORCEFEEDBACK) ?
		    "claimed (DIDC_FORCEFEEDBACK)" : "NOT claimed");
	}

	memset(&inst, 0, sizeof(inst));
	inst.dwSize = sizeof(inst);
	if (SUCCEEDED(IDirectInputDevice8_GetDeviceInfo(dev, &inst)))
		printf("product     %s\n", inst.tszProductName);
}

/*
 * Watch until one axis or button moves far enough to be a deliberate press,
 * then report what it was. Returns 0 when something moved.
 */
static int
watch_one(const char *what)
{
	DIJOYSTATE2 rest, now;
	int i, waited;

	printf("\n%s, then hold it. Watching %d seconds.\n", what,
	    WATCH_MS / 1000);
	fflush(stdout);

	Sleep(500);
	if (poll_state(&rest) != 0) {
		printf("  cannot read the device\n");
		return -1;
	}

	for (waited = 0; waited < WATCH_MS; waited += POLL_MS) {
		Sleep(POLL_MS);
		if (poll_state(&now) != 0)
			continue;

		for (i = 0; i < naxes; i++) {
			LONG a = axis_value(&rest, axes[i].ofs);
			LONG b = axis_value(&now, axes[i].ofs);
			LONG d = b > a ? b - a : a - b;

			if (d < MOVE_MIN)
				continue;

			printf("  %s: axis %s, ofs %lu, axle %u\n", what,
			    axes[i].name, (unsigned long)axes[i].ofs,
			    axle_number(axes[i].ofs));
			printf("      released %ld, pressed %ld, so pressing "
			    "makes it %s\n", (long)a, (long)b,
			    b > a ? "rise" : "FALL");
			if (b < a)
				printf("      that is inverted for a pedal: a "
				    "game reading it raw sees full at rest\n");
			return 0;
		}

		for (i = 0; i < nbuttons; i++) {
			BYTE was = rest.rgbButtons[buttons[i].ofs -
			    DIJOFS_BUTTON0];
			BYTE is = now.rgbButtons[buttons[i].ofs -
			    DIJOFS_BUTTON0];

			if (was == is || !(is & 0x80))
				continue;

			printf("  %s: button %lu (%s)\n", what,
			    (unsigned long)(buttons[i].ofs - DIJOFS_BUTTON0),
			    buttons[i].name);
			return 0;
		}
	}

	printf("  nothing moved. If that was a pedal, it may be arriving as a "
	    "button, or not at all.\n");

	return -1;
}

static void
identify(void)
{
	static const char *prompts[] = {
		"Turn the wheel to the right",
		"Press the accelerator",
		"Press the brake",
		"Press the left paddle",
		"Press the right paddle",
	};
	size_t i;

	printf("\n--- identification ---\n");
	printf("One control at a time, and let go between them.\n");

	for (i = 0; i < sizeof(prompts) / sizeof(prompts[0]); i++)
		(void)watch_one(prompts[i]);
}

/*
 * Create a real effect and play it. This is the whole point of the tool: it
 * exercises the proxy, the daemon and the wheel with no game involved, so a
 * game that asks wrongly can be told apart from a chain that does not
 * deliver.
 */
static int
ffb_test(void)
{
	DIEFFECT ef;
	DICONSTANTFORCE cf;
	DICONDITION cond;
	DWORD axis = DIJOFS_X;
	LONG dir = 0;
	IDirectInputEffect *e = NULL;
	HRESULT hr;

	printf("\n--- force feedback self test ---\n");
	printf("HOLD THE WHEEL. Two effects, a few seconds each.\n");

	memset(&cf, 0, sizeof(cf));
	cf.lMagnitude = 8000;		/* 80 percent, one direction */

	memset(&ef, 0, sizeof(ef));
	ef.dwSize = sizeof(ef);
	ef.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
	ef.dwDuration = 3 * DI_SECONDS;
	ef.dwGain = DI_FFNOMINALMAX;
	ef.dwTriggerButton = DIEB_NOTRIGGER;
	ef.cAxes = 1;
	ef.rgdwAxes = &axis;
	ef.rglDirection = &dir;
	ef.cbTypeSpecificParams = sizeof(cf);
	ef.lpvTypeSpecificParams = &cf;

	hr = IDirectInputDevice8_CreateEffect(dev, &GUID_ConstantForce, &ef,
	    &e, NULL);
	if (FAILED(hr) || e == NULL) {
		printf("  constant force: CreateEffect failed, 0x%08lx\n",
		    (unsigned long)hr);
		printf("  the proxy is not accepting effects; nothing below "
		    "this can work\n");
		return -1;
	}

	hr = IDirectInputEffect_Start(e, 1, 0);
	printf("  constant force: Start %s (0x%08lx)\n",
	    SUCCEEDED(hr) ? "accepted" : "FAILED", (unsigned long)hr);
	printf("  ---> did the wheel pull to one side just now?\n");
	Sleep(3000);
	(void)IDirectInputEffect_Stop(e);
	IDirectInputEffect_Release(e);
	e = NULL;

	memset(&cond, 0, sizeof(cond));
	cond.lPositiveCoefficient = 8000;
	cond.lNegativeCoefficient = 8000;
	cond.dwPositiveSaturation = 10000;
	cond.dwNegativeSaturation = 10000;

	ef.cbTypeSpecificParams = sizeof(cond);
	ef.lpvTypeSpecificParams = &cond;
	ef.dwDuration = 5 * DI_SECONDS;

	hr = IDirectInputDevice8_CreateEffect(dev, &GUID_Damper, &ef, &e, NULL);
	if (FAILED(hr) || e == NULL) {
		printf("  damper: CreateEffect failed, 0x%08lx\n",
		    (unsigned long)hr);
		return -1;
	}

	hr = IDirectInputEffect_Start(e, 1, 0);
	printf("  damper: Start %s (0x%08lx)\n",
	    SUCCEEDED(hr) ? "accepted" : "FAILED", (unsigned long)hr);
	printf("  ---> turn the wheel now. Is it heavier than before?\n");
	Sleep(5000);
	(void)IDirectInputEffect_Stop(e);
	IDirectInputEffect_Release(e);

	printf("\n  Both effects were accepted. Whether the wheel moved is\n"
	    "  the answer, and only you can see it.\n");

	return 0;
}

/*
 * Two flags, parsed where they are used. This is the one tool here that
 * does not use getopt, so tests/usage_check.c does not cover it; it does not
 * need to, because there is no separate option string that can drift out of
 * step with the usage text the way probe_intr's -H once did.
 */
static void
usage(void)
{
	fprintf(stderr,
	    "usage: probe_dinput [-i] [-f]\n"
	    "\n"
	    "  no flags   dump the device and every object it declares\n"
	    "  -i         then ask for each control one at a time\n"
	    "  -f         then create a real effect and play it\n"
	    "\n"
	    "Run it in the bottle. With the proxy installed it measures the\n"
	    "proxy; add --dll dinput8=b to the wine command to measure\n"
	    "CrossOver's builtin instead, and compare.\n");
	exit(2);
}

int
main(int argc, char *argv[])
{
	GUID inst;
	HRESULT hr;
	int i, want_id = 0, want_ff = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-i") == 0)
			want_id = 1;
		else if (strcmp(argv[i], "-f") == 0)
			want_ff = 1;
		else
			usage();
	}

	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
		fprintf(stderr, "CoInitializeEx failed\n");
		return 1;
	}

	hr = DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION,
	    &IID_IDirectInput8A, (void **)&di, NULL);
	if (FAILED(hr) || di == NULL) {
		fprintf(stderr, "DirectInput8Create failed, 0x%08lx\n",
		    (unsigned long)hr);
		return 1;
	}

	memset(&inst, 0, sizeof(inst));
	(void)IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, on_device,
	    &inst, DIEDFL_ATTACHEDONLY);
	if (IsEqualGUID(&inst, &GUID_NULL)) {
		fprintf(stderr, "no T150 found. Is the wheel in firmware mode "
		    "and does the bottle see it at all?\n");
		return 1;
	}

	hr = IDirectInput8_CreateDevice(di, &inst, &dev, NULL);
	if (FAILED(hr) || dev == NULL) {
		fprintf(stderr, "CreateDevice failed, 0x%08lx\n",
		    (unsigned long)hr);
		return 1;
	}

	hr = IDirectInputDevice8_SetDataFormat(dev, &c_dfDIJoystick2);
	if (FAILED(hr))
		printf("SetDataFormat failed, 0x%08lx\n", (unsigned long)hr);

	/*
	 * Force feedback needs an exclusive acquisition, and exclusive needs a
	 * window. A console program has one only when it was started from a
	 * terminal, so report what happened rather than pressing on silently:
	 * a -f run that fails here fails for a reason that has nothing to do
	 * with the wheel.
	 */
	hr = IDirectInputDevice8_SetCooperativeLevel(dev, GetConsoleWindow(),
	    DISCL_EXCLUSIVE | DISCL_BACKGROUND);
	if (FAILED(hr))
		printf("SetCooperativeLevel(exclusive) failed, 0x%08lx. Force "
		    "feedback will not work; run this from a terminal.\n",
		    (unsigned long)hr);

	hr = IDirectInputDevice8_Acquire(dev);
	if (FAILED(hr))
		printf("Acquire failed, 0x%08lx\n", (unsigned long)hr);

	dump_caps();
	printf("\nobjects:\n");
	(void)IDirectInputDevice8_EnumObjects(dev, on_object, NULL,
	    DIDFT_ALL);
	printf("\neffects the device says it supports:\n");
	(void)IDirectInputDevice8_EnumEffects(dev, on_effect, NULL, DIEFT_ALL);

	printf("\n%d axis(es), %d button(s)\n", naxes, nbuttons);
	for (i = 0; i < naxes; i++)
		if (axes[i].is_ff)
			break;
	if (i == naxes)
		printf("no axis is marked as a force feedback actuator\n");

	if (want_id)
		identify();
	if (want_ff)
		(void)ffb_test();

	(void)IDirectInputDevice8_Unacquire(dev);
	IDirectInputDevice8_Release(dev);
	IDirectInput8_Release(di);
	CoUninitialize();

	return 0;
}
