/*
 * hid_darwin.c - the backend that actually drives the wheel.
 *
 * Writes the daemon's packets to the T150 with IOHIDDeviceSetReport, which
 * needs no root, no entitlement and no device capture. That last part is the
 * whole reason this design works: CrossOver keeps reading the wheel as an
 * ordinary joystick while this writes to it, so a game gets its input from
 * the normal path and its force feedback from here.
 *
 * The framing is the one that was measured, not the one the descriptor
 * suggests: report type output, report id 0, and the payload raw. See
 * RESEARCH.md A19 for the run that settled it and PROTOCOL.md for why the
 * declared id 0x0A report is a red herring.
 *
 * Opening the wheel's input is protocol and session.c owns it: the wheel
 * renders no effect while no input is open. This file remembers the last such
 * packet and replays it after re-acquiring a device, because a replugged
 * wheel forgets and the session has no way to know that happened. It also
 * scrubs every slot on acquire, because the open outlives whoever sent it
 * and a wheel inherited from a dead process can still be rendering that
 * process's last effect. RESEARCH.md A30.
 *
 * NOT reentrant and not thread safe, which suits the single threaded daemon
 * that owns it.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "t150/encode.h"
#include "t150/t150.h"
#include "t150d.h"

#include "mac/bootswitch.h"

/*
 * How long to wait before looking for the wheel again after losing it.
 * Unplugged wheels come back on their own and a game that keeps sending is
 * not a reason to walk the IOKit registry every millisecond.
 */
#define RESCAN_MS	500u

struct hid_be {
	IOHIDManagerRef	 mgr;
	IOHIDDeviceRef	 dev;		/* borrowed from the manager */
	CFSetRef	 devices;	/* owns dev, released with it */
	/*
	 * The backend this drives, so acquire() can say that it happened.
	 * The scrub below wipes every slot on the wheel and the session has
	 * no other way to find that out.
	 */
	struct t150_backend *be;
	uint64_t	 next_scan_ms;
	/*
	 * The last input open or close that passed through, replayed after a
	 * re-acquire. A wheel that is unplugged and replugged comes back with
	 * its input shut, and only this layer knows that happened: the
	 * session sends 42 04 once, on hello, and would never send it again.
	 * Without this a mid-game replug, or a wheel that only appears after
	 * the client has said hello, leaves the wheel silently deaf to every
	 * effect. Set from the intent rather than from a successful write, so
	 * that an open sent to an absent wheel still counts. 0 means nothing
	 * has been asked for yet.
	 */
	uint8_t		 input_state;
	long		 vid;
	long		 pid;
	unsigned int	 gap_ms;
	int		 verbose;
	int		 opened;
};

static uint64_t
mono_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;

	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

static void
nap_ms(unsigned int ms)
{
	struct timespec ts;

	if (ms == 0)
		return;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	(void)nanosleep(&ts, NULL);
}

static CFMutableDictionaryRef
match_dict(long vid, long pid)
{
	CFMutableDictionaryRef d;
	CFNumberRef n;
	int v = (int)vid, p = (int)pid;
	int up = 0x01, us = 0x04;	/* generic desktop, joystick */

	d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (d == NULL)
		return NULL;

	if ((n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v))) {
		CFDictionarySetValue(d, CFSTR(kIOHIDVendorIDKey), n);
		CFRelease(n);
	}
	if ((n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &p))) {
		CFDictionarySetValue(d, CFSTR(kIOHIDProductIDKey), n);
		CFRelease(n);
	}

	/*
	 * Narrow it to the joystick node the wheel publishes in firmware
	 * mode, so that a device with more than one HID node cannot leave the
	 * choice to whatever order an unordered set happens to yield.
	 */
	if ((n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &up))) {
		CFDictionarySetValue(d, CFSTR(kIOHIDPrimaryUsagePageKey), n);
		CFRelease(n);
	}
	if ((n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &us))) {
		CFDictionarySetValue(d, CFSTR(kIOHIDPrimaryUsageKey), n);
		CFRelease(n);
	}

	return d;
}

/* Let the go and release whatever we are holding, without closing the manager. */
static void
drop_device(struct hid_be *h)
{
	if (h->dev != NULL && h->opened)
		(void)IOHIDDeviceClose(h->dev, kIOHIDOptionsTypeNone);
	h->opened = 0;
	h->dev = NULL;
	if (h->devices != NULL) {
		CFRelease(h->devices);
		h->devices = NULL;
	}
}

