/*
 * probe_setreport - can an unprivileged process make the wheel react?
 *
 * This is the experiment the whole project depends on. If an ordinary,
 * unentitled, non-root process can move the wheel with IOHIDDeviceSetReport
 * then a userspace force feedback bridge is possible and no kext, DEXT, SIP
 * change or AMFI change is needed. If it cannot, nothing else in this repo
 * matters.
 *
 * The default action sets the autocenter spring to full and enables it,
 * because that is the one command whose effect is unmistakable: the wheel
 * starts pulling itself back to centre the moment it lands. Use -A to switch
 * it off again.
 *
 * Three things are deliberately left as separate flags rather than being
 * guessed, because each is an open question the run is meant to settle:
 *
 *   -i   the report id. The wheel's descriptor declares output report 0x0A,
 *        but the Linux driver writes raw on the interrupt OUT pipe with no
 *        report id at all. Try 0 first, then 0x0A.
 *   -P   zero-pad to the descriptor's declared output report length. macOS
 *        may reject or clip a short write.
 *   -n   which HID node to write to, when the wheel publishes more than one.
 *
 * -x is repeatable so a whole sequence lands on one open handle. A force
 * feedback upload is four packets that correlate through slot keys, and
 * whether the wheel keeps an uploaded effect across a close is itself
 * unknown, so sending them in separate runs would confuse a negative result
 * with a lost one.
 *
 * The device is opened with kIOHIDOptionsTypeNone, never with
 * kIOHIDOptionsTypeSeizeDevice, so a running game keeps receiving input.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "t150/t150.h"

#define MAX_PAYLOAD	64
/* Four is what a force feedback upload needs: first, update, commit, play. */
/*
 * A force feedback upload is three packets plus a start, and the
 * documented sequence clears the autocenter and sets the gain first, so
 * six. Four was enough only while nobody had tried one.
 */
#define MAX_PACKETS	8

struct packet {
	uint8_t	bytes[MAX_PAYLOAD];
	size_t	len;
};

enum action {
	ACT_NONE = 0,
	ACT_AUTOCENTER,
	ACT_AUTOCENTER_OFF,
	ACT_RANGE,
	ACT_GAIN,
	ACT_RAW
};

static void
list_nodes(const struct probe_devlist *dl)
{
	CFIndex i;

	for (i = 0; i < dl->count; i++) {
		IOHIDDeviceRef dev = (IOHIDDeviceRef)dl->items[i];
		long page = -1, usage = -1, maxout = -1;

		(void)probe_get_long(dev, CFSTR(kIOHIDPrimaryUsagePageKey),
		    &page);
		(void)probe_get_long(dev, CFSTR(kIOHIDPrimaryUsageKey),
		    &usage);
		(void)probe_get_long(dev, CFSTR(kIOHIDMaxOutputReportSizeKey),
		    &maxout);

		printf("  node %ld: usage page 0x%02lx usage 0x%02lx, "
		    "MaxOutputReportSize %ld\n", (long)i, page, usage, maxout);
	}
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: probe_setreport [-v vid] [-p pid] [-n node] [-i id] [-P]\n"
	    "                       [-a force | -A | -r degrees | -g gain |\n"
	    "                        -x \"hex bytes\"]\n"
	    "\n"
	    "  -v vid       vendor id (default 0x%04x)\n"
	    "  -p pid       product id (default 0x%04x, T150 firmware mode)\n"
	    "  -n node      which matching HID node to write to (default 0)\n"
	    "  -i id        HID report id (default 0, meaning unnumbered)\n"
	    "               the payload never includes the report id itself\n"
	    "  -P           zero-pad the payload to MaxOutputReportSize\n"
	    "\n"
	    "  -a force     set autocenter spring to force (0..100) and enable\n"
	    "               it, the default action with force 100\n"
	    "  -A           disable autocenter\n"
	    "  -r degrees   set rotation range (%u..%u)\n"
	    "  -g gain      set gain, raw hardware units (0..%u), where full\n"
	    "               scale is 0x80 and not 0xff\n"
	    "  -x \"40 11 ..\"  send these raw bytes, repeatable up to %d times\n"
	    "               to send a whole sequence on one open handle\n",
	    T150_VID, T150_PID_FIRMWARE, T150_RANGE_MIN, T150_RANGE_MAX,
	    T150_GAIN_MAX, MAX_PACKETS);
	exit(2);
}

