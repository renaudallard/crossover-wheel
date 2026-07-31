/*
 * common.h - helpers shared by the three probe tools.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef T150_PROBE_COMMON_H
#define T150_PROBE_COMMON_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOReturn.h>
#include <IOKit/hid/IOHIDManager.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * A matched set of HID devices, held open only for as long as the caller
 * needs it. The IOHIDDeviceRef values in items are owned by devices, so they
 * stay valid until probe_devlist_close and must not be released individually.
 */
struct probe_devlist {
	IOHIDManagerRef	  mgr;
	CFSetRef	  devices;
	const void	**items;
	CFIndex		  count;
};

/*
 * Match every HID node with this vendor id, and this product id too when
 * have_pid is nonzero. Returns 0 on success, in which case the caller must
 * call probe_devlist_close even if count came back 0, and -1 if the manager
 * could not be created.
 *
 * This never opens a device: it schedules the manager on the run loop and
 * pumps it until it goes quiet, so it cannot seize a wheel out from under a
 * running game and cannot raise a privacy prompt.
 */
int	probe_devlist_open(struct probe_devlist *dl, long vid, int have_pid,
	    long pid);
void	probe_devlist_close(struct probe_devlist *dl);

/* Read a numeric or string property. Both return -1 when it is absent. */
int	probe_get_long(IOHIDDeviceRef dev, CFStringRef key, long *out);
int	probe_get_string(IOHIDDeviceRef dev, CFStringRef key, char *buf,
	    size_t buflen);

/*
 * Parse an unsigned integer accepting decimal, 0x hex and 0 octal. Returns 0
 * on success and -1 if the string is not a number in its entirety or the
 * value exceeds max.
 */
int	probe_parse_uint(const char *s, unsigned long max, unsigned long *out);

/*
 * Parse a whitespace separated list of hex byte values ("40 11 ff 7f") into
 * buf. Returns the number of bytes parsed, or -1 on a malformed input or if
 * the list is longer than buflen.
 */
int	probe_parse_hex(const char *s, uint8_t *buf, size_t buflen);

/* Print len bytes of buf as an offset-prefixed hex dump on fp. */
void	probe_hexdump(FILE *fp, const uint8_t *buf, size_t len);

/*
 * Name of an IOReturn value. Returns a static string for the codes this
 * project can actually provoke, or a "0x%08x" rendering in a static buffer
 * for anything else, so it is not reentrant and the result must be printed
 * before the next call.
 */
const char *probe_ioreturn_str(IOReturn r);

#endif /* T150_PROBE_COMMON_H */
