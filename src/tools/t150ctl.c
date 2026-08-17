/*
 * t150ctl - set the wheel's rotation range, gain and autocenter.
 *
 * Everything here goes through IOHIDDeviceSetReport on a device opened
 * without seizing it, so it needs no root, no entitlement and no device
 * capture, and a game running in a bottle keeps reading the wheel while this
 * runs. That is not an assumption: RESEARCH.md A19 is the measurement.
 *
 * The wheel keeps these settings itself, so this is fire and forget. There is
 * no daemon involved and nothing to leave running.
 *
 * It deliberately does not open the wheel's input. Settings are honoured with
 * it closed, only force feedback needs it, and opening one here would change
 * how the wheel behaves after the tool exits.
 *
 * macOS only.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "t150/encode.h"
#include "t150/t150.h"

#define MAX_PKT		4
#define PKT_LEN		8

struct job {
	uint8_t	bytes[MAX_PKT][PKT_LEN];
	size_t	len[MAX_PKT];
	size_t	n;
};

static void
usage(void)
{
	fprintf(stderr,
	    "usage: t150ctl [-v vid] [-p pid] command\n"
	    "\n"
	    "  -v vid       vendor id (default 0x%04x)\n"
	    "  -p pid       product id (default 0x%04x)\n"
	    "\n"
	    "commands:\n"
	    "  range DEGREES      lock to lock, %u to %u\n"
	    "  gain N             overall force, 0 to %u\n"
	    "  autocenter N       centring spring, 0 to %u, 0 releases it\n"
	    "  status             what the wheel says it is\n"
	    "\n"
	    "  Needs no privilege and does not take the wheel from a running\n"
	    "  game. The wheel keeps these settings until it is unplugged.\n",
	    T150_VID, T150_PID_FIRMWARE, T150_RANGE_MIN, T150_RANGE_MAX,
	    T150_DI_MAX, T150_DI_MAX);
	exit(2);
}

/*
 * Append whatever the encoder produced, refusing a packet it would not make.
 * The encoder fills the caller's own buffer and this copies it in, so the
 * bounds check happens before anything is written to the job: handing the
 * encoder j->bytes[j->n] directly would have it write the slot first and be
 * told the job was full afterwards.
 */
static int
add(struct job *j, const uint8_t *src, size_t n)
{
	if (n == 0 || n > PKT_LEN || j->n >= MAX_PKT)
		return -1;
	memcpy(j->bytes[j->n], src, n);
	j->len[j->n] = n;
	j->n++;

	return 0;
}

static int
build(struct job *j, int argc, char *argv[])
{
	uint8_t tmp[PKT_LEN];
	unsigned long v;

	if (argc < 1)
		return -1;

	if (strcmp(argv[0], "status") == 0)
		return argc == 1 ? 0 : -1;

	if (argc != 2)
		return -1;

	if (strcmp(argv[0], "range") == 0) {
		if (probe_parse_uint(argv[1], T150_RANGE_MAX, &v) != 0 ||
		    v < T150_RANGE_MIN)
			return -1;
		return add(j, tmp, t150_enc_range(tmp, sizeof(tmp),
		    (unsigned int)v));
	}

	if (strcmp(argv[0], "gain") == 0) {
		if (probe_parse_uint(argv[1], T150_DI_MAX, &v) != 0)
			return -1;
		return add(j, tmp, t150_enc_gain(tmp, sizeof(tmp),
		    (uint32_t)v));
	}

	if (strcmp(argv[0], "autocenter") == 0) {
		if (probe_parse_uint(argv[1], T150_DI_MAX, &v) != 0)
			return -1;
		/*
		 * The force, then the flag. Zero force is what releases the
		 * wheel: the flag only decides whether the autocenter
		 * survives an application opening the input, and the effect
		 * is active whenever none has. Sending only the flag is the
		 * mistake that cost this project six sessions.
		 */
		if (add(j, tmp, t150_enc_autocenter_force(tmp, sizeof(tmp),
		    (uint32_t)v)) != 0)
			return -1;
		return add(j, tmp, t150_enc_autocenter_enable(tmp,
		    sizeof(tmp), v != 0));
	}

	return -1;
}