/*
 * Errors that mean the wheel is gone rather than that one packet was bad.
 *
 * The named IOKit returns are the polite way for the device to say so, and
 * they are not the way it actually said it. A wheel unplugged mid race takes
 * the mach port behind its IOHIDDeviceRef with it, and the next SetReport
 * comes back 0x10000003, which is MACH_SEND_INVALID_DEST: a send to a dead
 * port name, from a layer below IOKit entirely. That is not in the list
 * below and never could be, so the daemon kept a corpse, every write failed,
 * the wheel's input was never reopened, and the driver felt nothing but the
 * wheel's own centring spring for the rest of the session. Test 30, and it
 * took a wheel being unplugged in a game to find it.
 *
 * So the test is the domain rather than the list. Both mach and IOKit put
 * the system in the top six bits; IOKit's is 0x38 and everything it returns
 * begins 0xE00002xx. A status from any other system came from below the
 * driver, and by the time a mach send has failed the port is already dead,
 * which is the same thing as the device being gone.
 */
#define ERR_SYSTEM(x)	(((unsigned int)(x) >> 26) & 0x3f)
#define SYS_IOKIT	0x38u

static int
means_removed(IOReturn r)
{
	if (r == kIOReturnSuccess)
		return 0;
	if (ERR_SYSTEM(r) != SYS_IOKIT)
		return 1;

	return r == kIOReturnNoDevice || r == kIOReturnNotOpen ||
	    r == kIOReturnOffline || r == kIOReturnNotAttached ||
	    r == kIOReturnNotResponding;
}

static IOReturn
raw_write(struct hid_be *h, const uint8_t *buf, size_t len)
{
	/*
	 * Report id 0 and the payload raw. The wheel's descriptor declares an
	 * output report with id 0x0A, and that is not what it listens to:
	 * every packet this project has ever moved the wheel with was sent
	 * unnumbered. RESEARCH.md A19.
	 */
	return IOHIDDeviceSetReport(h->dev, kIOHIDReportTypeOutput, 0, buf,
	    (CFIndex)len);
}

/*
 * No wheel at the firmware id. It may still be sitting at the boot id, which
 * is where every replug and every wake leaves it, so switch it and let the
 * next scan find it. Until this existed a wheel unplugged during a game never
 * came back on its own however correctly the daemon noticed it had gone: the
 * person driving had to run t150boot by hand, mid race.
 *
 * Always returns -1. The wheel is not usable yet either way, and the rescan
 * timer decides when to look again.
 */
static int
boot_switch_if_present(struct hid_be *h)
{
	enum t150_boot_result r;
	uint8_t model = 0;

	r = t150_boot_switch(h->vid, T150_PID_BOOT, T150_SWITCH_VALUE, &model);

	/*
	 * Every outcome is said, including the boring one. A silent failure
	 * here is indistinguishable from never having looked, which is
	 * exactly the question a replug report has to answer.
	 */
	if (h->verbose) {
		if (r == T150_BOOT_SENT)
			fprintf(stderr, "t150d: switched a wheel out of boot "
			    "mode, waiting for it to come back\n");
		else if (r == T150_BOOT_OTHER_MODEL)
			fprintf(stderr, "t150d: boot mode: %s (0x%02x), not "
			    "switching it. Use t150boot -V\n",
			    t150_boot_result_str(r), model);
		else
			fprintf(stderr, "t150d: boot mode: %s\n",
			    t150_boot_result_str(r));
	}

	return -1;
}

/*
 * Find the wheel and open it without seizing it. Returns 0 when h->dev is
 * usable. The manager is scheduled on the run loop and pumped until it goes
 * quiet, because a manager that has never run its sources reports no devices.
 */
