/*
 * probe_intr - write to the wheel on the pipe it actually listens to.
 *
 * The wheel obeys settings on both this pipe and the HID one, so this tool is
 * the cross-check rather than the only way in. RESEARCH.md A19 has the
 * measurement, and C7, which predicted the HID path could not work, is
 * recorded there as false for this wheel.
 *
 * Reaching this pipe from userspace means taking the device away from the HID
 * driver, which is what this does: capture, write, release. That is the price,
 * and it is why the daemon uses the HID path instead. Settings survive the
 * release because the wheel keeps them. An uploaded effect does not, because
 * the release re-enumerates the wheel, so -H holds the device open for a while
 * first and a force feedback test is meaningless without it.
 *
 * Having the device captured also allows the opposite direction: -R reads the
 * interrupt IN pipe, which is the only way to see what the wheel reports
 * without anything above the USB layer in the way.
 *
 * NEEDS ROOT. Device capture is privileged, unlike everything else here.
 *
 * The wheel is handed back on every exit path, including failures. If this
 * is killed between the capture and the release, unplug and replug the wheel
 * to get it back.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "t150/encode.h"
#include "t150/t150.h"

#define MAX_PAYLOAD	64
#define MAX_PACKETS	8

/* The device disappears and comes back around a capture. */
#define SETTLE_MS	500
#define WAIT_TRIES	300
#define WAIT_STEP_MS	10

#define READ_SECONDS	15

/* Bound on how long the abort below is given to settle. */
#define DRAIN_TRIES	20
#define DRAIN_STEP	0.1

static void
nap(long ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	(void)nanosleep(&ts, NULL);
}

struct packet {
	uint8_t	bytes[MAX_PAYLOAD];
	size_t	len;
};

/*
 * Why a read is running. The two callers want opposite things from the
 * person at the wheel, and telling them the wrong one wastes the session:
 * -R wants every control worked, -H wants hands off so that any movement is
 * the wheel's own.
 */
enum read_why {
	READ_CONTROLS = 0,
	READ_EFFECT
};

/* A pipe reference as IOUSBLib wants it, plus the endpoint it belongs to. */
struct pipe {
	UInt8	ref;
	UInt8	addr;
};

/*
 * The initialisation the Linux driver performs before the mode switch, sent
 * on the interrupt OUT endpoint while the wheel is still at the boot product
 * id. Source: drivers/hid/hid-thrustmaster.c, setup_0 to setup_4, sent by
 * thrustmaster_interrupts() from thrustmaster_probe() before the model query.
 * Akellacom's macOS T300RS driver ships the same five packets and says they
 * "MUST be sent before the mode switch".
 *
 * This project never sent them, which is the difference between a wheel that
 * ends up free on Linux and one that ends up blocked here.
 */
static const uint8_t init_pkts[][9] = {
	{ 0x42, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x0a, 0x04, 0x90, 0x03, 0x00, 0x00, 0x00, 0x00 },
	{ 0x0a, 0x04, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00 },
	{ 0x0a, 0x04, 0x12, 0x10, 0x00, 0x00, 0x00, 0x00 },
	{ 0x0a, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00 }
};
static const size_t init_lens[] = { 9, 8, 8, 8, 8 };

enum action {
	ACT_NONE = 0,
	ACT_INIT,
	ACT_AUTOCENTER,
	ACT_AUTOCENTER_OFF,
	ACT_RANGE,
	ACT_GAIN,
	ACT_RAW,
	ACT_READ,
	ACT_INPUT_OPEN,
	ACT_INPUT_CLOSE
};

static io_service_t
find_device(long vid, long pid)
{
	CFMutableDictionaryRef match;
	io_iterator_t iter = IO_OBJECT_NULL;
	io_service_t svc;
	SInt32 v = (SInt32)vid, p = (SInt32)pid;
	CFNumberRef n;

	if ((match = IOServiceMatching(kIOUSBDeviceClassName)) == NULL)
		return IO_OBJECT_NULL;

	if ((n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v))) {
		CFDictionarySetValue(match, CFSTR(kUSBVendorID), n);
		CFRelease(n);
	}
	if ((n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &p))) {
		CFDictionarySetValue(match, CFSTR(kUSBProductID), n);
		CFRelease(n);
	}

	if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) !=
	    KERN_SUCCESS)
		return IO_OBJECT_NULL;

	svc = IOIteratorNext(iter);
	IOObjectRelease(iter);

	return svc;
}

