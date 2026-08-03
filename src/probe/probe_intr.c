/*
 * probe_intr - write to the wheel on the pipe it actually listens to.
 *
 * probe_setreport asks whether IOHIDDeviceSetReport moves the wheel. On a
 * T150 the answer so far is that every write is accepted and nothing ever
 * happens, which RESEARCH.md C7 explains: Thrustmaster firmware acknowledges
 * the control SET_REPORT pipe and ignores it, and the pipe it listens to is
 * interrupt OUT.
 *
 * Reaching that pipe from userspace means taking the device away from the
 * HID driver, which is what this does: capture, write, release. That is the
 * price, and it is why this cannot be how the daemon drives effects during a
 * game. It is fine for settings, which the wheel keeps after the device goes
 * back, and it is the experiment that says whether the bytes in
 * docs/PROTOCOL.md are right.
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

/* Short enough that -R still stops on time between reports. */
#define READ_TIMEOUT_MS	100
#define READ_SECONDS	15

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
	ACT_READ
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
	UInt8 n = 0, i;

	if ((*iface)->GetNumEndpoints(iface, &n) != kIOReturnSuccess)
		return;

	for (i = 1; i <= n; i++) {
		UInt8 dir = 0, num = 0, tt = 0, interval = 0;
		UInt16 maxsize = 0;
		struct pipe *p;

		if ((*iface)->GetPipeProperties(iface, i, &dir, &num, &tt,
		    &maxsize, &interval) != kIOReturnSuccess)
			continue;

		printf("  pipe %u: %s, type %u, endpoint 0x%02x, max %u\n", i,
		    dir == kUSBOut ? "out" : dir == kUSBIn ? "in" : "?", tt,
		    (unsigned)(num | (dir == kUSBIn ? 0x80 : 0)), maxsize);

		if (tt != kUSBInterrupt)
			continue;
		p = dir == kUSBOut ? out : dir == kUSBIn ? in : NULL;
		if (p == NULL || p->ref != 0)
			continue;
		p->ref = i;
		p->addr = (UInt8)(num | (dir == kUSBIn ? 0x80 : 0));
	}
}

/*
 * Read the interrupt IN pipe and print only the reports that differ from the
 * one before. The wheel streams its state continuously, so printing every
 * report would bury the one thing this is for: whether pressing a button
 * changes any bit. A press shows up as one new line.
 */
static int
read_reports(IOUSBInterfaceInterface500 **iface, struct pipe *in,
    unsigned long seconds)
{
	uint8_t buf[MAX_PAYLOAD], prev[MAX_PAYLOAD];
	unsigned long total = 0, shown = 0;
	struct timespec start, now;
	size_t prev_len = 0;

	if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
		warnx("clock_gettime failed");
		return -1;
	}

	printf("\nreading on pipe %u, endpoint 0x%02x for %lu second(s)\n",
	    in->ref, in->addr, seconds);
	printf("only reports that differ from the one before are shown, so\n"
	    "work every button, the hat and the pedals while this runs\n\n");

	memset(prev, 0, sizeof(prev));

	for (;;) {
		UInt32 size = (UInt32)sizeof(buf);
		IOReturn r;

		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
			break;
		if ((unsigned long)(now.tv_sec - start.tv_sec) >= seconds)
			break;

		r = (*iface)->ReadPipeTO(iface, in->ref, buf, &size,
		    READ_TIMEOUT_MS, READ_TIMEOUT_MS);
		if (r == kIOReturnTimeout || r == kIOUSBTransactionTimeout)
			continue;
		if (r != kIOReturnSuccess) {
			printf("  ReadPipeTO                   %s\n",
			    probe_ioreturn_str(r));
			return -1;
		}
		if (size > sizeof(buf))
			size = (UInt32)sizeof(buf);

		total++;
		if (size == prev_len && memcmp(buf, prev, prev_len) == 0)
			continue;

		printf("  read %2u byte(s):", (unsigned)size);
		probe_hexdump(stdout, buf, size);
		memcpy(prev, buf, size);
		prev_len = size;
		shown++;
	}

	printf("\n%lu report(s) read, %lu of them different from the one "
	    "before\n", total, shown);
	if (total == 0)
		printf("the wheel sent nothing at all, which is a result in "
		    "itself\n");

	return 0;
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

	memset(&req, 0, sizeof(req));
	req.bmRequestType = T150_RQ_SWITCH_TYPE;
	req.bRequest = T150_RQ_SWITCH;
	req.wValue = T150_SWITCH_VALUE;
	req.noDataTimeout = 1000;
	req.completionTimeout = 1000;

	r = (*dev)->DeviceRequestTO(dev, &req);
	printf("  mode switch                  %s\n", probe_ioreturn_str(r));

	/* The wheel leaves before it can answer, which is the normal case. */
	return (r == kIOReturnSuccess || r == kIOReturnNotResponding) ? 0 : -1;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: probe_intr [-v vid] [-p pid] [-N pad]\n"
	    "                  [-I | -a force | -A | -r degrees | -g gain |\n"
	    "                   -R seconds | -x \"hex bytes\"]\n"
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
	long vid = T150_VID, pid = T150_PID_FIRMWARE;
	enum action act = ACT_NONE;
	size_t npkt = 0, i;
	int ch, rc = 1, raw_len;

	memset(pkt, 0, sizeof(pkt));
	memset(&out, 0, sizeof(out));
	memset(&in, 0, sizeof(in));

	while ((ch = getopt(argc, argv, "v:p:N:Ia:Ar:g:R:x:")) != -1) {
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
		rc = read_reports(iface, &in, seconds) == 0 ? 0 : 1;
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
	if (rc == 0 && act == ACT_INIT) {
		printf("\nthe wheel has left to re-enumerate, so there is "
		    "nothing to hand back\n");
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
