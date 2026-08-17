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
 * Four things it does:
 *
 *   the dump         what the device and every one of its objects declare
 *   -i               work every control by name, one at a time, confirm each
 *                    identification, and end with a table of the whole wheel
 *   -F               run a fixed sequence of effects with nobody watching,
 *                    for a test rig: what it produces is the daemon's packet
 *                    log rather than anything printed here
 *   -f               create two real force feedback effects, ask after
 *                    each whether the wheel moved, and write the answers
 *                    down, with no game involved at all
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

#include <stdarg.h>

/*
 * Everything this prints goes to a file as well as to the screen, and it
 * does so without being asked. A run of this tool is evidence, and evidence
 * that lives only in a terminal has been lost to a closed window, a
 * truncated scrollback and an interrupted tail more than once in this
 * project's history. The default sits on the host filesystem through Wine's
 * Z: mapping, so it can be read from macOS without hunting through the
 * bottle; -o puts it anywhere else.
 *
 * One name per mode, because the documented way to use this tool is three
 * runs in a row and a single name meant each one destroyed the one before
 * it. Test 27 survived only because that tester copied the file by hand
 * between runs, which nothing asked them to do.
 */
#define LOG_DUMP	"Z:\\tmp\\probe_dinput-dump.log"
#define LOG_CONTROLS	"Z:\\tmp\\probe_dinput-controls.log"
#define LOG_FFB		"Z:\\tmp\\probe_dinput-ffb.log"
#define LOG_BOTH	"Z:\\tmp\\probe_dinput-controls-ffb.log"
#define LOG_SEQ		"Z:\\tmp\\probe_dinput-sequence.log"

static const char *
default_log(int want_id, int want_ff, int want_seq)
{
	if (want_seq)
		return LOG_SEQ;
	if (want_id && want_ff)
		return LOG_BOTH;
	if (want_ff)
		return LOG_FFB;
	if (want_id)
		return LOG_CONTROLS;

	return LOG_DUMP;
}

static FILE *logfp;

static void
out(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)vprintf(fmt, ap);
	va_end(ap);
	(void)fflush(stdout);

	if (logfp == NULL)
		return;

	va_start(ap, fmt);
	(void)vfprintf(logfp, fmt, ap);
	va_end(ap);
	(void)fflush(logfp);
}

/* How long to watch for movement when asking for one control. */
#define WATCH_MS	6000
#define POLL_MS		20

/* A press has to move a good part of the range to count, not just jitter. */
#define MOVE_MIN	2000

/*
 * Two different offsets belong to every object, and confusing them is the
 * mistake this tool shipped with.
 *
 * EnumObjects reports dwOfs in the DEVICE's own format: Wine hands the
 * instance straight to the callback without remapping it, so for this wheel
 * the axes come back at 0, 4, 8, 12 and the buttons at small offsets just
 * past them. That number is worth printing, because dividing it by four
 * gives the order DirectInput enumerates the axes in, which is the number a
 * game's configuration file tends to record.
 *
 * It is NOT an offset into the state struct. SetDataFormat(c_dfDIJoystick2)
 * decides that, and there the axes sit at DIJOFS_X, DIJOFS_Y and so on with
 * the buttons at DIJOFS_BUTTON0 and up. Reading the state at the device
 * offset reads the wrong field, and subtracting DIJOFS_BUTTON0 from a device
 * offset goes negative and indexes off the front of the array.
 *
 * So both are kept, and only state_ofs is ever used to read anything.
 */
#define NO_STATE_OFS	((DWORD)~0u)

struct axis_seen {
	DWORD	dev_ofs;	/* what EnumObjects reported */
	DWORD	state_ofs;	/* where it sits in DIJOYSTATE2 */
	DWORD	type;		/* DIDFT_*, identifies the object */
	char	name[64];
	int	is_ff;
};

struct button_seen {
	DWORD	dev_ofs;
	DWORD	state_ofs;
	DWORD	type;
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

		out("  axis   ofs %3lu (axle %u)  %-6s  range %ld..%ld  "
		    "usage %02x/%02x  %s%s\n", (unsigned long)o->dwOfs,
		    axle_number(o->dwOfs), guid_axis_name(&o->guidType),
		    (long)r.lMin, (long)r.lMax, o->wUsagePage, o->wUsage,
		    o->tszName, ff ? "  [force feedback actuator]" : "");