/* Wait for the wheel to come back after a re-enumeration. */
static io_service_t
wait_for_device(long vid, long pid)
{
	io_service_t svc;
	int i;

	nap(SETTLE_MS);
	for (i = 0; i < WAIT_TRIES; i++) {
		if ((svc = find_device(vid, pid)) != IO_OBJECT_NULL)
			return svc;
		nap(WAIT_STEP_MS);
	}

	return IO_OBJECT_NULL;
}

static IOUSBDeviceInterface500 **
open_device(io_service_t svc)
{
	IOUSBDeviceInterface500 **dev = NULL;
	IOCFPlugInInterface **plug = NULL;
	SInt32 score = 0;

	if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
	    kIOCFPlugInInterfaceID, &plug, &score) != KERN_SUCCESS || plug == NULL)
		return NULL;

	if ((*plug)->QueryInterface(plug,
	    CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID *)&dev) != S_OK)
		dev = NULL;
	IODestroyPlugInInterface(plug);

	return dev;
}

/*
 * Take the device away from the HID driver. Everything after this must be
 * undone by release_device(), or the wheel stays gone until it is replugged.
 */
static int
capture_device(long vid, long pid)
{
	IOUSBDeviceInterface500 **dev;
	io_service_t svc;
	IOReturn r;

	if ((svc = find_device(vid, pid)) == IO_OBJECT_NULL) {
		warnx("no USB device matches %04lx:%04lx", vid, pid);
		return -1;
	}
	dev = open_device(svc);
	IOObjectRelease(svc);
	if (dev == NULL) {
		warnx("cannot get a device interface");
		return -1;
	}

	r = (*dev)->USBDeviceOpenSeize(dev);
	if (r != kIOReturnSuccess)
		r = (*dev)->USBDeviceOpen(dev);
	printf("USBDeviceOpen                  %s\n", probe_ioreturn_str(r));
	if (r != kIOReturnSuccess) {
		(*dev)->Release(dev);
		return -1;
	}

	r = (*dev)->USBDeviceReEnumerate(dev, kUSBReEnumerateCaptureDeviceMask);
	printf("capture                        %s\n", probe_ioreturn_str(r));
	(*dev)->USBDeviceClose(dev);
	(*dev)->Release(dev);

	return r == kIOReturnSuccess ? 0 : -1;
}

/*
 * Wait for a device with these ids to appear, up to three seconds. The same
 * poll t150boot does after its own switch, and for the same reason: the wheel
 * detaches, re-enumerates and is matched again, which is not instant.
 */
#define SETTLE_TRIES	60
#define SETTLE_STEP_MS	50

static int
wheel_came_back(long vid, long pid)
{
	io_service_t svc;
	int i;

	for (i = 0; i < SETTLE_TRIES; i++) {
		if ((svc = find_device(vid, pid)) != IO_OBJECT_NULL) {
			IOObjectRelease(svc);
			return 1;
		}
		nap(SETTLE_STEP_MS);
	}

	return 0;
}

static void
release_device(long vid, long pid)
{
	IOUSBDeviceInterface500 **dev;
	io_service_t svc;
	IOReturn r;

	if ((svc = find_device(vid, pid)) == IO_OBJECT_NULL) {
		warnx("cannot find the wheel to hand it back, replug it");
		return;
	}
	if ((dev = open_device(svc)) == NULL) {
		IOObjectRelease(svc);
		warnx("cannot hand the wheel back, replug it");
		return;
	}
	IOObjectRelease(svc);

	if ((*dev)->USBDeviceOpen(dev) == kIOReturnSuccess) {
		r = (*dev)->USBDeviceReEnumerate(dev,
		    kUSBReEnumerateReleaseDeviceMask);
		printf("release                        %s\n",
		    probe_ioreturn_str(r));
		(*dev)->USBDeviceClose(dev);
	}
	(*dev)->Release(dev);
}

