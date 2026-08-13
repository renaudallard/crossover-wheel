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
 * Opening the wheel's input is protocol, and this file holds it open for as
 * long as it holds the wheel. The firmware renders no effect while no
 * application has the input open, and it also rests the pedals at maximum,
 * so a wheel whose input is shut reads to a game as though both pedals are
 * pressed. Every acquire therefore closes the input and opens it again: the
 * close ends anything an inherited wheel was still rendering, since an open
 * outlives whoever sent it (A30), and the open is what the pedals and the
 * next game need. Every slot is scrubbed first for the same inheritance
 * reason.
 *
 * NOT reentrant and not thread safe, which suits the single threaded daemon
 * that owns it.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "t150/encode.h"
#include "t150/t150.h"
#include "t150d.h"

#include "mac/bootswitch.h"
#include "wirequeue.h"

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
	 *
	 * Atomic because with a writer it is set by the poll thread inside
	 * hid_write and read by the writer inside acquire(). One byte, but a
	 * race is a race.
	 */
	_Atomic uint8_t	 input_state;
	/*
	 * The last boot mode outcome that was logged. The scan runs twice a
	 * second now, so saying the same thing every time buries whatever
	 * else the daemon has to say, and the commonest outcome by far is
	 * the least interesting one.
	 */
	int		 last_boot;
	long		 vid;
	long		 pid;
	unsigned int	 gap_ms;
	int		 verbose;
	int		 opened;

	/*
	 * The writer, when -w asked for one. Everything about the device is
	 * then owned by that thread and touched by nothing else: this file
	 * says at the top that it is not reentrant, and the way to keep that
	 * true with a thread is for the other thread never to come in.
	 *
	 * The poll thread only ever appends to the queue and signals. It does
	 * not acquire, does not release, does not write and does not read the
	 * device, so there is one owner and no lock around IOKit at all.
	 *
	 * The queue coalesces, which is what keeps the wheel current rather
	 * than merely fed: see wirequeue.c for the rates that make that
	 * necessary.
	 */
	int			 threaded;
	pthread_t		 thread;
	pthread_mutex_t		 mtx;
	pthread_cond_t		 cv;
	struct t150_wirequeue	 q;
	int			 stop;
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
	if (h->verbose && (int)r != h->last_boot) {
		h->last_boot = (int)r;
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
	 *
	 * Reported rather than sent in silence, for the reason the gain and
	 * the range are: a wheel that vibrates about a point is what A17 says
	 * this spring feels like, the tester now reports one at dead centre
	 * and again near 135 degrees, and nothing anywhere could say whether
	 * this packet had reached the wheel, been refused, or been sent to a
	 * wheel that was already going away. That is three faults with one
	 * silence between them.
	 */
	if ((len = t150_enc_autocenter_force(pkt, sizeof(pkt), 0)) > 0) {
		IOReturn ar = raw_write(h, pkt, len);

		if (h->verbose)
			fprintf(stderr, "t150d: wheel autocentre released: "
			    "%s\n", ar == kIOReturnSuccess ? "sent" :
			    "the write failed");
	}
	nap_ms(h->gap_ms);

	/*
	 * Close the input, then open it, and both halves are load bearing.
	 *
	 * The close is what ends the rendering of anything the scrub above
	 * could not reach: the firmware runs no effect while no application
	 * holds the input (A28), and an open outlives whoever sent it (A30),
	 * so a wheel inherited from a crashed daemon or an abandoned probe
	 * can arrive still pushing. Closing it goes quiet at startup rather
	 * than at shutdown.
	 *
	 * The open is what makes the pedals read correctly, and this cost a
	 * release to learn. With the input shut, the wheel rests its pedals at
	 * maximum, so a game that enumerates the wheel before the proxy has
	 * connected calibrates them fully pressed and is inverted from then
	 * on. That is what a tester saw the moment the daemon started doing
	 * its own boot switch: the wheel re-enumerates, comes back with the
	 * input shut, and the game reads it in that state. Running t150boot by
	 * hand first had hidden it, because the input was still open from an
	 * earlier session.
	 *
	 * So the daemon holds the input for as long as it holds the wheel.
	 * Nothing renders while no client has uploaded anything, and the
	 * session still closes it in the safe state when the client goes.
	 */
	if ((len = t150_enc_input_close(pkt, sizeof(pkt))) > 0)
		(void)raw_write(h, pkt, len);
	nap_ms(h->gap_ms);

	if ((len = t150_enc_input_open(pkt, sizeof(pkt))) > 0 &&
	    raw_write(h, pkt, len) != kIOReturnSuccess && h->verbose)
		fprintf(stderr, "t150d: could not open the wheel's input\n");
	h->input_state = T150_INPUT_OPEN;

	/*
	 * Say that the wheel is a new one as far as its contents go. The
	 * scrub above emptied every slot, so a session that believes it has
	 * already uploaded an effect is wrong from here on, and only this
	 * tells it so. Bumped after the scrub and the input replay, so what
	 * the session reconciles against is a wheel already in a known state.
	 */
	if (h->be != NULL)
		h->be->epoch++;
	h->last_boot = -1;

	if (h->verbose)
		fprintf(stderr, "t150d: wheel %04lx:%04lx open\n", h->vid,
		    h->pid);

	return 0;
}

