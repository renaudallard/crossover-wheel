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
 *   -i               work every control by name, one at a time, confirm each
 *                    identification, and end with a table of the whole wheel
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
 * The controls this wheel has, in the order it is natural to work through
 * them, named the way the person holding it would name them. The T150's
 * thirteen buttons were identified on hardware and are recorded in
 * RESEARCH.md A37; asking for "button 7" instead would put the burden of
 * the mapping back on whoever is running this, which is the thing the tool
 * exists to remove.
 */
struct control {
	const char	*prompt;
	int		 is_axis;
};

static const struct control controls[] = {
	{ "Turn the wheel fully right, then let it centre",	1 },
	{ "Press the accelerator fully, then release it",	1 },
	{ "Press the brake fully, then release it",		1 },
	{ "Press the clutch fully if you have one, then release", 1 },
	{ "Press the LEFT paddle",				0 },
	{ "Press the RIGHT paddle",				0 },
	{ "Press TRIANGLE",					0 },
	{ "Press SQUARE",					0 },
	{ "Press CIRCLE",					0 },
	{ "Press CROSS",					0 },
	{ "Press SHARE or SELECT",				0 },
	{ "Press OPTIONS or START",				0 },
	{ "Press R2",						0 },
	{ "Press L2",						0 },
	{ "Press L3",						0 },
	{ "Press R3",						0 },
	{ "Press the PS button",				0 },
	{ "Push the hat UP",					0 },
	{ "Push the hat DOWN",					0 },
	{ "Push the hat LEFT",					0 },
	{ "Push the hat RIGHT",					0 }
};

/* What one control turned out to be, for the table at the end. */
struct result {
	const char	*prompt;
	char		 found[80];
	char		 detail[120];
	int		 seen;
	int		 disputed;
	int		 skipped;
};

static struct result results[sizeof(controls) / sizeof(controls[0])];

/*
 * Ask, and take y, n or s. Enter means yes, because the common case is the
 * tool being right and a person working through twenty controls should not
 * have to type for each one.
 */
static int
confirm(void)
{
	int c, first;

	printf("      correct? [Y/n/s to skip] ");
	fflush(stdout);

	first = c = getchar();
	while (c != '\n' && c != EOF)
		c = getchar();

	if (first == 'n' || first == 'N')
		return -1;
	if (first == 's' || first == 'S')
		return 1;

	return 0;
}

/*
 * Watch one control through a whole press and release. Axes are sampled the
 * entire time rather than at the first movement, so the record is the real
 * travel: where it rests, how far it goes, and which way. A pedal that rests
 * at its maximum is then a number rather than an impression, and a pedal
 * that only reaches half its range shows up as one too.
 */