static IOUSBInterfaceInterface500 **
open_interface(IOUSBDeviceInterface500 **dev)
{
	IOUSBFindInterfaceRequest req;
	IOUSBInterfaceInterface500 **iface = NULL;
	IOCFPlugInInterface **plug = NULL;
	io_iterator_t iter = IO_OBJECT_NULL;
	io_service_t svc;
	SInt32 score = 0;

	req.bInterfaceClass = kIOUSBFindInterfaceDontCare;
	req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
	req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
	req.bAlternateSetting = kIOUSBFindInterfaceDontCare;

	if ((*dev)->CreateInterfaceIterator(dev, &req, &iter) != KERN_SUCCESS)
		return NULL;
	svc = IOIteratorNext(iter);
	IOObjectRelease(iter);
	if (svc == IO_OBJECT_NULL)
		return NULL;

	if (IOCreatePlugInInterfaceForService(svc,
	    kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug,
	    &score) != KERN_SUCCESS || plug == NULL) {
		IOObjectRelease(svc);
		return NULL;
	}
	IOObjectRelease(svc);

	if ((*plug)->QueryInterface(plug,
	    CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500),
	    (LPVOID *)&iface) != S_OK)
		iface = NULL;
	IODestroyPlugInInterface(plug);

	return iface;
}

/*
 * The first interrupt pipe in each direction. OUT is where the wheel listens
 * and IN is where it reports, and one walk of the endpoints finds both.
 */
static void
find_pipes(IOUSBInterfaceInterface500 **iface, struct pipe *out,
    struct pipe *in)
{
	unsigned int i;
	UInt8 n = 0;

	if ((*iface)->GetNumEndpoints(iface, &n) != kIOReturnSuccess)
		return;

	/*
	 * i counts wider than n on purpose. Both were UInt8, so an interface
	 * claiming 255 endpoints made i wrap from 255 to 0 and the loop never
	 * ended, with the wheel captured and only a kill to escape, which is
	 * exactly what strands it until a replug.
	 */
	for (i = 1; i <= n; i++) {
		UInt8 dir = 0, num = 0, tt = 0, interval = 0;
		UInt16 maxsize = 0;
		struct pipe *p;

		if ((*iface)->GetPipeProperties(iface, (UInt8)i, &dir, &num,
		    &tt, &maxsize, &interval) != kIOReturnSuccess)
			continue;

		printf("  pipe %u: %s, type %u, endpoint 0x%02x, max %u\n", i,
		    dir == kUSBOut ? "out" : dir == kUSBIn ? "in" : "?", tt,
		    (unsigned)(num | (dir == kUSBIn ? 0x80 : 0)), maxsize);

		if (tt != kUSBInterrupt)
			continue;
		p = dir == kUSBOut ? out : dir == kUSBIn ? in : NULL;
		if (p == NULL || p->ref != 0)
			continue;
		p->ref = (UInt8)i;
		p->addr = (UInt8)(num | (dir == kUSBIn ? 0x80 : 0));
	}
}

/*
 * State for the asynchronous read below. The reads have to be asynchronous:
 * IOUSBLib documents ReadPipeTO as bulk only and says it returns
 * kIOReturnBadArgument if timeout values are given for an interrupt pipe, and
 * the synchronous ReadPipe takes no timeout and would block forever on a
 * wheel that reports nothing, which is one of the answers this is looking
 * for. So the read is queued on a run loop that stops on its own.
 */
struct reader {
	IOUSBInterfaceInterface500 **iface;
	uint8_t		buf[MAX_PAYLOAD];
	uint8_t		prev[MAX_PAYLOAD];
	uint8_t		changed[MAX_PAYLOAD];	/* bits that ever moved */
	size_t		prev_len;
	size_t		widest;
	unsigned long	total;
	unsigned long	shown;
	UInt8		ref;
	int		outstanding;	/* a read is queued in the kernel */
	int		stopping;	/* past the deadline, do not re-arm */
	int		varied;		/* reports were not all one length */
	int		failed;
};

/* Each completion queues the next read, so the two refer to each other. */
static void read_done(void *refcon, IOReturn result, void *arg0);

static void
arm_read(struct reader *rd)
{
	IOReturn r;

	r = (*rd->iface)->ReadPipeAsync(rd->iface, rd->ref, rd->buf,
	    (UInt32)sizeof(rd->buf), read_done, rd);
	if (r != kIOReturnSuccess) {
		printf("  ReadPipeAsync                %s\n",
		    probe_ioreturn_str(r));
		rd->failed = 1;
		CFRunLoopStop(CFRunLoopGetCurrent());
		return;
	}
	rd->outstanding = 1;
}

/*
 * One report arrived. Print it only if it differs from the one before,
 * because the wheel streams its state continuously and printing every report
 * would bury the one thing this is for: whether pressing a button changes any
 * bit. Remember which bits ever moved, so that an axis jittering at rest
 * cannot hide the answer in a wall of output.
 */