		if (naxes < (int)(sizeof(axes) / sizeof(axes[0]))) {
			axes[naxes].dev_ofs = o->dwOfs;
			axes[naxes].state_ofs = NO_STATE_OFS;
			axes[naxes].type = o->dwType;
			axes[naxes].is_ff = ff;
			(void)snprintf(axes[naxes].name,
			    sizeof(axes[naxes].name), "%s",
			    guid_axis_name(&o->guidType));
			naxes++;
		}
	} else if (o->dwType & DIDFT_BUTTON) {
		out("  button ofs %3lu (instance %lu)  %s\n",
		    (unsigned long)o->dwOfs,
		    (unsigned long)DIDFT_GETINSTANCE(o->dwType), o->tszName);

		if (nbuttons < (int)(sizeof(buttons) / sizeof(buttons[0]))) {
			buttons[nbuttons].dev_ofs = o->dwOfs;
			buttons[nbuttons].state_ofs = NO_STATE_OFS;
			buttons[nbuttons].type = o->dwType;
			(void)snprintf(buttons[nbuttons].name,
			    sizeof(buttons[nbuttons].name), "%s", o->tszName);
			nbuttons++;
		}
	} else if (o->dwType & DIDFT_POV) {
		out("  hat    ofs %3lu  %s\n", (unsigned long)o->dwOfs,
		    o->tszName);
	}

	return DIENUM_CONTINUE;
}

