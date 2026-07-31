/*
 * common.c - helpers shared by the three probe tools.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static CFMutableDictionaryRef
make_match(long vid, int have_pid, long pid)
{
	CFMutableDictionaryRef d;
	CFNumberRef n;

	d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (d == NULL)
		return NULL;

	if ((n = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongType,
	    &vid)) == NULL) {
		CFRelease(d);
		return NULL;
	}
	CFDictionarySetValue(d, CFSTR(kIOHIDVendorIDKey), n);
	CFRelease(n);

	if (have_pid) {
		if ((n = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongType,
		    &pid)) == NULL) {
			CFRelease(d);
			return NULL;
		}
		CFDictionarySetValue(d, CFSTR(kIOHIDProductIDKey), n);
		CFRelease(n);
	}

	return d;
}

/*
 * IOHIDManagerCopyDevices only reports what the manager has already seen, and
 * the manager learns about devices from the run loop. Pump it until it goes
 * quiet rather than calling IOHIDManagerOpen, which would open every matched
 * device.
 */
static void
settle_run_loop(void)
{
	SInt32 res;

	do {
		res = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, 0);
	} while (res != kCFRunLoopRunFinished && res != kCFRunLoopRunTimedOut);
}

int
probe_devlist_open(struct probe_devlist *dl, long vid, int have_pid, long pid)
{
	CFMutableDictionaryRef match;

	memset(dl, 0, sizeof(*dl));

	if ((match = make_match(vid, have_pid, pid)) == NULL)
		return -1;

	dl->mgr = IOHIDManagerCreate(kCFAllocatorDefault,
	    kIOHIDOptionsTypeNone);
	if (dl->mgr == NULL) {
		CFRelease(match);
		return -1;
	}

	IOHIDManagerSetDeviceMatching(dl->mgr, match);
	CFRelease(match);

	IOHIDManagerScheduleWithRunLoop(dl->mgr, CFRunLoopGetCurrent(),
	    kCFRunLoopDefaultMode);
	settle_run_loop();

	if ((dl->devices = IOHIDManagerCopyDevices(dl->mgr)) == NULL)
		return 0;

	dl->count = CFSetGetCount(dl->devices);
	if (dl->count <= 0) {
		dl->count = 0;
		return 0;
	}

	if ((dl->items = calloc((size_t)dl->count, sizeof(*dl->items)))
	    == NULL) {
		dl->count = 0;
		return -1;
	}
	CFSetGetValues(dl->devices, dl->items);

	return 0;
}

void
probe_devlist_close(struct probe_devlist *dl)
{
	free(dl->items);
	dl->items = NULL;

	if (dl->devices != NULL) {
		CFRelease(dl->devices);
		dl->devices = NULL;
	}
	if (dl->mgr != NULL) {
		IOHIDManagerUnscheduleFromRunLoop(dl->mgr,
		    CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
		CFRelease(dl->mgr);
		dl->mgr = NULL;
	}
	dl->count = 0;
}

int
probe_get_long(IOHIDDeviceRef dev, CFStringRef key, long *out)
{
	CFTypeRef v;

	v = IOHIDDeviceGetProperty(dev, key);
	if (v == NULL || CFGetTypeID(v) != CFNumberGetTypeID())
		return -1;
	if (!CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, out))
		return -1;
	return 0;
}

int
probe_get_string(IOHIDDeviceRef dev, CFStringRef key, char *buf, size_t buflen)
{
	CFTypeRef v;

	if (buflen == 0)
		return -1;
	buf[0] = '\0';

	v = IOHIDDeviceGetProperty(dev, key);
	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID())
		return -1;
	if (!CFStringGetCString((CFStringRef)v, buf, (CFIndex)buflen,
	    kCFStringEncodingUTF8)) {
		buf[0] = '\0';
		return -1;
	}
	return 0;
}

int
probe_parse_uint(const char *s, unsigned long max, unsigned long *out)
{
	char *end;
	unsigned long v;

	if (s == NULL || *s == '\0')
		return -1;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno != 0 || *end != '\0' || end == s)
		return -1;
	if (v > max)
		return -1;

	*out = v;
	return 0;
}

int
probe_parse_hex(const char *s, uint8_t *buf, size_t buflen)
{
	size_t n = 0;

	if (s == NULL)
		return -1;

	while (*s != '\0') {
		unsigned int hi, lo;

		while (isspace((unsigned char)*s) || *s == ',')
			s++;
		if (*s == '\0')
			break;

		if (!isxdigit((unsigned char)s[0]) ||
		    !isxdigit((unsigned char)s[1]))
			return -1;
		if (n >= buflen)
			return -1;

		hi = (unsigned int)(isdigit((unsigned char)s[0]) ?
		    s[0] - '0' : tolower((unsigned char)s[0]) - 'a' + 10);
		lo = (unsigned int)(isdigit((unsigned char)s[1]) ?
		    s[1] - '0' : tolower((unsigned char)s[1]) - 'a' + 10);
		buf[n++] = (uint8_t)((hi << 4) | lo);
		s += 2;

		/* A third hex digit means the caller mistyped a byte. */
		if (isxdigit((unsigned char)*s))
			return -1;
	}

	if (n > (size_t)INT_MAX)
		return -1;
	return (int)n;
}

void
probe_hexdump(FILE *fp, const uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (i % 16 == 0)
			fprintf(fp, "%s%04zx:", i == 0 ? "" : "\n", i);
		fprintf(fp, " %02x", buf[i]);
	}
	if (len > 0)
		fputc('\n', fp);
}

const char *
probe_ioreturn_str(IOReturn r)
{
	static char unknown[16];

	switch (r) {
	case kIOReturnSuccess:
		return "kIOReturnSuccess";
	case kIOReturnError:
		return "kIOReturnError";
	case kIOReturnNoMemory:
		return "kIOReturnNoMemory";
	case kIOReturnNoResources:
		return "kIOReturnNoResources";
	case kIOReturnBadArgument:
		return "kIOReturnBadArgument";
	case kIOReturnExclusiveAccess:
		return "kIOReturnExclusiveAccess";
	case kIOReturnUnsupported:
		return "kIOReturnUnsupported";
	case kIOReturnNoDevice:
		return "kIOReturnNoDevice";
	case kIOReturnNotPrivileged:
		return "kIOReturnNotPrivileged";
	case kIOReturnNotPermitted:
		return "kIOReturnNotPermitted";
	case kIOReturnNotOpen:
		return "kIOReturnNotOpen";
	case kIOReturnNotResponding:
		return "kIOReturnNotResponding";
	case kIOReturnTimeout:
		return "kIOReturnTimeout";
	case kIOReturnAborted:
		return "kIOReturnAborted";
	case kIOReturnUnderrun:
		return "kIOReturnUnderrun";
	case kIOReturnOverrun:
		return "kIOReturnOverrun";
	case kIOReturnBusy:
		return "kIOReturnBusy";
	case kIOReturnOffline:
		return "kIOReturnOffline";
	default:
		(void)snprintf(unknown, sizeof(unknown), "0x%08x",
		    (unsigned int)r);
		return unknown;
	}
}