static void
read_done(void *refcon, IOReturn result, void *arg0)
{
	struct reader *rd = refcon;
	size_t len, i, n;

	rd->outstanding = 0;

	if (result != kIOReturnSuccess) {
		/* Aborted is how the deadline below ends a pending read. */
		if (result != kIOReturnAborted) {
			printf("  read                         %s\n",
			    probe_ioreturn_str(result));
			rd->failed = 1;
		}
		CFRunLoopStop(CFRunLoopGetCurrent());
		return;
	}

	len = (size_t)(uintptr_t)arg0;
	if (len > sizeof(rd->buf))
		len = sizeof(rd->buf);

	rd->total++;
	if (len > rd->widest)
		rd->widest = len;

	if (len != rd->prev_len || memcmp(rd->buf, rd->prev, len) != 0) {
		/*
		 * The mask can only compare the bytes both reports have. A
		 * device that mixes report lengths on one endpoint would leave
		 * the tail uncompared, so say so rather than let a zero there
		 * read as "this never moved".
		 */
		if (rd->prev_len != 0 && len != rd->prev_len)
			rd->varied = 1;

		n = len < rd->prev_len ? len : rd->prev_len;
		for (i = 0; i < n; i++)
			rd->changed[i] |= (uint8_t)(rd->buf[i] ^ rd->prev[i]);

		printf("  read %2zu byte(s):", len);
		probe_hexdump(stdout, rd->buf, len);
		memcpy(rd->prev, rd->buf, len);
		rd->prev_len = len;
		rd->shown++;
	}

	/*
	 * A completion can already be queued when the deadline expires, and it
	 * is dispatched by the drain below. Re-arming from there would start a
	 * read that outlives the interface, so the deadline says stop.
	 */
	if (rd->stopping) {
		CFRunLoopStop(CFRunLoopGetCurrent());
		return;
	}

	arm_read(rd);
}

static int
read_reports(IOUSBInterfaceInterface500 **iface, struct pipe *in,
    unsigned long seconds, enum read_why why)
{
	CFRunLoopSourceRef src = NULL;
	IOReturn r;
	int i;

	/*
	 * Static, not automatic. A queued read has given the kernel a pointer
	 * into this, and the abort below is drained rather than waited on, so
	 * the buffer has to outlive the function even in the case where the
	 * drain gives up. Nothing here is re-entered, so static costs nothing.
	 */
	static struct reader rd;

	memset(&rd, 0, sizeof(rd));
	rd.iface = iface;
	rd.ref = in->ref;

	r = (*iface)->CreateInterfaceAsyncEventSource(iface, &src);
	printf("CreateInterfaceAsyncEventSource %s\n", probe_ioreturn_str(r));
	if (r != kIOReturnSuccess || src == NULL)
		return -1;
	CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);

	printf("\nreading on pipe %u, endpoint 0x%02x for %lu second(s)\n",
	    (unsigned)in->ref, (unsigned)in->addr, seconds);
	if (why == READ_EFFECT)
		printf("only reports that differ from the one before are "
		    "shown.\n**Keep your hands off the wheel.** Anything that "
		    "changes now is the\nwheel moving under its own power, "
		    "which is what the effect was for\n\n");
	else
		printf("only reports that differ from the one before are "
		    "shown, so\nwork every button, the hat and the pedals "
		    "while this runs\n\n");

	arm_read(&rd);
	if (rd.failed == 0)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode,
		    (CFTimeInterval)seconds, 0);

	/*
	 * Stop re-arming before aborting, not after. A completion that was
	 * already queued when the deadline expired is dispatched by the drain,
	 * and without this it would queue another read that outlives both the
	 * interface and this function.
	 */
	rd.stopping = 1;
	(void)(*iface)->AbortPipe(iface, in->ref);
	for (i = 0; rd.outstanding && i < DRAIN_TRIES; i++)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, DRAIN_STEP, 0);

	CFRunLoopRemoveSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
	CFRelease(src);

	if (rd.outstanding)
		warnx("a read is still queued after the abort");

	printf("\n%lu report(s) read, %lu of them different from the one "
	    "before\n", rd.total, rd.shown);

	/*
	 * Only draw a conclusion from a run that worked. A read path that
	 * failed produces no reports either, and reporting that as a silent
	 * wheel is the one wrong answer this tool must never give.
	 */
	if (rd.failed) {
		printf("the read failed, so this run says nothing about the "
		    "wheel either way\n");
	} else if (rd.total == 0) {
		printf("the wheel sent nothing at all in that time, which is "
		    "a result in itself.\nCheck it is still attached before "
		    "believing it\n");
	} else if (rd.shown <= 1) {
		printf("nothing ever changed, so nothing you did reached the "
		    "wire\n");
	} else {
		printf("bits that changed at any point:");
		probe_hexdump(stdout, rd.changed, rd.widest);
		if (rd.varied)
			printf("reports arrived in more than one length, so "
			    "read that mask with care: only the bytes every\n"
			    "report had are compared\n");
		else
			printf("a byte reading 00 there never moved, whatever "
			    "you pressed\n");
	}

	return rd.failed ? -1 : 0;
}