static BOOL CALLBACK
on_effect(const DIEFFECTINFOA *e, void *ctx)
{
	(void)ctx;
	out("  effect %08lx  %s\n", (unsigned long)e->guid.Data1,
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

/*
 * Work out where each object landed in DIJOYSTATE2.
 *
 * The state layout is fixed and public, so rather than guess from an object
 * where it sits, this walks the layout and asks DirectInput what is in each
 * slot. GetObjectInfo with DIPH_BYOFFSET takes exactly that question, and
 * answers DIERR_NOTFOUND for a slot the wheel does not fill. What comes back
 * is matched to what EnumObjects already reported by dwType, which names an
 * object uniquely.
 *
 * Every offset that reaches axis_value or rgbButtons therefore came from
 * this table rather than from the device, and cannot be out of range.
 */
static const DWORD joy2_axis_ofs[] = {
	DIJOFS_X, DIJOFS_Y, DIJOFS_Z,
	DIJOFS_RX, DIJOFS_RY, DIJOFS_RZ,
	DIJOFS_SLIDER(0), DIJOFS_SLIDER(1)
};

static void
resolve_state_offsets(void)
{
	DIDEVICEOBJECTINSTANCEA o;
	size_t s;
	int i;

	for (s = 0; s < sizeof(joy2_axis_ofs) / sizeof(joy2_axis_ofs[0]); s++) {
		memset(&o, 0, sizeof(o));
		o.dwSize = sizeof(o);
		if (FAILED(IDirectInputDevice8_GetObjectInfo(dev, &o,
		    joy2_axis_ofs[s], DIPH_BYOFFSET)))
			continue;

		for (i = 0; i < naxes; i++)
			if (axes[i].type == o.dwType)
				axes[i].state_ofs = joy2_axis_ofs[s];
	}

	for (s = 0; s < sizeof(buttons) / sizeof(buttons[0]); s++) {
		memset(&o, 0, sizeof(o));
		o.dwSize = sizeof(o);
		if (FAILED(IDirectInputDevice8_GetObjectInfo(dev, &o,
		    DIJOFS_BUTTON((int)s), DIPH_BYOFFSET)))
			continue;

		for (i = 0; i < nbuttons; i++)
			if (buttons[i].type == o.dwType)
				buttons[i].state_ofs = DIJOFS_BUTTON((int)s);
	}
}

static int
poll_state(DIJOYSTATE2 *st)
{
	if (FAILED(IDirectInputDevice8_Poll(dev)))
		(void)IDirectInputDevice8_Acquire(dev);

	return SUCCEEDED(IDirectInputDevice8_GetDeviceState(dev,
	    sizeof(*st), st)) ? 0 : -1;
}

/* Whether the device claimed force feedback, for the actuator check later. */
static int ff_claimed;

/*
 * Which dinput8 answered all this.
 *
 * The proxy says so itself, but through OutputDebugString and stderr, so a
 * log file carries no trace of it and three different situations produce a
 * byte for byte identical run: the proxy absent, the proxy present with no
 * daemon, and the proxy deliberately bypassed with --dll dinput8=b. A run
 * sent in for reading has to answer that on its own.
 *
 * The proxy is the only thing that loads dinput8_orig.dll, so the presence
 * of that module in this process is the whole question.
 */
static void
dump_chain(void)
{
	if (GetModuleHandleA("dinput8_orig.dll") != NULL)
		out("dinput8     the proxy, with CrossOver's builtin chained "
		    "behind it\n");
	else
		out("dinput8     CrossOver's own, no proxy in the chain\n");
}

static void
dump_caps(void)
{
	DIDEVCAPS caps;
	DIDEVICEINSTANCEA inst;

	memset(&caps, 0, sizeof(caps));
	caps.dwSize = sizeof(caps);
	if (SUCCEEDED(IDirectInputDevice8_GetCapabilities(dev, &caps))) {
		ff_claimed = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
		out("device type 0x%08lx   axes %lu  buttons %lu  povs %lu\n",
		    (unsigned long)caps.dwDevType, (unsigned long)caps.dwAxes,
		    (unsigned long)caps.dwButtons, (unsigned long)caps.dwPOVs);
		out("force feedback %s\n", ff_claimed ?
		    "claimed (DIDC_FORCEFEEDBACK)" : "NOT claimed");
	}

	memset(&inst, 0, sizeof(inst));
	inst.dwSize = sizeof(inst);
	if (SUCCEEDED(IDirectInputDevice8_GetDeviceInfo(dev, &inst)))
		out("product     %s\n", inst.tszProductName);
}

/*
 * Where answers come from. A run from a terminal answers on stdin. A run
 * started from the CrossOver GUI has nothing behind stdin at all: Wine
 * creates no console for it, and getchar() answers EOF without a person
 * anywhere near the question. This tool shipped taking that EOF for a yes,
 * twenty one times in a row, and wrote a table that looked confirmed by a
 * person who was never asked. So EOF is never an answer here. The first
 * one moves every question into a message box, which a bottle can always
 * draw; and when even the box cannot appear, which is what a headless CI
 * run looks like, the answer is recorded as never given rather than
 * invented.
 */
static int ask_by_box;
static int nobody_to_ask;

static int
ask_box(const char *text, UINT type)
{
	return MessageBoxA(NULL, text, "probe_dinput",
	    type | MB_SETFOREGROUND);
}

static void
stop_asking(void)
{
	nobody_to_ask = 1;
	out("nothing can show a question; answers from here on are "
	    "recorded as never given\n");
}

/*
 * Wait until the person says they are ready. This is also the moment the
 * tool learns whether stdin can answer at all, before the first watch
 * rather than six seconds into it.
 *
 * Returns 1 when a person was actually reached, 0 when there is nobody there.
 * The identification walk can carry on unattended and record every row as
 * never answered, which is the point of it. Driving the wheel cannot: the
 * force feedback test asks somebody to hold the wheel and then pulls it at
 * eighty percent, and doing that after establishing that no warning can reach
 * anyone is the one thing here that could hurt.
 */
static int
ready(const char *what)
{
	char text[300];
	int c;

	out("\n%s\n", what);

	if (!ask_by_box) {
		out("Press Enter to start.\n");
		c = getchar();
		while (c != '\n' && c != EOF)
			c = getchar();
		if (c == '\n')
			return 1;
		ask_by_box = 1;
		out("stdin cannot answer; asking on screen instead\n");
	}

	if (nobody_to_ask)
		return 0;

	(void)snprintf(text, sizeof(text), "%s\n\nOK starts.", what);
	if (ask_box(text, MB_OK) <= 0) {
		stop_asking();
		return 0;
	}

	return 1;
}

/*
 * Ask a yes or no question about what the wheel just did, and write the
 * answer down. The observation only the person holding the wheel can make
 * is the measurement, and a question that is asked but not recorded might
 * as well not have been asked. It insists on y or n where the confirms
 * take a bare Enter: this answer is the measurement, so nothing stands in
 * for it.
 */
static void
answer(const char *question)
{
	char text[300];
	const char *said = NULL;
	int c, first;

	out("  ---> %s?\n", question);

	while (!ask_by_box && said == NULL) {
		out("       [y/n] ");
		first = c = getchar();
		while (c != '\n' && c != EOF)
			c = getchar();
		if (first == EOF) {
			ask_by_box = 1;
			out("\nstdin cannot answer; asking on screen "
			    "instead\n");
		} else if (first == 'y' || first == 'Y') {
			said = "yes";
		} else if (first == 'n' || first == 'N') {
			said = "no";
		}
	}

	if (said == NULL && !nobody_to_ask) {
		(void)snprintf(text, sizeof(text), "%s?", question);
		switch (ask_box(text, MB_YESNO)) {
		case IDYES:
			said = "yes";
			break;
		case IDNO:
			said = "no";
			break;
		default:
			stop_asking();
		}
	}

	out("       answer: %s\n", said != NULL ? said : "never given");
}

/*
 * Tell the person a failure happened. out() already put it on stdout and
 * in the log, but a run whose questions travel by box has nobody looking
 * at either, and a person told to hold the wheel deserves to hear why
 * nothing is coming.
 */
static void
tell_box(const char *text)
{
	if (ask_by_box && !nobody_to_ask && ask_box(text, MB_OK) <= 0)
		stop_asking();
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
	int		 unanswered;
};

static struct result results[sizeof(controls) / sizeof(controls[0])];

/*
 * Ask, and take y, n or s. Enter means yes, because the common case is the
 * tool being right and a person working through twenty controls should not
 * have to type for each one. When stdin cannot answer, the same question
 * goes out as a message box instead.
 * Returns 0 for yes, -1 for no, 1 for skip, 2 for nobody answered.
 *
 * The answer is written down. A terminal echoes what was typed to the
 * screen and nowhere else, so the log the tester is asked to send held the
 * question and not the reply, and the only surviving trace of twenty one
 * answers was one absent row. A box run left even less.
 */
static int
said(int answer)
{
	static const char *const name[] = { "yes", "no", "skip",
	    "never given" };

	out("      answer: %s\n", name[answer < 0 ? 1 : answer == 0 ? 0 :
	    answer == 1 ? 2 : 3]);

	return answer;
}

static int
confirm(const struct result *r)
{
	char text[512];
	int c, first;

	if (!ask_by_box) {
		out("      correct? [Y/n/s to skip] ");

		first = c = getchar();
		while (c != '\n' && c != EOF)
			c = getchar();

		if (first != EOF) {
			if (first == 'n' || first == 'N')
				return said(-1);
			if (first == 's' || first == 'S')
				return said(1);
			return said(0);
		}
		ask_by_box = 1;
		out("\nstdin cannot answer; asking on screen instead\n");
	}

	if (!nobody_to_ask) {
		(void)snprintf(text, sizeof(text),
		    "%s.\n\n%s\n%s\n\n"
		    "Yes means correct, No means wrong, Cancel skips it.",
		    r->prompt, r->found, r->detail);
		switch (ask_box(text, MB_YESNOCANCEL)) {
		case IDYES:
			return said(0);
		case IDNO:
			return said(-1);
		case IDCANCEL:
			return said(1);
		default:
			stop_asking();
		}
	}

	return said(2);
}

/*
 * Where each control ended up in the state struct, which is a different
 * number from the enumeration offset above and the one that decides which
 * value a game reads for it. Printing both is the whole point: when they
 * disagree in a surprising way, this is the line that says so.
 */
static void
dump_state_map(void)
{
	int i;

	out("\nwhere each control sits in the state DirectInput fills in:\n");

	for (i = 0; i < naxes; i++) {
		if (axes[i].state_ofs == NO_STATE_OFS) {
			out("  axis   %-6s NOT READABLE, no slot in the "
			    "state\n", axes[i].name);
			continue;
		}
		out("  axis   %-6s state offset %lu\n", axes[i].name,
		    (unsigned long)axes[i].state_ofs);
	}

	for (i = 0; i < nbuttons; i++) {
		if (buttons[i].state_ofs == NO_STATE_OFS) {
			out("  button %-2d     NOT READABLE, no slot in the "
			    "state\n", i);
			continue;
		}
		out("  button %-2d     rgbButtons[%lu]\n", i,
		    (unsigned long)(buttons[i].state_ofs - DIJOFS_BUTTON0));
	}
}

/*
 * How an axis is named in the table at the end. The axle number comes from
 * the enumeration offset rather than the state offset, because that is the
 * one that matches what a game writes into its configuration.
 */
static void
describe_axis(char *buf, size_t len, int i)
{
	(void)snprintf(buf, len, "axis %s, ofs %lu, axle %u", axes[i].name,
	    (unsigned long)axes[i].dev_ofs, axle_number(axes[i].dev_ofs));
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

	out("\n%s.\n  Watching %d seconds.\n", c->prompt, WATCH_MS / 1000);
	fflush(stdout);

	Sleep(400);
	if (poll_state(&rest) != 0) {
		(void)snprintf(r->found, sizeof(r->found), "device unreadable");
		return;
	}

	for (i = 0; i < naxes; i++)
		lo[i] = hi[i] = axes[i].state_ofs == NO_STATE_OFS ? 0 :
		    axis_value(&rest, axes[i].state_ofs);

	for (waited = 0; waited < WATCH_MS; waited += POLL_MS) {
		Sleep(POLL_MS);
		if (poll_state(&now) != 0)
			continue;

		for (i = 0; i < naxes; i++) {
			LONG v;

			if (axes[i].state_ofs == NO_STATE_OFS)
				continue;
			v = axis_value(&now, axes[i].state_ofs);

			if (v < lo[i])
				lo[i] = v;
			if (v > hi[i])
				hi[i] = v;
		}
		/*
		 * A button counts when it goes down during the window, not
		 * when it is down. The axes were always judged against the
		 * baseline sampled above and these were not, so a button still
		 * held from the previous question, or one the wheel reports
		 * pressed at rest, was attributed to whatever was being asked
		 * about now. The same for the hat.
		 */
		for (i = 0; i < nbuttons && btn < 0; i++) {
			DWORD n;

			if (buttons[i].state_ofs == NO_STATE_OFS)
				continue;
			n = buttons[i].state_ofs - DIJOFS_BUTTON0;

			if ((now.rgbButtons[n] & 0x80) &&
			    !(rest.rgbButtons[n] & 0x80))
				btn = i;
		}
		if (pov < 0 && LOWORD(now.rgdwPOV[0]) != 0xffff &&
		    LOWORD(rest.rgdwPOV[0]) == 0xffff)
			pov = (int)now.rgdwPOV[0];
	}

	/* The axis that travelled furthest is the one that was worked. */
	for (i = 0; i < naxes; i++)
		if (hi[i] - lo[i] >= MOVE_MIN &&
		    (best < 0 || hi[i] - lo[i] > hi[best] - lo[best]))
			best = i;

	if (c->is_axis && best >= 0) {
		LONG at_rest = axis_value(&rest, axes[best].state_ofs);
		int inverted = at_rest > (lo[best] + hi[best]) / 2;

		r->seen = 1;
		describe_axis(r->found, sizeof(r->found), best);
		(void)snprintf(r->detail, sizeof(r->detail),
		    "rest %ld, range %ld..%ld, pressing makes it %s%s",
		    (long)at_rest, (long)lo[best], (long)hi[best],
		    inverted ? "FALL" : "rise",
		    inverted ? "  [inverted]" : "");
		out("  -> %s\n     %s\n", r->found, r->detail);
	} else if (!c->is_axis && btn >= 0) {
		r->seen = 1;
		(void)snprintf(r->found, sizeof(r->found), "button %lu",
		    (unsigned long)(buttons[btn].state_ofs - DIJOFS_BUTTON0));
		(void)snprintf(r->detail, sizeof(r->detail), "%s",
		    buttons[btn].name);
		out("  -> %s (%s)\n", r->found, r->detail);
	} else if (!c->is_axis && pov >= 0) {
		r->seen = 1;
		(void)snprintf(r->found, sizeof(r->found), "hat %d", pov / 100);
		(void)snprintf(r->detail, sizeof(r->detail),
		    "hat switch, %d hundredths of a degree", pov);
		out("  -> %s\n", r->found);
	} else if (best >= 0) {
		/* Asked for a button and an axis moved, or the reverse. */
		r->seen = 1;
		describe_axis(r->found, sizeof(r->found), best);
		(void)snprintf(r->detail, sizeof(r->detail),
		    "an AXIS moved where a button was expected");
		out("  -> %s\n     %s\n", r->found, r->detail);
	} else if (btn >= 0) {
		r->seen = 1;
		(void)snprintf(r->found, sizeof(r->found), "button %lu",
		    (unsigned long)(buttons[btn].state_ofs - DIJOFS_BUTTON0));
		(void)snprintf(r->detail, sizeof(r->detail),
		    "a BUTTON moved where an axis was expected");
		out("  -> %s\n     %s\n", r->found, r->detail);
	} else {
		(void)snprintf(r->found, sizeof(r->found), "nothing moved");
		out("  -> nothing moved\n");
	}
}

static void
identify(void)
{
	char text[256];
	size_t i, n = sizeof(controls) / sizeof(controls[0]);
	unsigned skipped = 0, disputed = 0, unanswered = 0;

	out("\n--- identification ---\n");
	(void)ready("One control at a time. Let go of everything else, and press\n"
	    "each one fully so its whole travel is recorded.");

	for (i = 0; i < n; i++) {
		int c;

		if (ask_by_box && !nobody_to_ask) {
			(void)snprintf(text, sizeof(text),
			    "%s.\n\nOK starts a %d second watch.",
			    controls[i].prompt, WATCH_MS / 1000);
			if (ask_box(text, MB_OK) <= 0)
				stop_asking();
		}

		watch_control(&controls[i], &results[i]);

		c = confirm(&results[i]);
		if (c < 0)
			results[i].disputed = 1;
		else if (c == 1)
			results[i].skipped = 1;
		else if (c == 2)
			results[i].unanswered = 1;
	}

	out("\n--- what this wheel is, as DirectInput sees it ---\n\n");
	out("%-44s %-34s %s\n", "control", "is", "notes");
	for (i = 0; i < n; i++) {
		const struct result *r = &results[i];

		if (r->skipped) {
			skipped++;
			out("%-44s %-34s %s\n", controls[i].prompt, "SKIPPED",
			    "you said this wheel has no such control");
			continue;
		}
		out("%-44s %-34s %s%s%s\n", controls[i].prompt,
		    r->seen ? r->found : "NOT SEEN", r->detail,
		    r->disputed ? "  [DISPUTED]" : "",
		    r->unanswered ? "  [NEVER ANSWERED]" : "");
		if (r->disputed)
			disputed++;
		if (r->unanswered)
			unanswered++;
	}

	/*
	 * A count, because a table is only evidence if the reader can tell it
	 * is complete. A skipped row used to be dropped, so this table had
	 * twenty rows for twenty one controls and nothing said which one was
	 * missing or that anything was.
	 */
	out("\n%u controls, %u skipped, %u disputed, %u never answered\n",
	    (unsigned)n, skipped, disputed, unanswered);
	out("\nAnything marked DISPUTED is where the tool named a control "
	    "and you said it was wrong.\nThat is the interesting line. "
	    "NEVER ANSWERED means no one could be asked, and SKIPPED means "
	    "you\nsaid the wheel has no such control.\n");
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
	char text[200];

	out("\n--- force feedback self test ---\n");
	if (!ready("HOLD THE WHEEL. Two effects, a few seconds each.")) {
		out("  nobody could be warned, so no force was commanded.\n"
		    "  Run this where a person can answer.\n");
		return -1;
	}

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
		out("  constant force: CreateEffect failed, 0x%08lx\n",
		    (unsigned long)hr);
		out("  the proxy is not accepting effects; nothing below "
		    "this can work\n");
		(void)snprintf(text, sizeof(text),
		    "CreateEffect failed, 0x%08lx.\n\nThe proxy is not "
		    "accepting effects; nothing more can run.",
		    (unsigned long)hr);
		tell_box(text);
		return -1;
	}

	hr = IDirectInputEffect_Start(e, 1, 0);
	out("  constant force: Start %s (0x%08lx)\n",
	    SUCCEEDED(hr) ? "accepted" : "FAILED", (unsigned long)hr);
	if (FAILED(hr)) {
		(void)snprintf(text, sizeof(text),
		    "The constant force did not start, 0x%08lx.\n\n"
		    "Answer for what you felt anyway.", (unsigned long)hr);
		tell_box(text);
	}
	Sleep(3000);
	(void)IDirectInputEffect_Stop(e);
	IDirectInputEffect_Release(e);
	e = NULL;
	answer("did the wheel pull to one side just now");

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
		out("  damper: CreateEffect failed, 0x%08lx\n",
		    (unsigned long)hr);
		(void)snprintf(text, sizeof(text),
		    "Damper CreateEffect failed, 0x%08lx. Nothing more "
		    "can run.", (unsigned long)hr);
		tell_box(text);
		return -1;
	}

	(void)ready("Now the damper. Turn the wheel while it runs.");

	hr = IDirectInputEffect_Start(e, 1, 0);
	out("  damper: Start %s (0x%08lx)\n",
	    SUCCEEDED(hr) ? "accepted" : "FAILED", (unsigned long)hr);
	if (FAILED(hr)) {
		(void)snprintf(text, sizeof(text),
		    "The damper did not start, 0x%08lx.\n\n"
		    "Answer for what you felt anyway.", (unsigned long)hr);
		tell_box(text);
	}
	Sleep(5000);
	(void)IDirectInputEffect_Stop(e);
	IDirectInputEffect_Release(e);
	answer("was the wheel heavier to turn while it ran");

	out("\n  The answers above are the measurement, and only the person\n"
	    "  holding the wheel could make it.\n");

	return 0;
}

