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
 * This does not open the wheel's input. That is protocol, session.c sends it
 * on hello, and it matters: the wheel renders no effect while no input is
 * open.
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

#include "t150/t150.h"
#include "t150d.h"

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
	uint64_t	 next_scan_ms;
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
		return -1;
	if ((n = CFSetGetCount(h->devices)) < 1) {
		CFRelease(h->devices);
		h->devices = NULL;
		return -1;
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

	if (h->dev == NULL) {
		uint64_t now = mono_ms();

		/* Rate limited, so a busy game cannot turn this into a spin. */
		if (now < h->next_scan_ms)
			return -1;
		h->next_scan_ms = now + RESCAN_MS;
		if (acquire(h) != 0)
			return -1;
	}

	/*
	 * Report id 0 and the payload raw. The wheel's descriptor declares an
	 * output report with id 0x0A, and that is not what it listens to:
	 * every packet this project has ever moved the wheel with was sent
	 * unnumbered. RESEARCH.md A19.
	 */
	r = IOHIDDeviceSetReport(h->dev, kIOHIDReportTypeOutput, 0, buf,
	    (CFIndex)len);
	if (r != kIOReturnSuccess) {
		if (h->verbose)
			fprintf(stderr, "t150d: SetReport failed: 0x%08x\n",
			    (unsigned int)r);
		/*
		 * Assume the wheel has gone rather than that one packet was
		 * bad. Unplugging is the common cause and the next write
		 * re-acquires; a genuinely bad packet would fail again and
		 * cost only a scan interval.
		 */
		drop_device(h);
		h->next_scan_ms = mono_ms() + RESCAN_MS;
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