/*
 * Which node to write to. A CFSet has no order, so taking the first element
 * of a multi-node match is a coin toss, and the wheel publishes more than one
 * node in some modes. Prefer the Generic Desktop one, which is the joystick
 * or gamepad collection that carries the wheel, and refuse to guess when
 * there is more than one candidate.
 */
static IOHIDDeviceRef
pick_node(const struct probe_devlist *dl, int *joysticks)
{
	IOHIDDeviceRef found = NULL;
	CFIndex i;
	int seen = 0;

	*joysticks = 1;
	if (dl->count == 1)
		return (IOHIDDeviceRef)dl->items[0];

	for (i = 0; i < dl->count; i++) {
		IOHIDDeviceRef d = (IOHIDDeviceRef)dl->items[i];
		long page = 0;

		if (probe_get_long(d, CFSTR(kIOHIDPrimaryUsagePageKey),
		    &page) != 0 || page != 0x01)
			continue;
		found = d;
		seen++;
	}

	*joysticks = seen;

	return seen == 1 ? found : NULL;
}

static void
show_status(IOHIDDeviceRef dev, long vid, long pid)
{
	char s[128];
	long n;

	printf("wheel     %04lx:%04lx", vid, pid);
	if (pid == (long)T150_PID_FIRMWARE)
		printf("  firmware mode");
	else if (pid == (long)T150_PID_BOOT)
		printf("  boot mode, run t150boot");
	printf("\n");

	if (probe_get_string(dev, CFSTR(kIOHIDProductKey), s, sizeof(s)) == 0)
		printf("product   %s\n", s);
	if (probe_get_long(dev, CFSTR(kIOHIDPrimaryUsagePageKey), &n) == 0)
		printf("usage     page %ld", n);
	if (probe_get_long(dev, CFSTR(kIOHIDPrimaryUsageKey), &n) == 0)
		printf(" usage %ld", n);
	printf("\n");
	if (probe_get_long(dev, CFSTR(kIOHIDMaxOutputReportSizeKey), &n) == 0)
		printf("output    %ld bytes\n", n);

	/*
	 * The wheel reports no setting back, so there is nothing to read: it
	 * has no input report that carries its range or gain. What it is set
	 * to is only what it was last told.
	 */
	printf("settings  not readable, the wheel reports none back\n");
}

int
main(int argc, char *argv[])
{
	struct probe_devlist dl;
	struct job j;
	IOHIDDeviceRef dev;
	long vid = T150_VID, pid = T150_PID_FIRMWARE;
	unsigned long parsed;
	size_t i;
	int ch, rc = 0, joysticks = 0;

	memset(&j, 0, sizeof(j));

	while ((ch = getopt(argc, argv, "v:p:")) != -1) {
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
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (build(&j, argc, argv) != 0)
		usage();

	if (probe_devlist_open(&dl, vid, 1, pid) != 0) {
		probe_devlist_close(&dl);
		errx(1, "cannot enumerate HID devices");
	}
	if (dl.count < 1) {
		probe_devlist_close(&dl);
		errx(1, "no wheel at %04lx:%04lx, is it plugged in and "
		    "switched to firmware mode", vid, pid);
	}
	if ((dev = pick_node(&dl, &joysticks)) == NULL) {
		long n = (long)dl.count;

		probe_devlist_close(&dl);
		/*
		 * pick_node refuses for two different reasons and this said
		 * the first for both, so somebody with two joystick nodes was
		 * sent looking for a missing one. The man page documents the
		 * rule the other way round: exactly one has to be a Generic
		 * Desktop collection.
		 */
		errx(1, "%ld nodes match %04lx:%04lx and %s, so there is no "
		    "telling which one drives the wheel", n, vid, pid,
		    joysticks == 0 ? "none is a joystick" :
		    "more than one is a joystick");
	}

	if (j.n == 0) {
		show_status(dev, vid, pid);
		probe_devlist_close(&dl);
		return 0;
	}

	if (IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
		probe_devlist_close(&dl);
		errx(1, "cannot open the wheel, something may have seized it");
	}

	for (i = 0; i < j.n; i++) {
		IOReturn r = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput,
		    0, j.bytes[i], (CFIndex)j.len[i]);

		if (r != kIOReturnSuccess) {
			warnx("the wheel refused a packet: %s",
			    probe_ioreturn_str(r));
			rc = 1;
			break;
		}
	}

	(void)IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
	probe_devlist_close(&dl);

	return rc;
}