/*
 * The two endpoint 0 transfers, issued while the device is still captured so
 * that nothing re-enumerates between the setup packets and the switch.
 */
static int
mode_switch(IOUSBDeviceInterface500 **dev)
{
	IOUSBDevRequestTO req;
	uint8_t buf[T150_RQ_MODEL_LEN];
	IOReturn r;

	memset(&req, 0, sizeof(req));
	memset(buf, 0, sizeof(buf));
	req.bmRequestType = T150_RQ_MODEL_TYPE;
	req.bRequest = T150_RQ_MODEL;
	req.wLength = T150_RQ_MODEL_LEN;
	req.pData = buf;
	req.noDataTimeout = 1000;
	req.completionTimeout = 1000;

	r = (*dev)->DeviceRequestTO(dev, &req);
	printf("  model query                  %s\n", probe_ioreturn_str(r));
	if (r != kIOReturnSuccess)
		return -1;
	printf("  attachment 0x%02x, model 0x%02x\n",
	    buf[T150_RQ_MODEL_OFF_ATTACH], buf[T150_RQ_MODEL_OFF_MODEL]);

	/*
	 * The switch value selects the model, and this one belongs to the
	 * T150: it means something else entirely to a T300RS or a TMX. Every
	 * other place in this project that can send it says so first, and
	 * probe_ep0 offers -V to override. This sent it to whatever T-series
	 * wheel answered, without a word.
	 */
	if (buf[T150_RQ_MODEL_OFF_MODEL] != T150_MODEL ||
	    buf[T150_RQ_MODEL_OFF_ATTACH] != T150_ATTACHMENT) {
		printf("  not the T150 row, so the T150's switch value is not "
		    "sent.\n  Use probe_ep0 -w -V <value> for another "
		    "model.\n");
		return -1;
	}

	memset(&req, 0, sizeof(req));
	req.bmRequestType = T150_RQ_SWITCH_TYPE;
	req.bRequest = T150_RQ_SWITCH;
	req.wValue = T150_SWITCH_VALUE;
	req.noDataTimeout = 1000;
	req.completionTimeout = 1000;

	r = (*dev)->DeviceRequestTO(dev, &req);
	printf("  mode switch                  %s\n", probe_ioreturn_str(r));

	/*
	 * The wheel leaves before it can answer, which is the normal case, and
	 * the host sees that departure as any of three things depending on how
	 * far the transfer had got. Measured on a T150: kIOUSBPipeStalled, on a
	 * switch that worked and re-enumerated at the firmware id. Treating it
	 * as a failure printed a replug warning for a wheel that was fine.
	 */
	return (r == kIOReturnSuccess || r == kIOReturnNotResponding ||
	    r == kIOUSBPipeStalled) ? 0 : -1;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: probe_intr [-v vid] [-p pid] [-N pad]\n"
	    "                  [-H seconds]\n"
	    "                  [-I | -O | -C | -a force | -A | -r degrees |\n"
	    "                   -g gain | -R seconds | -x \"hex bytes\"]\n"
	    "\n"
	    "  Writes on the interrupt OUT pipe rather than through the HID\n"
	    "  layer, which means capturing the wheel from macOS and handing\n"
	    "  it back afterwards. Needs root.\n"
	    "\n"
	    "  -v vid       vendor id (default 0x%04x)\n"
	    "  -p pid       product id (default 0x%04x)\n"
	    "  -N pad       zero-pad every packet to this length\n"
	    "\n"
	    "  -I           send the initialisation the Linux driver sends on\n"
	    "               this pipe, then the mode switch, in one capture.\n"
	    "               Run it on a wheel still at the boot id 0x%04x.\n"
	    "  -O           open the wheel's input, 42 04. The firmware tracks\n"
	    "               whether an application has the input open and the\n"
	    "               autocenter is unconditionally active while none\n"
	    "               has, so effects may be gated the same way. Combine\n"
	    "               with -H, or nothing holds the input open\n"
	    "  -C           close the wheel's input, 42 00\n"
	    "  -a force     autocenter to force (0..10000) and enable it\n"
	    "  -A           clear the autocenter enable flag. Note this does\n"
	    "               not free a held wheel: the autocenter is always\n"
	    "               active while nothing has the input open, so use\n"
	    "               -a 0 for that, which is also the default\n"
	    "  -r degrees   set rotation range (%u..%u)\n"
	    "  -g gain      set gain (0..10000)\n"
	    "  -R seconds   write nothing, read the interrupt IN pipe instead\n"
	    "               and print the reports that change (default %d).\n"
	    "               This is what says whether the buttons reach the\n"
	    "               wire at all, independently of what Wine makes of\n"
	    "               them\n"
	    "  -H seconds   after writing, hold the wheel this long instead of\n"
	    "               handing it straight back, reading the IN pipe while\n"
	    "               it waits. An uploaded effect does not survive the\n"
	    "               release, so a force feedback test needs this\n"
	    "  -x \"40 04 ..\"  send these raw bytes, repeatable up to %d times\n",
	    T150_VID, T150_PID_FIRMWARE, T150_PID_BOOT, T150_RANGE_MIN,
	    T150_RANGE_MAX, READ_SECONDS, MAX_PACKETS);
	exit(2);
}