static int
acquire(struct hid_be *h)
{
	CFMutableDictionaryRef d;
	const void **items;
	CFIndex n;
	IOReturn r;
	/*
	 * Holds the longest packet this function encodes, which is four bytes
	 * for both a control and a settings write. An encoder handed a buffer
	 * shorter than its packet returns 0 rather than writing past the end,
	 * and every call here is guarded on that, so this cannot overflow even
	 * if a packet grows.
	 */
	uint8_t pkt[T150_FF_CONTROL_LEN];
	size_t i, len;

	drop_device(h);

	if ((d = match_dict(h->vid, h->pid)) == NULL)
		return -1;
	IOHIDManagerSetDeviceMatching(h->mgr, d);
	CFRelease(d);

	IOHIDManagerScheduleWithRunLoop(h->mgr, CFRunLoopGetCurrent(),
	    kCFRunLoopDefaultMode);
	while (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, TRUE) ==
	    kCFRunLoopRunHandledSource)
		continue;
	IOHIDManagerUnscheduleFromRunLoop(h->mgr, CFRunLoopGetCurrent(),
	    kCFRunLoopDefaultMode);

	if ((h->devices = IOHIDManagerCopyDevices(h->mgr)) == NULL)
		return boot_switch_if_present(h);
	if ((n = CFSetGetCount(h->devices)) < 1) {
		CFRelease(h->devices);
		h->devices = NULL;
		return boot_switch_if_present(h);
	}

	if ((items = calloc((size_t)n, sizeof(*items))) == NULL) {
		CFRelease(h->devices);
		h->devices = NULL;
		return -1;
	}
	CFSetGetValues(h->devices, items);
	h->dev = (IOHIDDeviceRef)items[0];
	free(items);

	/*
	 * kIOHIDOptionsTypeNone, never kIOHIDOptionsTypeSeizeDevice. Seizing
	 * would take the wheel away from CrossOver, which is exactly what
	 * this whole design exists to avoid.
	 */
	r = IOHIDDeviceOpen(h->dev, kIOHIDOptionsTypeNone);
	if (r != kIOReturnSuccess) {
		if (h->verbose)
			fprintf(stderr, "t150d: cannot open the wheel: "
			    "0x%08x%s\n", (unsigned int)r,
			    r == kIOReturnExclusiveAccess ?
			    ", something has seized it" : "");
		h->dev = NULL;
		CFRelease(h->devices);
		h->devices = NULL;
		return -1;
	}
	h->opened = 1;

	/*
	 * The wheel keeps whatever it was last told, and the input open
	 * outlives whoever sent it (RESEARCH.md A30), so the wheel taken here
	 * may arrive with an effect still playing and its input still open,
	 * left by a probe or by a daemon that died. Stop every slot, so what
	 * follows starts from a wheel whose state is known. Failures are
	 * ignored: if the wheel vanished mid-scrub the next write finds out.
	 */
	for (i = 0; i < T150_SLOT_MAX; i++) {
		if ((len = t150_enc_control(pkt, sizeof(pkt), (uint8_t)i, 0,
		    0)) > 0)
			(void)raw_write(h, pkt, len);
		nap_ms(h->gap_ms);
	}

	/*
	 * Release the autocenter, for the same reason the session's safe state
	 * does: a wheel this daemon holds should be limp rather than fighting
	 * whoever turns it. It matters more here than it looks. Closing the
	 * input below re-arms the wheel's built-in autocenter, because the
	 * firmware runs it whenever no application has the input open (A15),
	 * and that spring is felt as a stiffness about a point that does not
	 * return the wheel to centre (A17). Without this the daemon idling on
	 * its real backend leaves a wheel that is hard to turn and never
	 * recentres, which is exactly what a tester reported. The enable flag
	 * is not sent with it: it is a no-op on macOS, and only the force
	 * decides the strength.
	 */
	if ((len = t150_enc_autocenter_force(pkt, sizeof(pkt), 0)) > 0)
		(void)raw_write(h, pkt, len);
	nap_ms(h->gap_ms);

	/*
	 * Then put the input where the session believes it is. A replugged
	 * wheel has forgotten that its input was open, and the session will
	 * not say so again, so replay it here or every effect after a replug
	 * is accepted and ignored. With no session wanting it open, close it,
	 * which is what ends the rendering of anything the scrub above could
	 * not reach and returns an inherited wheel to its idle state.
	 */
	if (h->input_state == T150_INPUT_OPEN)
		len = t150_enc_input_open(pkt, sizeof(pkt));
	else
		len = t150_enc_input_close(pkt, sizeof(pkt));
	if (len > 0 && raw_write(h, pkt, len) != kIOReturnSuccess &&
	    h->verbose)
		fprintf(stderr, "t150d: could not set the wheel's input "
		    "state\n");

	/*
	 * Say that the wheel is a new one as far as its contents go. The
	 * scrub above emptied every slot, so a session that believes it has
	 * already uploaded an effect is wrong from here on, and only this
	 * tells it so. Bumped after the scrub and the input replay, so what
	 * the session reconciles against is a wheel already in a known state.
	 */
	if (h->be != NULL)
		h->be->epoch++;

	if (h->verbose)
		fprintf(stderr, "t150d: wheel %04lx:%04lx open\n", h->vid,
		    h->pid);

	return 0;
}