/*
 * How long the sequence waits between steps.
 *
 * Under the proxy's ASSUME_MS, which is what makes the steps mean anything:
 * that constant is how long the proxy will assume an effect it uploaded is
 * still on the daemon, and a step that waits longer than it is answered by a
 * fresh upload whatever the cache would have decided. The first version of
 * this waited 60 against an ASSUME_MS of 50, so every step took the same path
 * and the sequence tested none of what its comments claimed.
 *
 * Twenty rather than something smaller because the daemon's emitter has a four
 * millisecond floor and a pass has to have happened before the next step asks
 * about it.
 */
#define SEQ_WARM_MS	20

/* How long the ramp runs, so a step can start it again after it has ended. */
#define SEQ_RAMP_MS	300

/*
 * The same path with nobody holding the wheel, driven to a script.
 *
 * Separate from ffb_test above rather than a mode inside it, because that one
 * asks before it moves anything and must go on asking: a prompt that treats
 * end of file as an answer is how an unattended wheel starts pulling at
 * somebody, and the rule in this project is that it never does. This is the
 * other case, a rig where no one can be surprised, and it says so by being a
 * flag of its own.
 *
 * What it is for is the daemon's log rather than anything printed here. Each
 * step is chosen because a specific fault in the proxy's upload cache shows up
 * as a missing packet, and the run is separated into phases by a device gain
 * set to full, which changes nothing and prints one packet the assertions can
 * count from.
 */