int
main(int argc, char *argv[])
{
	struct packet pkt[MAX_PACKETS];
	IOUSBDeviceInterface500 **dev = NULL;
	IOUSBInterfaceInterface500 **iface = NULL;
	struct pipe out, in;
	io_service_t svc;
	unsigned long arg = 10000, padto = 0, seconds = READ_SECONDS;
	unsigned long hold = 0;
	long vid = T150_VID, pid = T150_PID_FIRMWARE;
	enum action act = ACT_NONE;
	size_t npkt = 0, i;
	int ch, rc = 1, raw_len;

	memset(pkt, 0, sizeof(pkt));
	memset(&out, 0, sizeof(out));
	memset(&in, 0, sizeof(in));

	while ((ch = getopt(argc, argv, "v:p:N:IOCa:Ar:g:R:H:x:")) != -1) {
		unsigned long parsed;

		switch (ch) {
		case 'v':
			if (probe_parse_uint(optarg, 0xffff, &parsed) != 0)
				usage();
			vid = (long)parsed;
			break;
		case 'p':
			if (probe_parse_uint(optarg, 0xffff, &parsed) != 0)
				usage();
			pid = (long)parsed;
			break;
		case 'N':
			if (probe_parse_uint(optarg, MAX_PAYLOAD, &padto) != 0)
				usage();
			break;
		case 'a':
			if (act != ACT_NONE ||
			    probe_parse_uint(optarg, T150_DI_MAX, &arg) != 0)
				usage();
			act = ACT_AUTOCENTER;
			break;
		case 'A':
			if (act != ACT_NONE)
				usage();
			act = ACT_AUTOCENTER_OFF;
			break;
		case 'I':
			if (act != ACT_NONE)
				usage();
			act = ACT_INIT;
			/* The wheel is at the boot id until this succeeds. */
			pid = T150_PID_BOOT;
			break;
		case 'r':
			if (act != ACT_NONE ||
			    probe_parse_uint(optarg, T150_RANGE_MAX, &arg) != 0)
				usage();
			act = ACT_RANGE;
			break;
		case 'g':
			if (act != ACT_NONE ||
			    probe_parse_uint(optarg, T150_DI_MAX, &arg) != 0)
				usage();
			act = ACT_GAIN;
			break;
		case 'R':
			if (act != ACT_NONE ||
			    probe_parse_uint(optarg, 3600, &seconds) != 0)
				usage();
			act = ACT_READ;
			break;
		case 'H':
			if (probe_parse_uint(optarg, 3600, &hold) != 0)
				usage();
			break;
		case 'O':
			if (act != ACT_NONE)
				usage();
			act = ACT_INPUT_OPEN;
			break;
		case 'C':
			if (act != ACT_NONE)
				usage();
			act = ACT_INPUT_CLOSE;
			break;
		case 'x':
			if (act != ACT_NONE && act != ACT_RAW)
				usage();
			if (npkt >= MAX_PACKETS)
				errx(2, "at most %d -x packets", MAX_PACKETS);
			if ((raw_len = probe_parse_hex(optarg, pkt[npkt].bytes,
			    sizeof(pkt[npkt].bytes))) <= 0)
				usage();
			pkt[npkt].len = (size_t)raw_len;
			npkt++;
			act = ACT_RAW;
			break;
		default:
			usage();
		}
	}
	if (optind != argc)
		usage();

	if (act == ACT_NONE) {
		/*
		 * Autocenter force zero, not the enable flag. The wheel's
		 * autocenter is always active while nothing has its input
		 * open, which on macOS is always, so clearing the flag
		 * changes nothing and only the strength does.
		 */
		act = ACT_AUTOCENTER;
		arg = 0;
		printf("no action given, so setting the autocenter to zero\n");
	}

	/*
	 * The bytes come from the shared encoders, so whatever this proves
	 * about the wheel it proves about src/lib/encode.c as well.
	 */
	switch (act) {
	case ACT_INIT:
		for (npkt = 0; npkt < sizeof(init_lens) / sizeof(init_lens[0]);
		    npkt++) {
			memcpy(pkt[npkt].bytes, init_pkts[npkt],
			    init_lens[npkt]);
			pkt[npkt].len = init_lens[npkt];
		}
		break;
	case ACT_AUTOCENTER:
		pkt[0].len = t150_enc_autocenter_force(pkt[0].bytes,
		    sizeof(pkt[0].bytes), (uint32_t)arg);
		pkt[1].len = t150_enc_autocenter_enable(pkt[1].bytes,
		    sizeof(pkt[1].bytes), 1);
		npkt = 2;
		break;
	case ACT_AUTOCENTER_OFF:
		pkt[0].len = t150_enc_autocenter_enable(pkt[0].bytes,
		    sizeof(pkt[0].bytes), 0);
		npkt = 1;
		break;
	case ACT_RANGE:
		pkt[0].len = t150_enc_range(pkt[0].bytes, sizeof(pkt[0].bytes),
		    (unsigned int)arg);
		npkt = 1;
		break;
	case ACT_GAIN:
		pkt[0].len = t150_enc_gain(pkt[0].bytes, sizeof(pkt[0].bytes),
		    (uint32_t)arg);
		npkt = 1;
		break;
	case ACT_INPUT_OPEN:
		pkt[0].len = t150_enc_input_open(pkt[0].bytes,
		    sizeof(pkt[0].bytes));
		npkt = 1;
		break;
	case ACT_INPUT_CLOSE:
		pkt[0].len = t150_enc_input_close(pkt[0].bytes,
		    sizeof(pkt[0].bytes));
		npkt = 1;
		break;
	case ACT_RAW:
	case ACT_READ:
		break;
	case ACT_NONE:
		usage();
	}

	if (geteuid() != 0)
		warnx("not running as root: capturing the device will fail");

	printf("capturing %04lx:%04lx from the HID driver\n", vid, pid);
	if (capture_device(vid, pid) != 0) {
		warnx("capture failed, the wheel was not taken and nothing was "
		    "written");
		return 1;
	}

	/* From here on every exit goes through release. */
	if ((svc = wait_for_device(vid, pid)) == IO_OBJECT_NULL) {
		warnx("the wheel did not come back after the capture");
		goto out;
	}
	dev = open_device(svc);
	IOObjectRelease(svc);
	if (dev == NULL) {
		warnx("cannot reopen the captured wheel");
		goto out;
	}
	if ((*dev)->USBDeviceOpen(dev) != kIOReturnSuccess) {
		warnx("cannot open the captured wheel");
		goto out;
	}
	(void)(*dev)->SetConfiguration(dev, 1);

	if ((iface = open_interface(dev)) == NULL) {
		warnx("cannot get interface 0");
		goto out;
	}
	printf("USBInterfaceOpen               %s\n",
	    probe_ioreturn_str((*iface)->USBInterfaceOpen(iface)));

	find_pipes(iface, &out, &in);

	if (act == ACT_READ) {
		if (in.ref == 0) {
			warnx("no interrupt IN pipe on this interface");
			goto out;
		}
		rc = read_reports(iface, &in, seconds, READ_CONTROLS) == 0 ?
		    0 : 1;
		goto out;
	}

	if (out.ref == 0) {
		warnx("no interrupt OUT pipe on this interface");
		goto out;
	}
	printf("\nwriting on pipe %u, endpoint 0x%02x\n", out.ref, out.addr);

	if (act == ACT_INIT)
		printf("sending the initialisation the Linux driver sends\n");

	rc = 0;
	for (i = 0; i < npkt; i++) {
		size_t len = pkt[i].len;
		IOReturn r;

		if (len == 0) {
			warnx("packet %zu is empty, the encoder refused it",
			    i);
			rc = 1;
			break;
		}
		if (padto > len)
			len = padto;

		printf("  send %2zu byte(s):", len);
		probe_hexdump(stdout, pkt[i].bytes, len);
		r = (*iface)->WritePipe(iface, out.ref, pkt[i].bytes,
		    (UInt32)len);
		printf("  WritePipe                    %s\n",
		    probe_ioreturn_str(r));
		if (r != kIOReturnSuccess) {
			rc = 1;
			break;
		}
	}

	/*
	 * Hold the device rather than handing it straight back. An uploaded
	 * effect does not survive the release, because the release
	 * re-enumerates the wheel, so without this an effect that worked
	 * perfectly would still look like nothing happened. Akellacom's T300RS
	 * driver keeps its session open for exactly this reason and pumps a
	 * read while it does; reading the IN pipe here does both jobs at once,
	 * because a wheel that starts pushing also starts reporting movement.
	 */
	if (rc == 0 && hold > 0 && act != ACT_INIT) {
		printf("\nholding the wheel for %lu second(s) before handing "
		    "it back\n", hold);
		if (in.ref != 0)
			(void)read_reports(iface, &in, hold, READ_EFFECT);
		else
			nap((long)hold * 1000);
	}

	/*
	 * Still captured, so nothing re-enumerates between the setup packets
	 * and the switch. That ordering is the point of doing both here.
	 */
	if (rc == 0 && act == ACT_INIT) {
		printf("\nswitching to firmware mode\n");
		if (mode_switch(dev) != 0)
			rc = 1;
	}

out:
	if (iface != NULL) {
		(void)(*iface)->USBInterfaceClose(iface);
		(*iface)->Release(iface);
	}
	if (dev != NULL) {
		(*dev)->USBDeviceClose(dev);
		(*dev)->Release(dev);
	}
	/*
	 * After a successful switch the wheel has already detached and is
	 * coming back under a different product id, so there is nothing to
	 * hand back and looking for it only produces a false alarm.
	 */
	/*
	 * After a successful switch the wheel has already detached and is
	 * coming back under a different product id, so there is nothing to
	 * hand back. Whether it really went is a question with an answer:
	 * look for it at the firmware id, the way t150boot does, rather than
	 * inferring it from the transfer status.
	 *
	 * The inference was wrong in one direction and stranded the hardware
	 * when it was. A control endpoint stall is also the standard way a
	 * device refuses a request it will not honour, and a device that
	 * stalls stays firmly on the bus; the wheel was still captured, so
	 * macOS could not see it and only a replug brought it back.
	 */
	if (rc == 0 && act == ACT_INIT &&
	    wheel_came_back(vid, T150_PID_FIRMWARE)) {
		printf("\nthe wheel has left and come back at 0x%04x, so "
		    "there is nothing to hand back\n", T150_PID_FIRMWARE);
	} else {
		printf("\nhanding the wheel back to macOS\n");
		release_device(vid, pid);
	}

	if (rc == 0 && act == ACT_INIT)
		printf("\nThe wheel should have re-enumerated at 0x%04x, and\n"
		    "this time it was initialised first. Check with probe_hid,\n"
		    "then try turning it by hand.\n", T150_PID_FIRMWARE);
	else if (rc == 0 && act == ACT_READ)
		printf("\nA line per state change means the wheel puts buttons\n"
		    "on the wire and anything that loses them is above the\n"
		    "USB layer. No line for any button means the wheel.\n");
	else if (rc == 0)
		printf("\nEvery write was accepted on the pipe the firmware\n"
		    "listens to. What settles this is whether the wheel\n"
		    "reacted: with -a 0, whether it became free to turn.\n");

	return rc;
}
