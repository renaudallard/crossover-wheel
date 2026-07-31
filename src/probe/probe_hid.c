/*
 * probe_hid - inventory the HID nodes macOS publishes for the wheel.
 *
 * This answers the questions the rest of the project is built on:
 *
 *   - Is the wheel sitting at the boot PID (0xB65D) or has it already
 *     reached firmware mode (0xB677)? If it is already in firmware mode
 *     then the vendor control transfer, and the root privilege it may
 *     need, is not required at all.
 *   - How many IOHIDDevice nodes does one physical wheel publish, and does
 *     the node carrying the joystick usage also carry the vendor output
 *     report? If those live on separate nodes, the daemon has to open both.
 *   - What is kIOHIDMaxOutputReportSize? If it is smaller than the packets
 *     we need to send, IOHIDDeviceSetReport may clip them.
 *   - Does the node carry a ProtectedAccess property, which would mean an
 *     unprivileged process cannot open it at all?
 *
 * It only reads properties and never opens a device, so it cannot disturb a
 * running game and cannot trigger a privacy prompt.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <err.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "t150/t150.h"

static const char *
pid_meaning(long vid, long pid)
{
	if (vid != (long)T150_VID)
		return "";
	if (pid == (long)T150_PID_BOOT)
		return "  <- boot mode, needs the vendor mode switch";
	if (pid == (long)T150_PID_FIRMWARE)
		return "  <- T150 firmware mode";
	return "";
}

static void
print_long(IOHIDDeviceRef dev, const char *label, CFStringRef key)
{
	long v;

	if (probe_get_long(dev, key, &v) == 0)
		printf("  %-24s %ld\n", label, v);
	else
		printf("  %-24s (absent)\n", label);
}

static void
print_flag(IOHIDDeviceRef dev, const char *label, CFStringRef key)
{
	CFTypeRef v;

	v = IOHIDDeviceGetProperty(dev, key);
	printf("  %-24s %s\n", label, v == NULL ? "(absent)" : "PRESENT");
}

static void
print_usage_pairs(IOHIDDeviceRef dev)
{
	CFArrayRef pairs;
	CFIndex i, n;

	pairs = (CFArrayRef)IOHIDDeviceGetProperty(dev,
	    CFSTR(kIOHIDDeviceUsagePairsKey));
	if (pairs == NULL || CFGetTypeID(pairs) != CFArrayGetTypeID()) {
		printf("  %-24s (absent)\n", "UsagePairs");
		return;
	}

	n = CFArrayGetCount(pairs);
	printf("  %-24s %ld pair(s)\n", "UsagePairs", (long)n);
	for (i = 0; i < n; i++) {
		CFDictionaryRef d;
		CFNumberRef pageref, usageref;
		long page = -1, usage = -1;

		d = (CFDictionaryRef)CFArrayGetValueAtIndex(pairs, i);
		if (d == NULL || CFGetTypeID(d) != CFDictionaryGetTypeID())
			continue;

		pageref = (CFNumberRef)CFDictionaryGetValue(d,
		    CFSTR(kIOHIDDeviceUsagePageKey));
		usageref = (CFNumberRef)CFDictionaryGetValue(d,
		    CFSTR(kIOHIDDeviceUsageKey));
		if (pageref != NULL)
			(void)CFNumberGetValue(pageref, kCFNumberLongType,
			    &page);
		if (usageref != NULL)
			(void)CFNumberGetValue(usageref, kCFNumberLongType,
			    &usage);

		printf("      page 0x%02lx usage 0x%02lx%s\n", page, usage,
		    (page == 0x01 && (usage == 0x04 || usage == 0x05)) ?
		    "   (joystick or gamepad, the node CrossOver enumerates)" :
		    "");
	}
}

static void
dump_descriptor(IOHIDDeviceRef dev, const char *outdir, long vid, long pid,
    int index)
{
	CFDataRef data;
	const UInt8 *bytes;
	CFIndex len;
	char path[PATH_MAX];
	FILE *fp;

	data = (CFDataRef)IOHIDDeviceGetProperty(dev,
	    CFSTR(kIOHIDReportDescriptorKey));
	if (data == NULL || CFGetTypeID(data) != CFDataGetTypeID()) {
		printf("  %-24s (absent)\n", "ReportDescriptor");
		return;
	}

	len = CFDataGetLength(data);
	bytes = CFDataGetBytePtr(data);
	if (bytes == NULL || len <= 0) {
		printf("  %-24s (unreadable)\n", "ReportDescriptor");
		return;
	}

	printf("  %-24s %ld bytes\n", "ReportDescriptor", (long)len);
	probe_hexdump(stdout, bytes, (size_t)len);

	if (outdir == NULL)
		return;

	if (snprintf(path, sizeof(path), "%s/desc-%04lx-%04lx-%d.bin", outdir,
	    vid, pid, index) >= (int)sizeof(path)) {
		warnx("output path too long, descriptor not saved");
		return;
	}
	if ((fp = fopen(path, "wb")) == NULL) {
		warn("%s", path);
		return;
	}
	if (fwrite(bytes, 1, (size_t)len, fp) != (size_t)len)
		warnx("short write to %s", path);
	if (fclose(fp) != 0)
		warn("%s", path);
	else
		printf("  wrote %s\n", path);
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: probe_hid [-v vid] [-p pid] [-o dir]\n"
	    "  -v vid   match this USB vendor id (default 0x%04x)\n"
	    "  -p pid   also match this product id\n"
	    "  -o dir   write each report descriptor to dir as a .bin file\n",
	    T150_VID);
	exit(2);
}

int
main(int argc, char *argv[])
{
	struct probe_devlist dl;
	const char *outdir = NULL;
	unsigned long parsed;
	long vid = T150_VID, pid = 0;
	int have_pid = 0, ch;
	CFIndex i;

	while ((ch = getopt(argc, argv, "v:p:o:")) != -1) {
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
			have_pid = 1;
			break;
		case 'o':
			outdir = optarg;
			break;
		default:
			usage();
		}
	}
	if (optind != argc)
		usage();

	if (probe_devlist_open(&dl, vid, have_pid, pid) != 0) {
		probe_devlist_close(&dl);
		errx(1, "cannot enumerate HID devices");
	}

	printf("%ld HID node(s) match vendor 0x%04lx\n\n", (long)dl.count, vid);
	if (dl.count == 0) {
		printf("If the wheel is plugged in it may be at the boot PID "
		    "with no HID node at all.\nCheck with:\n"
		    "    system_profiler SPUSBDataType | "
		    "grep -A5 -i thrustmaster\n");
		probe_devlist_close(&dl);
		return 0;
	}

	for (i = 0; i < dl.count; i++) {
		IOHIDDeviceRef dev = (IOHIDDeviceRef)dl.items[i];
		char product[256], manufacturer[256], serial[256];
		long dvid = 0, dpid = 0;

		(void)probe_get_long(dev, CFSTR(kIOHIDVendorIDKey), &dvid);
		(void)probe_get_long(dev, CFSTR(kIOHIDProductIDKey), &dpid);
		(void)probe_get_string(dev, CFSTR(kIOHIDProductKey), product,
		    sizeof(product));
		(void)probe_get_string(dev, CFSTR(kIOHIDManufacturerKey),
		    manufacturer, sizeof(manufacturer));
		(void)probe_get_string(dev, CFSTR(kIOHIDSerialNumberKey),
		    serial, sizeof(serial));

		printf("node %ld: %04lx:%04lx %s%s\n", (long)i, dvid, dpid,
		    product[0] != '\0' ? product : "(no product string)",
		    pid_meaning(dvid, dpid));
		if (manufacturer[0] != '\0')
			printf("  %-24s %s\n", "Manufacturer", manufacturer);
		if (serial[0] != '\0')
			printf("  %-24s %s\n", "SerialNumber", serial);

		print_long(dev, "PrimaryUsagePage",
		    CFSTR(kIOHIDPrimaryUsagePageKey));
		print_long(dev, "PrimaryUsage", CFSTR(kIOHIDPrimaryUsageKey));
		print_usage_pairs(dev);
		print_long(dev, "MaxInputReportSize",
		    CFSTR(kIOHIDMaxInputReportSizeKey));
		print_long(dev, "MaxOutputReportSize",
		    CFSTR(kIOHIDMaxOutputReportSizeKey));
		print_long(dev, "MaxFeatureReportSize",
		    CFSTR(kIOHIDMaxFeatureReportSizeKey));
		/*
		 * Raw key name rather than kIOHIDProtectedAccessKey: the
		 * constant is not in the public SDK, only the property is.
		 */
		print_flag(dev, "ProtectedAccess", CFSTR("ProtectedAccess"));
		dump_descriptor(dev, outdir, dvid, dpid, (int)i);
		printf("\n");
	}

	probe_devlist_close(&dl);
	return 0;
}