static void
seq_mark(void)
{
	DIPROPDWORD g;

	memset(&g, 0, sizeof(g));
	g.diph.dwSize = sizeof(g);
	g.diph.dwHeaderSize = sizeof(g.diph);
	g.diph.dwHow = DIPH_DEVICE;
	g.dwData = DI_FFNOMINALMAX;
	(void)IDirectInputDevice8_SetProperty(dev, DIPROP_FFGAIN, &g.diph);
}

static int
ffb_sequence(void)
{
	DIEFFECT ef;
	DICONSTANTFORCE cf;
	DIRAMPFORCE rf;
	DWORD axis = DIJOFS_X;
	LONG dir = 0;
	IDirectInputEffect *c = NULL, *r = NULL;
	HRESULT hr;

	out("\n--- scripted force feedback sequence ---\n");

	memset(&cf, 0, sizeof(cf));
	cf.lMagnitude = 8000;

	memset(&ef, 0, sizeof(ef));
	ef.dwSize = sizeof(ef);
	ef.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
	ef.dwDuration = INFINITE;
	ef.dwGain = DI_FFNOMINALMAX;
	ef.dwTriggerButton = DIEB_NOTRIGGER;
	ef.cAxes = 1;
	ef.rgdwAxes = &axis;
	ef.rglDirection = &dir;
	ef.cbTypeSpecificParams = sizeof(cf);
	ef.lpvTypeSpecificParams = &cf;

	/* One: a constant, uploaded and started. */
	seq_mark();
	hr = IDirectInputDevice8_CreateEffect(dev, &GUID_ConstantForce, &ef,
	    &c, NULL);
	if (FAILED(hr) || c == NULL) {
		out("  CreateEffect failed, 0x%08lx\n", (unsigned long)hr);
		return -1;
	}
	out("  one: constant created and started\n");
	(void)IDirectInputEffect_Start(c, 1, 0);
	Sleep(SEQ_WARM_MS);

	/*
	 * Two: started again with nothing changed, inside the window, so the
	 * proxy's cache skips the upload. The start still has to arrive: a
	 * skip that swallowed it would leave the wheel with nothing to play.
	 */
	seq_mark();
	out("  two: started again unchanged\n");
	(void)IDirectInputEffect_Start(c, 1, 0);
	Sleep(SEQ_WARM_MS);

	/* Three: the level moves, so the upload must not be skipped. */
	seq_mark();
	cf.lMagnitude = 4000;
	(void)IDirectInputEffect_SetParameters(c, &ef, DIEP_TYPESPECIFICPARAMS);
	out("  three: level moved to 4000\n");
	(void)IDirectInputEffect_Start(c, 1, 0);
	Sleep(SEQ_WARM_MS);

	/*
	 * Four: a ramp, left to run past its own duration and then started
	 * again. Nothing is asserted here. What it is for is the state the
	 * next step needs: a ramp whose upload the proxy has just
	 * acknowledged, so the only thing that can make it upload again is the
	 * rule that a ramp is never skipped.
	 */
	memset(&rf, 0, sizeof(rf));
	rf.lStart = 0;
	rf.lEnd = 10000;
	ef.dwDuration = SEQ_RAMP_MS * 1000;
	ef.cbTypeSpecificParams = sizeof(rf);
	ef.lpvTypeSpecificParams = &rf;

	seq_mark();
	hr = IDirectInputDevice8_CreateEffect(dev, &GUID_RampForce, &ef, &r,
	    NULL);
	if (FAILED(hr) || r == NULL) {
		out("  ramp CreateEffect failed, 0x%08lx\n", (unsigned long)hr);
		(void)IDirectInputEffect_Stop(c);
		IDirectInputEffect_Release(c);
		return -1;
	}
	out("  four: ramp created, run out, and started again\n");
	(void)IDirectInputEffect_Start(r, 1, 0);
	Sleep(SEQ_RAMP_MS + SEQ_WARM_MS);
	(void)IDirectInputEffect_Start(r, 1, 0);
	Sleep(SEQ_WARM_MS);

	/*
	 * Five: stopped, then started again inside the window.
	 *
	 * A stop is what a game does between races, and it leaves the proxy's
	 * record of the upload standing, so this is exactly the shape where an
	 * unchanged effect is started again with the cache warm. For a ramp
	 * the upload has to go anyway, and whether it did is what the daemon's
	 * log says next.
	 */
	seq_mark();
	out("  five: ramp stopped and started again\n");
	(void)IDirectInputEffect_Stop(r);
	(void)IDirectInputEffect_Start(r, 1, 0);
	Sleep(SEQ_WARM_MS);

	/*
	 * Six: the constant's turn to have a fresh acknowledgement, for the
	 * same reason as four. Nothing is asserted.
	 */
	seq_mark();
	out("  six: constant started again\n");
	(void)IDirectInputEffect_Start(c, 1, 0);
	Sleep(SEQ_WARM_MS);

	/*
	 * Seven: a device reset releases every slot, so the next start has to
	 * carry the whole effect again rather than trust what the daemon had.
	 * Inside the window and with nothing changed, only the proxy noticing
	 * the reset can make that happen.
	 */
	seq_mark();
	(void)IDirectInputDevice8_SendForceFeedbackCommand(dev, DISFFC_RESET);
	out("  seven: device reset, then the constant started again\n");
	(void)IDirectInputEffect_Start(c, 1, 0);
	Sleep(SEQ_WARM_MS);

	/*
	 * Eight: a pause and then a continue, which is the pair SDL makes out
	 * of SDL_HapticPause and SDL_HapticUnpause. The proxy has no wire
	 * opcode for either, so the pause goes out as a stop-everything; the
	 * continue has to start again what the pause stopped, and for a long
	 * time it sent nothing at all, which left a game that paused with no
	 * force feedback for the rest of its run.
	 */
	seq_mark();
	out("  eight: paused, then continued\n");
	(void)IDirectInputDevice8_SendForceFeedbackCommand(dev, DISFFC_PAUSE);
	Sleep(SEQ_WARM_MS);
	(void)IDirectInputDevice8_SendForceFeedbackCommand(dev, DISFFC_CONTINUE);
	Sleep(SEQ_WARM_MS);

	seq_mark();
	(void)IDirectInputEffect_Stop(c);
	(void)IDirectInputEffect_Stop(r);
	IDirectInputEffect_Release(c);
	IDirectInputEffect_Release(r);

	out("  the sequence is the daemon's log, not this one\n");

	return 0;
}

