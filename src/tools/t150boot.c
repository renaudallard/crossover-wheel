/*
 * t150boot - take the wheel out of boot mode.
 *
 * Every T-series wheel enumerates at the shared boot product id 044f:b65d and
 * only reveals its own identity, and its own protocol, after a pair of vendor
 * control transfers on endpoint 0. Nothing else in this project works until
 * that has happened, and sleep, wake and a replug all put the wheel back, so
 * this is meant to run on every plug-in rather than once.
 *
 * It needs no privilege. Both transfers were measured succeeding as an
 * ordinary user with the device unopened, which is RESEARCH.md A6, so this
 * can be a LaunchAgent that fires on the wheel appearing and never asks for a
 * password.
 *
 * It never claims a USB interface. An endpoint 0 device request does not need
 * one, claiming the interface macOS's HID driver already owns would be
 * refused, and libusb_claim_interface is currently reported to panic macOS
 * 26.3 on Apple silicon.
 *
 * What it does NOT do is send the five initialisation packets the Linux
 * driver sends on the interrupt OUT pipe first. Those need the device
 * captured, which needs root, and probe_intr -I is the tool for it. Whether
 * they matter is unsettled: they were adopted here to explain a wheel that
 * came back "blocked", and that turned out to be the autocenter holding at
 * full strength rather than anything the switch did or did not do. See
 * RESEARCH.md A13 and A15.
 *
 * macOS only.
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
#include <unistd.h>

#include "common.h"
#include "t150/t150.h"

static void
usage(void)
{
	fprintf(stderr,
	    "usage: t150boot [-nq] [-v vid] [-p pid] [-V value]\n"
	    "\n"
	    "  -v vid       vendor id (default 0x%04x)\n"
	    "  -p pid       boot product id (default 0x%04x)\n"
	    "  -V value     switch value (default 0x%04x, the T150 row)\n"
	    "  -n           query the model and stop, switching nothing\n"
	    "  -q           say nothing unless something goes wrong\n"
	    "\n"
	    "  Needs no privilege. Exits 0 if the wheel was switched or was\n"
	    "  already out of boot mode, so it is safe to run on every\n"
	    "  plug-in.\n",
	    T150_VID, T150_PID_BOOT, T150_SWITCH_VALUE);
	exit(2);
}

static io_service_t
find_usb(long vid, long pid)
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

static IOUSBDeviceInterface500 **
open_device(io_service_t svc)
{
	IOUSBDeviceInterface500 **dev = NULL;
	IOCFPlugInInterface **plug = NULL;
	SInt32 score = 0;

	if (IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
	    kIOCFPlugInInterfaceID, &plug, &score) != KERN_SUCCESS ||
	    plug == NULL)
		return NULL;

	if ((*plug)->QueryInterface(plug,
	    CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),
	    (LPVOID *)&dev) != S_OK)
		dev = NULL;
	IODestroyPlugInInterface(plug);

	return dev;
}

static IOReturn
model_query(IOUSBDeviceInterface500 **dev, uint8_t *buf, size_t buflen,
    UInt32 *done)
{
	IOUSBDevRequestTO req;
	IOReturn r;

	memset(&req, 0, sizeof(req));
	memset(buf, 0, buflen);

	req.bmRequestType = T150_RQ_MODEL_TYPE;
	req.bRequest = T150_RQ_MODEL;
	req.wLength = (UInt16)buflen;
	req.pData = buf;
	req.noDataTimeout = 1000;
	req.completionTimeout = 1000;

	r = (*dev)->DeviceRequestTO(dev, &req);
	*done = req.wLenDone;

	return r;
}

static IOReturn
mode_switch(IOUSBDeviceInterface500 **dev, uint16_t value)
{
	IOUSBDevRequestTO req;

	memset(&req, 0, sizeof(req));

	req.bmRequestType = T150_RQ_SWITCH_TYPE;
	req.bRequest = T150_RQ_SWITCH;
	req.wValue = value;
	req.noDataTimeout = 1000;
	req.completionTimeout = 1000;

	return (*dev)->DeviceRequestTO(dev, &req);
}

/*
 * The wheel leaves the bus the instant it accepts the switch, so it is gone
 * before the transfer can complete. Which error that surfaces as depends on
 * how far the request had got, and none of them mean failure.
 */
static int
left_the_bus(IOReturn r)
{
	return r == kIOReturnNotResponding || r == kIOUSBPipeStalled ||
	    r == kIOReturnNoDevice || r == kIOReturnAborted;
}

int
main(int argc, char *argv[])
{
	IOUSBDeviceInterface500 **dev;
	io_service_t svc;
	uint8_t buf[T150_RQ_MODEL_LEN];
	unsigned long parsed;
	long vid = T150_VID, pid = T150_PID_BOOT;
	UInt32 done = 0;
	IOReturn r;
	unsigned int value = T150_SWITCH_VALUE;
	int ch, query_only = 0, quiet = 0, rc = 1;

	while ((ch = getopt(argc, argv, "nqv:p:V:")) != -1) {
		switch (ch) {
		case 'n':
			query_only = 1;
			break;
		case 'q':
			quiet = 1;
			break;
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
		case 'V':
			if (probe_parse_uint(optarg, 0xffff, &parsed) != 0)
				usage();
			value = (unsigned int)parsed;
			break;
		default:
			usage();
		}
	}
	if (optind != argc)
		usage();

	if ((svc = find_usb(vid, pid)) == IO_OBJECT_NULL) {
		/*
		 * Nothing at the boot id is the normal outcome once the wheel
		 * has been switched, so a LaunchAgent that fires on every
		 * plug-in must not treat it as a failure.
		 */
		if (!quiet)
			printf("no wheel at %04lx:%04lx, nothing to switch\n",
			    vid, pid);
		return 0;
	}

	dev = open_device(svc);
	IOObjectRelease(svc);
	if (dev == NULL)
		errx(1, "cannot get a device interface for %04lx:%04lx",
		    vid, pid);

	/*
	 * Deliberately without opening the device. The request is a device
	 * request on endpoint 0 and was measured working this way, and
	 * opening would fail anyway while macOS's HID driver holds it.
	 */
	r = model_query(dev, buf, sizeof(buf), &done);
	if (r != kIOReturnSuccess) {
		warnx("the model query failed: %s", probe_ioreturn_str(r));
		goto out;
	}
	if (done <= T150_RQ_MODEL_OFF_MODEL) {
		warnx("the model query returned only %u byte(s)",
		    (unsigned int)done);
		goto out;
	}

	if (!quiet)
		printf("attachment 0x%02x, model 0x%02x%s\n",
		    buf[T150_RQ_MODEL_OFF_ATTACH], buf[T150_RQ_MODEL_OFF_MODEL],
		    buf[T150_RQ_MODEL_OFF_MODEL] == T150_MODEL ? "  T150" :
		    "  not a T150, check -V against hid-tminit's table");

	if (query_only) {
		rc = 0;
		goto out;
	}

	r = mode_switch(dev, (uint16_t)value);
	if (r == kIOReturnSuccess || left_the_bus(r)) {
		if (!quiet)
			printf("switched, the wheel is re-enumerating at "
			    "0x%04x\n", T150_PID_FIRMWARE);
		rc = 0;
	} else {
		warnx("the mode switch failed: %s", probe_ioreturn_str(r));
	}

out:
	(*dev)->Release(dev);

	return rc;
}