int
main(int argc, char *argv[])
{
	struct probe_devlist dl;
	struct packet pkt[MAX_PACKETS];
	IOHIDDeviceRef dev;
	IOReturn r;
	unsigned long parsed;
	long vid = T150_VID, pid = T150_PID_FIRMWARE, maxout = -1;
	unsigned long arg = 100, node = 0, report_id = 0;
	enum action act = ACT_NONE;
	size_t npkt = 0, i, padto = 0;
	int ch, pad = 0, rc = 0, raw_len = 0;

	/* Cleared before parsing, because -x fills packets as it goes. */
	memset(pkt, 0, sizeof(pkt));

	while ((ch = getopt(argc, argv, "v:p:n:i:Pa:Ar:g:x:")) != -1) {
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
		case 'n':
			if (probe_parse_uint(optarg, 63, &node) != 0)
				usage();
			break;
		case 'i':
			if (probe_parse_uint(optarg, 0xff, &report_id) != 0)
				usage();
			break;
		case 'P':
			pad = 1;
			break;
		case 'a':
			if (act != ACT_NONE ||
			    probe_parse_uint(optarg, 100, &arg) != 0)
				usage();
			act = ACT_AUTOCENTER;
			break;
		case 'A':
			if (act != ACT_NONE)
				usage();
			act = ACT_AUTOCENTER_OFF;
			break;
		case 'r':
			if (act != ACT_NONE ||
			    probe_parse_uint(optarg, T150_RANGE_MAX, &arg) != 0)
				usage();
			act = ACT_RANGE;
			break;
		case 'g':
			/* Raw hardware units, where full scale is 0x80. */
			if (act != ACT_NONE ||
			    probe_parse_uint(optarg, T150_GAIN_MAX, &arg) != 0)
				usage();
			act = ACT_GAIN;
			break;
		case 'x':
			if (act != ACT_NONE && act != ACT_RAW)
				usage();
			if (npkt >= MAX_PACKETS)
				errx(2, "at most %d -x packets", MAX_PACKETS);
			if ((raw_len = probe_parse_hex(optarg,
			    pkt[npkt].bytes, sizeof(pkt[npkt].bytes))) <= 0)
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
		act = ACT_AUTOCENTER;
		arg = 100;
	}

	switch (act) {
	case ACT_AUTOCENTER:
		pkt[0].bytes[0] = T150_OP_SETTINGS;
		pkt[0].bytes[1] = T150_OP_AUTOCENTER_FORCE;
		pkt[0].bytes[2] = (uint8_t)arg;
		pkt[0].bytes[3] = 0;
		pkt[0].len = 4;
		pkt[1].bytes[0] = T150_OP_SETTINGS;
		pkt[1].bytes[1] = T150_OP_AUTOCENTER_ENABLE;
		pkt[1].bytes[2] = 1;
		pkt[1].bytes[3] = 0;
		pkt[1].len = 4;
		npkt = 2;
		break;
	case ACT_AUTOCENTER_OFF:
		pkt[0].bytes[0] = T150_OP_SETTINGS;
		pkt[0].bytes[1] = T150_OP_AUTOCENTER_ENABLE;
		pkt[0].bytes[2] = 0;
		pkt[0].bytes[3] = 0;
		pkt[0].len = 4;
		npkt = 1;
		break;
	case ACT_RANGE: {
		uint16_t scaled = t150_range_arg((unsigned int)arg);

		pkt[0].bytes[0] = T150_OP_SETTINGS;
		pkt[0].bytes[1] = T150_OP_RANGE;
		pkt[0].bytes[2] = (uint8_t)(scaled & 0xff);
		pkt[0].bytes[3] = (uint8_t)(scaled >> 8);
		pkt[0].len = 4;
		npkt = 1;
		break;
	}
	case ACT_GAIN:
		pkt[0].bytes[0] = T150_OP_GAIN;
		pkt[0].bytes[1] = (uint8_t)arg;
		pkt[0].len = 2;
		npkt = 1;
		break;
	case ACT_RAW:
		/* Already built by -x while parsing. */
		break;
	case ACT_NONE:
		usage();
	}

	if (probe_devlist_open(&dl, vid, 1, pid) != 0) {
		probe_devlist_close(&dl);
		errx(1, "cannot enumerate HID devices");
	}
	if (dl.count == 0) {
		probe_devlist_close(&dl);
		errx(1, "no HID node matches %04lx:%04lx, run probe_hid first",
		    vid, pid);
	}
	if ((CFIndex)node >= dl.count) {
		printf("node %lu does not exist, %ld node(s) available:\n",
		    node, (long)dl.count);
		list_nodes(&dl);
		probe_devlist_close(&dl);
		return 1;
	}

	printf("%ld matching node(s):\n", (long)dl.count);
	list_nodes(&dl);

	dev = (IOHIDDeviceRef)dl.items[node];
	(void)probe_get_long(dev, CFSTR(kIOHIDMaxOutputReportSizeKey),
	    &maxout);

	if (pad) {
		if (maxout <= 0) {
			warnx("node %lu declares no MaxOutputReportSize, "
			    "padding to %u instead", node, T150_OUT_REPORT_LEN);
			padto = T150_OUT_REPORT_LEN;
		} else if ((size_t)maxout > sizeof(pkt[0].bytes)) {
			warnx("MaxOutputReportSize %ld exceeds this tool's "
			    "buffer, padding to %zu", maxout,
			    sizeof(pkt[0].bytes));
			padto = sizeof(pkt[0].bytes);
		} else {
			padto = (size_t)maxout;
		}
	}

	printf("\nwriting to node %lu with report id 0x%02lx%s\n", node,
	    report_id, pad ? ", zero-padded" : "");

	r = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
	printf("IOHIDDeviceOpen                %s\n", probe_ioreturn_str(r));
	if (r != kIOReturnSuccess) {
		if (r == kIOReturnExclusiveAccess)
			warnx("another process has seized the wheel, "
			    "close it and retry");
		probe_devlist_close(&dl);
		return 1;
	}

	for (i = 0; i < npkt; i++) {
		size_t len = pkt[i].len;

		if (padto > len) {
			/* bytes beyond len are already zero from memset */
			len = padto;
		}

		printf("  send %2zu byte(s):", len);
		probe_hexdump(stdout, pkt[i].bytes, len);

		r = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput,
		    (CFIndex)report_id, pkt[i].bytes, (CFIndex)len);
		printf("  IOHIDDeviceSetReport         %s\n",
		    probe_ioreturn_str(r));
		if (r != kIOReturnSuccess) {
			rc = 1;
			break;
		}

		/* the wheel drops back to back settings packets */
		if (i + 1 < npkt) {
			struct timespec gap = { 0, 20 * 1000 * 1000 };

			(void)nanosleep(&gap, NULL);
		}
	}

	r = IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
	printf("IOHIDDeviceClose               %s\n", probe_ioreturn_str(r));

	if (rc == 0) {
		printf("\nEvery write returned success. That is necessary but "
		    "not sufficient:\nmacOS can accept a report the firmware "
		    "then ignores. What decides this run\nis whether the wheel "
		    "physically reacted.\n");
		if (act == ACT_AUTOCENTER)
			printf("Turn the spring back off with:  "
			    "probe_setreport -A\n");
	} else {
		printf("\nThe write failed. Before concluding the HID path is "
		    "closed, retry with\na different report id (-i 0x0a), "
		    "with padding (-P), and against the other\nnodes listed "
		    "above (-n 1).\n");
	}

	probe_devlist_close(&dl);
	return rc;
}