static void
watch_control(const struct control *c, struct result *r)
{
	DIJOYSTATE2 rest, now;
	LONG lo[16], hi[16];
	int i, waited, best = -1, btn = -1, pov = -1;

	r->prompt = c->prompt;

	printf("\n%s.\n  Watching %d seconds.\n", c->prompt, WATCH_MS / 1000);
	fflush(stdout);

	Sleep(400);
	if (poll_state(&rest) != 0) {
		(void)snprintf(r->found, sizeof(r->found), "device unreadable");
		return;
	}

	for (i = 0; i < naxes; i++)
		lo[i] = hi[i] = axis_value(&rest, axes[i].ofs);

	for (waited = 0; waited < WATCH_MS; waited += POLL_MS) {
		Sleep(POLL_MS);
		if (poll_state(&now) != 0)
			continue;

		for (i = 0; i < naxes; i++) {
			LONG v = axis_value(&now, axes[i].ofs);

			if (v < lo[i])
				lo[i] = v;
			if (v > hi[i])
				hi[i] = v;
		}
		for (i = 0; i < nbuttons && btn < 0; i++) {
			int n = (int)(buttons[i].ofs - DIJOFS_BUTTON0);

			if (now.rgbButtons[n] & 0x80)
				btn = n;
		}
		if (pov < 0 && LOWORD(now.rgdwPOV[0]) != 0xffff)
			pov = (int)now.rgdwPOV[0];
	}

	/* The axis that travelled furthest is the one that was worked. */
	for (i = 0; i < naxes; i++)
		if (hi[i] - lo[i] >= MOVE_MIN &&
		    (best < 0 || hi[i] - lo[i] > hi[best] - lo[best]))
			best = i;

	if (c->is_axis && best >= 0) {
		LONG at_rest = axis_value(&rest, axes[best].ofs);
		int inverted = at_rest > (lo[best] + hi[best]) / 2;

		r->seen = 1;
		(void)snprintf(r->found, sizeof(r->found),
		    "axis %s, ofs %lu, axle %u", axes[best].name,
		    (unsigned long)axes[best].ofs,
		    axle_number(axes[best].ofs));
		(void)snprintf(r->detail, sizeof(r->detail),
		    "rest %ld, range %ld..%ld, pressing makes it %s%s",
		    (long)at_rest, (long)lo[best], (long)hi[best],
		    inverted ? "FALL" : "rise",
		    inverted ? "  [inverted]" : "");
		printf("  -> %s\n     %s\n", r->found, r->detail);
	} else if (!c->is_axis && btn >= 0) {
		r->seen = 1;
		(void)snprintf(r->found, sizeof(r->found), "button %d", btn);
		(void)snprintf(r->detail, sizeof(r->detail), "%s",
		    buttons[btn].name);
		printf("  -> %s (%s)\n", r->found, r->detail);
	} else if (!c->is_axis && pov >= 0) {
		r->seen = 1;
		(void)snprintf(r->found, sizeof(r->found), "hat %d", pov / 100);
		(void)snprintf(r->detail, sizeof(r->detail),
		    "hat switch, %d hundredths of a degree", pov);
		printf("  -> %s\n", r->found);
	} else if (best >= 0) {
		/* Asked for a button and an axis moved, or the reverse. */
		r->seen = 1;
		(void)snprintf(r->found, sizeof(r->found),
		    "axis %s, ofs %lu, axle %u", axes[best].name,
		    (unsigned long)axes[best].ofs,
		    axle_number(axes[best].ofs));
		(void)snprintf(r->detail, sizeof(r->detail),
		    "an AXIS moved where a button was expected");
		printf("  -> %s\n     %s\n", r->found, r->detail);
	} else if (btn >= 0) {
		r->seen = 1;
		(void)snprintf(r->found, sizeof(r->found), "button %d", btn);
		(void)snprintf(r->detail, sizeof(r->detail),
		    "a BUTTON moved where an axis was expected");
		printf("  -> %s\n     %s\n", r->found, r->detail);
	} else {
		(void)snprintf(r->found, sizeof(r->found), "nothing moved");
		printf("  -> nothing moved\n");
	}
}

static void
identify(void)
{
	size_t i, n = sizeof(controls) / sizeof(controls[0]);

	printf("\n--- identification ---\n");
	printf("One control at a time. Let go of everything else, and press\n"
	    "each one fully so its whole travel is recorded.\n");

	for (i = 0; i < n; i++) {
		int c;

		watch_control(&controls[i], &results[i]);

		c = confirm();
		if (c < 0)
			results[i].disputed = 1;
		else if (c > 0)
			results[i].skipped = 1;
	}

	printf("\n--- what this wheel is, as DirectInput sees it ---\n\n");
	printf("%-44s %-34s %s\n", "control", "is", "notes");
	for (i = 0; i < n; i++) {
		const struct result *r = &results[i];

		if (r->skipped)
			continue;
		printf("%-44s %-34s %s%s\n", controls[i].prompt,
		    r->seen ? r->found : "NOT SEEN", r->detail,
		    r->disputed ? "  [DISPUTED]" : "");
	}
	printf("\nAnything marked DISPUTED is where the tool named a control "
	    "and you said it was wrong.\nThat is the interesting line.\n");
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
	    "  -i         then work every control by name, one at a time,\n"
	    "             confirming each, and print a table at the end\n"
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