/*
 * Three flags, parsed where they are used. This is the one tool here that
 * does not use getopt, so tests/usage_check.c does not cover it; it does not
 * need to, because there is no separate option string that can drift out of
 * step with the usage text the way probe_intr's -H once did.
 */
static void
usage(void)
{
	fprintf(stderr,
	    "usage: probe_dinput [-i] [-f] [-F] [-o FILE]\n"
	    "\n"
	    "  -o FILE    write the run to FILE as well as to the screen\n"
	    "             (default " LOG_DUMP ", one name per mode so\n"
	    "             runs do not overwrite each other: -controls.log,\n"
	    "             -ffb.log, -controls-ffb.log for both, and\n"
	    "             -sequence.log for -F)\n"
	    "  no flags   dump the device and every object it declares\n"
	    "  -i         then work every control by name, one at a time,\n"
	    "             confirming each, and print a table at the end\n"
	    "  -f         then create two real effects, play them, and\n"
	    "             write down whether you felt each\n"
	    "  -F         then run a scripted sequence of effects without\n"
	    "             asking anybody anything. For a test rig, where no\n"
	    "             one can be surprised by a wheel that moves: what\n"
	    "             it is for is the daemon's packet log, not this one\n"
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
	const char *logpath = NULL;
	int i, want_id = 0, want_ff = 0, want_seq = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-i") == 0)
			want_id = 1;
		else if (strcmp(argv[i], "-f") == 0)
			want_ff = 1;
		else if (strcmp(argv[i], "-F") == 0)
			want_seq = 1;
		else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			logpath = argv[++i];
		else
			usage();
	}

	if (logpath == NULL)
		logpath = default_log(want_id, want_ff, want_seq);

	/*
	 * A run that cannot be written down is worth less than one that can,
	 * but not nothing, so a log that will not open is a warning rather
	 * than a refusal.
	 */
	if ((logfp = fopen(logpath, "w")) == NULL)
		printf("cannot write %s, this run will only be on screen\n",
		    logpath);
	else
		printf("writing this run to %s\n", logpath);

	out("probe_dinput %s\n", T150_PROXY_VERSION);

	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
		out("CoInitializeEx failed\n");
		return 1;
	}

	hr = DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION,
	    &IID_IDirectInput8A, (void **)&di, NULL);
	if (FAILED(hr) || di == NULL) {
		out("DirectInput8Create failed, 0x%08lx\n", (unsigned long)hr);
		return 1;
	}

	memset(&inst, 0, sizeof(inst));
	(void)IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, on_device,
	    &inst, DIEDFL_ATTACHEDONLY);
	if (IsEqualGUID(&inst, &GUID_NULL)) {
		out("no T150 found. Is the wheel in firmware mode and does the "
		    "bottle see it at all?\n");
		return 1;
	}

	hr = IDirectInput8_CreateDevice(di, &inst, &dev, NULL);
	if (FAILED(hr) || dev == NULL) {
		out("CreateDevice failed, 0x%08lx\n", (unsigned long)hr);
		return 1;
	}

	hr = IDirectInputDevice8_SetDataFormat(dev, &c_dfDIJoystick2);
	if (FAILED(hr))
		out("SetDataFormat failed, 0x%08lx\n", (unsigned long)hr);

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
		out("SetCooperativeLevel(exclusive) failed, 0x%08lx. Force "
		    "feedback will not work; run this from a terminal.\n",
		    (unsigned long)hr);

	hr = IDirectInputDevice8_Acquire(dev);
	if (FAILED(hr))
		out("Acquire failed, 0x%08lx\n", (unsigned long)hr);

	dump_chain();
	dump_caps();
	out("\nobjects:\n");
	(void)IDirectInputDevice8_EnumObjects(dev, on_object, NULL,
	    DIDFT_ALL);
	resolve_state_offsets();
	dump_state_map();
	out("\neffects the device says it supports:\n");
	(void)IDirectInputDevice8_EnumEffects(dev, on_effect, NULL, DIEFT_ALL);

	out("\n%d axis(es), %d button(s)\n", naxes, nbuttons);
	for (i = 0; i < naxes; i++)
		if (axes[i].is_ff)
			break;

	/*
	 * A device that claims force feedback and marks no axis to apply it
	 * to is a shape no real wheel has, and callers are entitled not to
	 * expect it: SDL counts actuator axes, finds none, and then builds
	 * effects with no axes at all. Saying the two facts on separate
	 * lines left a reader to notice the contradiction, and nobody did
	 * for four releases.
	 */
	if (i == naxes && ff_claimed)
		out("no axis is marked as a force feedback actuator, and the "
		    "device claims\nforce feedback: a game that looks for the "
		    "actuator will find none and may\nturn force feedback off "
		    "or send effects with no axis. RESEARCH.md B13.\n");
	else if (i == naxes)
		out("no axis is marked as a force feedback actuator, which "
		    "matches a device\nthat does not claim force feedback "
		    "either\n");

	if (want_id)
		identify();
	if (want_ff)
		(void)ffb_test();
	if (want_seq)
		(void)ffb_sequence();

	(void)IDirectInputDevice8_Unacquire(dev);
	IDirectInputDevice8_Release(dev);
	IDirectInput8_Release(di);
	CoUninitialize();

	if (logfp != NULL) {
		out("\nend of run. Send %s.\n", logpath);
		(void)fclose(logfp);
	}

	return 0;
}
