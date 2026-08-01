/*
 * probe_ep0 - can we issue the Thrustmaster mode switch, and as whom?
 *
 * The T-series boot to firmware switch is a pair of vendor control transfers
 * on endpoint 0. No HID API can express them, so this is the one operation
 * with no userspace HID equivalent and the one place root may be
 * unavoidable.
 *
 * Both requests are directed at interface 0, which is exactly the interface
 * macOS's own HID driver owns, so there is a real chance they are refused.
 * Apple's IOUSBLib contract says a device must be open for DeviceRequest and
 * that opening returns kIOReturnExclusiveAccess when another task holds it.
 * libusb nonetheless issues some requests on an unopened device, so this
 * tool tries in three escalating steps and prints the IOReturn of every one,
 * rather than assuming which will work.
 *
 * Run it twice, once as your user and once under sudo, and compare.
 *
 * By default it only performs the read-only model query. It does not switch
 * the wheel unless -w is given.
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

static IOReturn
model_query(IOUSBDeviceInterface500 **dev, uint8_t *buf, size_t buflen,
    UInt32 *done)
{
	IOUSBDevRequestTO req;

	memset(&req, 0, sizeof(req));
	memset(buf, 0, buflen);

	req.bmRequestType = T150_RQ_MODEL_TYPE;
	req.bRequest = T150_RQ_MODEL;
	req.wValue = 0;
	req.wIndex = 0;
	req.wLength = (UInt16)buflen;
	req.pData = buf;
	req.noDataTimeout = 1000;
	req.completionTimeout = 1000;

	*done = 0;
	{
		IOReturn r = (*dev)->DeviceRequestTO(dev, &req);

		*done = req.wLenDone;
		return r;
	}
}

static IOReturn
mode_switch(IOUSBDeviceInterface500 **dev, uint16_t value)
{
	IOUSBDevRequestTO req;

	memset(&req, 0, sizeof(req));

	req.bmRequestType = T150_RQ_SWITCH_TYPE;
	req.bRequest = T150_RQ_SWITCH;
	req.wValue = value;
	req.wIndex = 0;
	req.wLength = 0;
	req.pData = NULL;
	req.noDataTimeout = 1000;
	req.completionTimeout = 1000;

	return (*dev)->DeviceRequestTO(dev, &req);
}

static void
report_model(const uint8_t *buf, size_t len, UInt32 done)
{
	unsigned int attach, model;

	printf("  response %u byte(s):", (unsigned int)done);
	probe_hexdump(stdout, buf, len);

	if (done <= T150_RQ_MODEL_OFF_MODEL) {
		printf("  response too short to carry model and attachment\n");
		return;
	}

	attach = buf[T150_RQ_MODEL_OFF_ATTACH];
	model = buf[T150_RQ_MODEL_OFF_MODEL];
	printf("  attachment 0x%02x, model 0x%02x", attach, model);
	if (model == T150_MODEL && attach == T150_ATTACHMENT)
		printf("   (T150, switch value 0x%04x)\n", T150_SWITCH_VALUE);
	else
		printf("   (not the T150 row, pass the switch value with "
		    "-V)\n");
}

static CFMutableDictionaryRef
make_match(long vid, long pid)
{
	CFMutableDictionaryRef d;
	CFNumberRef n;
	SInt32 v;

	if ((d = IOServiceMatching(kIOUSBDeviceClassName)) == NULL)
		return NULL;

	v = (SInt32)vid;
	if ((n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
	    &v)) == NULL) {
		CFRelease(d);
		return NULL;
	}
	CFDictionarySetValue(d, CFSTR(kUSBVendorID), n);
	CFRelease(n);

	v = (SInt32)pid;
	if ((n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
	    &v)) == NULL) {
		CFRelease(d);
		return NULL;
	}
	CFDictionarySetValue(d, CFSTR(kUSBProductID), n);
	CFRelease(n);

	return d;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: probe_ep0 [-v vid] [-p pid] [-s] [-w] [-V value]\n"
	    "  -v vid     vendor id (default 0x%04x)\n"
	    "  -p pid     product id (default 0x%04x, T-series boot mode)\n"
	    "  -s         also try USBDeviceOpenSeize if USBDeviceOpen fails\n"
	    "  -w         actually send the mode switch, not just the query\n"
	    "  -V value   switch value to use with -w (default 0x%04x, T150)\n"
	    "\n"
	    "Without -w this tool only reads. Run it as your user first, then "
	    "under sudo.\n",
	    T150_VID, T150_PID_BOOT, T150_SWITCH_VALUE);
	exit(2);
}

int
main(int argc, char *argv[])
{
	CFMutableDictionaryRef match;
	io_iterator_t iter = IO_OBJECT_NULL;
	io_service_t service;
	IOCFPlugInInterface **plugin = NULL;
	IOUSBDeviceInterface500 **dev = NULL;
	kern_return_t kr;
	IOReturn r;
	SInt32 score = 0;
	UInt32 done = 0;
	unsigned long parsed;
	long vid = T150_VID, pid = T150_PID_BOOT;
	uint16_t switch_value = T150_SWITCH_VALUE;
	uint8_t buf[T150_RQ_MODEL_LEN];
	int ch, seize = 0, do_switch = 0, opened = 0, queried = 0, rc = 1;

	while ((ch = getopt(argc, argv, "v:p:swV:")) != -1) {
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
		case 's':
			seize = 1;
			break;
		case 'w':
			do_switch = 1;
			break;
		case 'V':
			if (probe_parse_uint(optarg, 0xffff, &parsed) != 0)
				usage();
			switch_value = (uint16_t)parsed;
			break;
		default:
			usage();
		}
	}
	if (optind != argc)
		usage();

	printf("running as uid %u\n\n", (unsigned int)geteuid());

	if ((match = make_match(vid, pid)) == NULL)
		errx(1, "cannot build the matching dictionary");

	kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter);
	if (kr != KERN_SUCCESS)
		errx(1, "IOServiceGetMatchingServices: 0x%08x",
		    (unsigned int)kr);

	if ((service = IOIteratorNext(iter)) == IO_OBJECT_NULL) {
		IOObjectRelease(iter);
		errx(1, "no USB device matches %04lx:%04lx", vid, pid);
	}

	kr = IOCreatePlugInInterfaceForService(service,
	    kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,
	    &score);
	IOObjectRelease(service);
	if (kr != KERN_SUCCESS || plugin == NULL) {
		IOObjectRelease(iter);
		errx(1, "IOCreatePlugInInterfaceForService: 0x%08x",
		    (unsigned int)kr);
	}

	if ((*plugin)->QueryInterface(plugin,
	    CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),
	    (LPVOID *)&dev) != S_OK || dev == NULL) {
		IODestroyPlugInInterface(plugin);
		IOObjectRelease(iter);
		errx(1, "QueryInterface for IOUSBDeviceInterface500 failed");
	}

	/*
	 * Step 1: the request without opening. libusb issues some control
	 * requests on an unopened device, so this may just work.
	 */
	printf("step 1: model query with the device unopened\n");
	r = model_query(dev, buf, sizeof(buf), &done);
	printf("  DeviceRequestTO              %s\n", probe_ioreturn_str(r));
	if (r == kIOReturnSuccess) {
		report_model(buf, sizeof(buf), done);
		queried = 1;
		rc = 0;
	}

	/* Step 2: the documented path, exclusive open then request. */
	if (!queried) {
		printf("\nstep 2: USBDeviceOpen, then the model query\n");
		r = (*dev)->USBDeviceOpen(dev);
		printf("  USBDeviceOpen                %s\n",
		    probe_ioreturn_str(r));
		if (r == kIOReturnSuccess) {
			opened = 1;
			r = model_query(dev, buf, sizeof(buf), &done);
			printf("  DeviceRequestTO              %s\n",
			    probe_ioreturn_str(r));
			if (r == kIOReturnSuccess) {
				report_model(buf, sizeof(buf), done);
				queried = 1;
				rc = 0;
			}
		}
	}

	/*
	 * Step 3: seize. This takes the device away from whatever holds it,
	 * so it is opt in. If this is the only step that works, the daemon
	 * has to seize too, and the wheel will disappear from CrossOver for
	 * as long as it does.
	 */
	if (!queried && seize) {
		printf("\nstep 3: USBDeviceOpenSeize, then the model query\n");
		r = (*dev)->USBDeviceOpenSeize(dev);
		printf("  USBDeviceOpenSeize           %s\n",
		    probe_ioreturn_str(r));
		if (r == kIOReturnSuccess) {
			opened = 1;
			r = model_query(dev, buf, sizeof(buf), &done);
			printf("  DeviceRequestTO              %s\n",
			    probe_ioreturn_str(r));
			if (r == kIOReturnSuccess) {
				report_model(buf, sizeof(buf), done);
				queried = 1;
				rc = 0;
			}
		}
	}

	if (queried && do_switch) {
		printf("\nsending the mode switch, value 0x%04x\n",
		    switch_value);
		r = mode_switch(dev, switch_value);
		printf("  DeviceRequestTO              %s\n",
		    probe_ioreturn_str(r));
		/*
		 * kIOReturnNotResponding is the expected answer, not a
		 * failure. The wheel detaches the moment it accepts the
		 * switch, so it is gone before the completion can come back.
		 * Only probe_hid can say whether it worked.
		 */
		if (r == kIOReturnSuccess || r == kIOReturnNotResponding)
			printf("  the wheel should now detach and come back "
			    "at 0x%04x, check with probe_hid.\n"
			    "  %s\n", T150_PID_FIRMWARE,
			    r == kIOReturnNotResponding ?
			    "not responding is expected here: it left before "
			    "it could answer." :
			    "it answered before leaving, which is unusual but "
			    "fine.");
		else
			rc = 1;
	}

	if (!queried)
		printf("\nThe model query never succeeded. If this was an "
		    "unprivileged run, try\nagain under sudo, and then with "
		    "-s, before concluding endpoint 0 is closed.\n");

	if (opened)
		(void)(*dev)->USBDeviceClose(dev);
	(void)(*dev)->Release(dev);
	IODestroyPlugInInterface(plugin);
	IOObjectRelease(iter);

	return rc;
}
