/*
 * bootswitch.c - taking a T-series wheel out of boot mode.
 *
 * Moved here from t150boot so the daemon can do it too. The wheel comes back
 * at the boot product id after every replug and after every wake, and the
 * daemon opens only the firmware one, so without this a wheel unplugged mid
 * race never returns no matter how correctly the daemon notices it left.
 *
 * Neither request opens the device. A device request on endpoint 0 needs no
 * interface, the transfers were measured working this way as an ordinary user
 * with the device unopened (RESEARCH.md A6), and claiming the interface
 * macOS's HID driver already owns would be refused.
 *
 * macOS only.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/USBSpec.h>

#include <string.h>

#include "t150/t150.h"

#include "bootswitch.h"

io_service_t
t150_usb_find(long vid, long pid)
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

IOUSBDeviceInterface500 **
t150_usb_open(io_service_t svc)
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

IOReturn
t150_usb_model(IOUSBDeviceInterface500 **dev, uint8_t *buf, size_t buflen,
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

IOReturn
t150_usb_switch(IOUSBDeviceInterface500 **dev, uint16_t value)
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

int
t150_usb_left_the_bus(IOReturn r)
{
	return r == kIOReturnNotResponding || r == kIOUSBPipeStalled ||
	    r == kIOReturnNoDevice || r == kIOReturnAborted;
}

const char *
t150_boot_result_str(enum t150_boot_result r)
{
	switch (r) {
	case T150_BOOT_SENT:
		return "the switch was sent";
	case T150_BOOT_ABSENT:
		return "nothing at the boot id";
	case T150_BOOT_NO_INTERFACE:
		return "a device is there but will not open";
	case T150_BOOT_NO_MODEL:
		return "the model query failed";
	case T150_BOOT_OTHER_MODEL:
		return "a T-series wheel that is not a T150";
	case T150_BOOT_REFUSED:
		return "the wheel refused the switch";
	}

	return "unknown";
}

enum t150_boot_result
t150_boot_switch(long vid, long boot_pid, uint16_t value, uint8_t *model)
{
	IOUSBDeviceInterface500 **dev;
	io_service_t svc;
	uint8_t buf[T150_RQ_MODEL_LEN];
	UInt32 done = 0;
	IOReturn r;
	enum t150_boot_result rc;

	if (model != NULL)
		*model = 0;

	if ((svc = t150_usb_find(vid, boot_pid)) == IO_OBJECT_NULL)
		return T150_BOOT_ABSENT;

	dev = t150_usb_open(svc);
	IOObjectRelease(svc);
	if (dev == NULL)
		return T150_BOOT_NO_INTERFACE;

	/*
	 * Refuse rather than send one wheel's switch value to another. The
	 * default belongs to the T150 and means something else entirely to a
	 * T300RS or a TMX, and an unattended caller is exactly the one that
	 * must not guess. Somebody with another wheel runs t150boot -V.
	 */
	r = t150_usb_model(dev, buf, sizeof(buf), &done);
	if (r != kIOReturnSuccess || done <= T150_RQ_MODEL_OFF_MODEL) {
		rc = T150_BOOT_NO_MODEL;
		goto out;
	}
	if (model != NULL)
		*model = buf[T150_RQ_MODEL_OFF_MODEL];
	if (buf[T150_RQ_MODEL_OFF_MODEL] != T150_MODEL) {
		rc = T150_BOOT_OTHER_MODEL;
		goto out;
	}

	/*
	 * The transfer's own result decides nothing. The wheel leaves the bus
	 * before it completes, so success and half a dozen failures all mean
	 * the same thing, and only the wheel reappearing at its firmware id
	 * settles it. That is the caller's next scan, not ours.
	 */
	r = t150_usb_switch(dev, value);
	rc = (r == kIOReturnSuccess || t150_usb_left_the_bus(r)) ?
	    T150_BOOT_SENT : T150_BOOT_REFUSED;

out:
	(*dev)->Release(dev);

	return rc;
}