/*
 * One packet out of the queue, or nothing. The caller owns the device, so
 * this only moves bytes.
 */
static int
queue_pop(struct hid_be *h, struct t150_wire *out)
{
	int got;

	pthread_mutex_lock(&h->mtx);
	got = t150_wq_pop(&h->q, out);
	pthread_mutex_unlock(&h->mtx);

	return got;
}

/*
 * The writer. It owns the device: it acquires it, drops it, scans for it and
 * is the only thing that writes to it. The poll thread appends and signals
 * and never blocks, which is the whole point: a game waiting for the daemon's
 * reply is no longer waiting for a USB transfer to finish.
 */
static void *
writer_main(void *arg)
{
	struct hid_be *h = arg;
	struct t150_wire p;

	for (;;) {
		struct timespec ts;

		pthread_mutex_lock(&h->mtx);
		while (t150_wq_depth(&h->q) == 0 && !h->stop) {
			/*
			 * Woken by work or by the timeout, whichever comes
			 * first. The timeout is what drives the scan for a
			 * wheel that is not there, since nothing will signal
			 * on its account.
			 */
			(void)clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += (long)RESCAN_MS * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += ts.tv_nsec / 1000000000L;
				ts.tv_nsec %= 1000000000L;
			}
			(void)pthread_cond_timedwait(&h->cv, &h->mtx, &ts);
		}
		if (h->stop && t150_wq_depth(&h->q) == 0) {
			pthread_mutex_unlock(&h->mtx);
			break;
		}
		pthread_mutex_unlock(&h->mtx);

		/*
		 * Look for the wheel on this thread too, for the same reason
		 * the poll thread used to: a replug leaves it at the boot id
		 * and nothing else will go looking.
		 */
		if (h->dev == NULL) {
			if (mono_ms() >= h->next_scan_ms) {
				h->next_scan_ms = mono_ms() + RESCAN_MS;
				(void)acquire(h);
			}
			if (h->dev == NULL) {
				/* Nowhere to put them. */
				pthread_mutex_lock(&h->mtx);
				t150_wq_clear(&h->q);
				pthread_mutex_unlock(&h->mtx);
				continue;
			}
		}

		while (queue_pop(h, &p)) {
			IOReturn r = raw_write(h, p.buf, p.len);

			if (r != kIOReturnSuccess) {
				if (h->verbose)
					fprintf(stderr, "t150d: SetReport "
					    "failed: 0x%08x\n",
					    (unsigned int)r);
				if (means_removed(r)) {
					drop_device(h);
					h->next_scan_ms = mono_ms() + RESCAN_MS;
					break;
				}
			}
			nap_ms(h->gap_ms);
		}
	}

	return NULL;
}