static int
hid_write(void *priv, const uint8_t *buf, size_t len)
{
	struct hid_be *h = priv;
	IOReturn r;

	if (len == 0)
		return 0;

	/*
	 * Record what the session wants before trying to send it, not after
	 * succeeding. A client that says hello while the wheel is absent, or
	 * still at the boot product id, has its 42 04 dropped here, and if
	 * that were only remembered on success the acquire that follows would
	 * open a device and never open its input. Every write after it would
	 * return success and the wheel would render nothing for the life of
	 * that client, which is the exact failure that took eleven sessions
	 * to find the first time.
	 */
	if (len == 2 && buf[0] == T150_OP_INPUT)
		h->input_state = buf[1];

	if (h->dev == NULL) {
		uint64_t now = mono_ms();

		/* Rate limited, so a busy game cannot turn this into a spin. */
		if (now < h->next_scan_ms)
			return -1;
		h->next_scan_ms = now + RESCAN_MS;
		if (acquire(h) != 0)
			return -1;
	}

	r = raw_write(h, buf, len);
	if (r != kIOReturnSuccess) {
		if (h->verbose)
			fprintf(stderr, "t150d: SetReport failed: 0x%08x\n",
			    (unsigned int)r);
		/*
		 * Only let go of the wheel when the error says it has gone.
		 * Dropping on any failure meant one transient refusal took
		 * the device away, and every packet after it in the same
		 * burst then failed on the rescan timer without being tried:
		 * a safe state that lost its first packet lost all of them,
		 * on a wheel that was still attached and still pushing.
		 */
		if (means_removed(r)) {
			drop_device(h);
			h->next_scan_ms = mono_ms() + RESCAN_MS;
		}
		return -1;
	}

	/*
	 * An optional pause between packets. probe_setreport has always left
	 * 20 ms between its writes with a comment saying the wheel drops back
	 * to back settings packets, and every successful run through the HID
	 * path has therefore had that pause. Nothing has ever tested whether
	 * it is needed, and the interrupt OUT path works with none, so the
	 * default here is none: a floor on the write rate would cap effect
	 * updates far below what force feedback wants. -g exists to put it
	 * back if a real wheel says otherwise.
	 */
	nap_ms(h->gap_ms);

	return 0;
}

static void
hid_close(void *priv)
{
	struct hid_be *h = priv;

	if (h == NULL)
		return;

	drop_device(h);
	if (h->mgr != NULL) {
		(void)IOHIDManagerClose(h->mgr, kIOHIDOptionsTypeNone);
		CFRelease(h->mgr);
	}
	free(h);
}

int
t150_backend_hid(struct t150_backend *be, long vid, long pid,
    unsigned int gap_ms, int verbose)
{
	struct hid_be *h;

	if ((h = calloc(1, sizeof(*h))) == NULL)
		return -1;

	h->vid = vid;
	h->pid = pid;
	h->gap_ms = gap_ms;
	h->verbose = verbose;
	/* Wired before the first acquire, which bumps the epoch itself. */
	h->be = be;
	be->epoch = 0;

	h->mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	if (h->mgr == NULL) {
		free(h);
		return -1;
	}
	if (IOHIDManagerOpen(h->mgr, kIOHIDOptionsTypeNone) !=
	    kIOReturnSuccess) {
		CFRelease(h->mgr);
		free(h);
		return -1;
	}

	/*
	 * Not an error if the wheel is absent now. A daemon started before
	 * the wheel is plugged in, or before t150boot has switched it out of
	 * boot mode, has to wait rather than give up.
	 */
	if (acquire(h) != 0 && verbose)
		fprintf(stderr, "t150d: no wheel yet, will keep looking\n");

	be->name = "macOS HID";
	be->write = hid_write;
	be->close = hid_close;
	be->priv = h;

	return 0;
}