/* Queue it, and never block the caller. */
static int
queue_push(struct hid_be *h, const uint8_t *buf, size_t len)
{
	int r;

	pthread_mutex_lock(&h->mtx);
	r = t150_wq_push(&h->q, buf, len);
	pthread_mutex_unlock(&h->mtx);
	if (r != 0)
		return r;
	pthread_cond_signal(&h->cv);

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

	/*
	 * With a writer, this returns the moment the bytes are copied. That
	 * is the whole fix: the daemon goes straight back to its socket and
	 * the game's next frame is answered without waiting for USB.
	 */
	if (h->threaded)
		return queue_push(h, buf, len);

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

/*
 * Notice the wheel has gone, and look for it again, on the daemon's clock
 * rather than only when something wants to write.
 *
 * A replug is exactly the case where nothing wants to write: the watchdog has
 * fired, the session has cleared every slot, the tick emits nothing and a
 * keepalive writes nothing at all. So a rescan that only runs when a write
 * fails never runs.
 *
 * Noticing has to happen here too, and the first version of this got that
 * wrong. It returned as soon as h->dev was non-NULL, which reads as "we have
 * a wheel, nothing to do", and h->dev is only cleared by a write that fails.
 * With no writes there was nothing to clear it, so the handle stayed a
 * pointer to a wheel that had been unplugged minutes ago and this returned
 * at its first line forever. The scan was fixed and the detection was left
 * where it had always been.
 *
 * The presence test is a registry match by vendor and product id, the same
 * one the boot switch uses. It needs no run loop and no open device, unlike
 * asking the HID manager, which would have to be scheduled and pumped.
 */
static void
hid_tick(void *priv, uint64_t now_ms)
{
	struct hid_be *h = priv;
	io_service_t svc;

	(void)now_ms;	/* the backend keeps its own monotonic clock */

	/* The writer scans on its own account, and owns the device. */
	if (h->threaded)
		return;

	if (mono_ms() < h->next_scan_ms)
		return;
	h->next_scan_ms = mono_ms() + RESCAN_MS;

	if (h->dev == NULL) {
		(void)acquire(h);
		return;
	}

	/* Still on the bus: keep it, and say nothing. */
	if ((svc = t150_usb_find(h->vid, h->pid)) != IO_OBJECT_NULL) {
		IOObjectRelease(svc);
		return;
	}

	/*
	 * Gone, and nobody had written to it to find out. Let go now so the
	 * next tick can look for it properly, rather than waiting for a game
	 * that has already given up to try again.
	 */
	drop_device(h);
	if (h->verbose)
		fprintf(stderr, "t150d: the wheel left the bus\n");
}

static void
hid_close(void *priv)
{
	struct hid_be *h = priv;

	if (h == NULL)
		return;

	/*
	 * Let the writer finish what it has before the device goes. The
	 * safe state is the last thing the session writes, and it is the one
	 * burst that must not be thrown away: a wheel left holding a force
	 * pulls on somebody's hands.
	 */
	if (h->threaded) {
		pthread_mutex_lock(&h->mtx);
		h->stop = 1;
		pthread_mutex_unlock(&h->mtx);
		pthread_cond_signal(&h->cv);
		(void)pthread_join(h->thread, NULL);
		/*
		 * Merged packets are the queue working: the daemon builds
		 * faster than the wheel takes and the newer value replaced
		 * one the wheel was never going to render. Dropped packets
		 * are the queue failing, and after coalescing that means a
		 * hundred and twenty eight distinct packets went unwritten.
		 */
		if (h->verbose)
			fprintf(stderr, "t150d: the writer merged %u packet(s) "
			    "into fresher ones and dropped %u\n",
			    h->q.merged, h->q.dropped);
	}

	drop_device(h);
	if (h->mgr != NULL) {
		(void)IOHIDManagerClose(h->mgr, kIOHIDOptionsTypeNone);
		CFRelease(h->mgr);
	}
	free(h);
}

int
t150_backend_hid(struct t150_backend *be, long vid, long pid,
    unsigned int gap_ms, int verbose, int threaded)
{
	struct hid_be *h;

	if ((h = calloc(1, sizeof(*h))) == NULL)
		return -1;

	h->vid = vid;
	h->pid = pid;
	/*
	 * Not any outcome, so the first of every kind is said. calloc's 0
	 * would have been T150_BOOT_SENT and would have eaten the one line
	 * here that most wants saying.
	 */
	h->last_boot = -1;
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

	if (threaded) {
		t150_wq_init(&h->q);
		if (pthread_mutex_init(&h->mtx, NULL) != 0 ||
		    pthread_cond_init(&h->cv, NULL) != 0) {
			hid_close(h);
			return -1;
		}
		if (pthread_create(&h->thread, NULL, writer_main, h) != 0) {
			hid_close(h);
			return -1;
		}
		h->threaded = 1;
	}

	be->name = threaded ? "macOS HID, threaded" : "macOS HID";
	be->write = hid_write;
	be->tick = hid_tick;
	be->close = hid_close;
	be->priv = h;

	return 0;
}
