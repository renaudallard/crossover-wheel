/*
 * t150.h - Thrustmaster T150 wire constants.
 *
 * Every value here is transcribed from a cited source. Nothing in this
 * header is inferred. See docs/PROTOCOL.md for the derivation and for the
 * points that are still unconfirmed against real hardware.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef T150_H
#define T150_H

#include <stdint.h>

/*
 * USB identity. All T-series wheels enumerate at the shared boot PID and
 * only reveal their model-specific firmware PID after the mode switch below.
 * Source: scarburato/hid-tminit, linux drivers/hid/hid-thrustmaster.c.
 */
#define T150_VID		0x044fu
#define T150_PID_BOOT		0xb65du	/* "Thrustmaster FFB Wheel" */
#define T150_PID_FIRMWARE	0xb677u

/*
 * Boot to firmware mode switch, two vendor control transfers on endpoint 0.
 *
 * 1. model query:  bmRequestType 0xC1, bRequest 73, wLength 16.
 *    Response byte 6 is the attachment, byte 7 is the model.
 * 2. mode switch:  bmRequestType 0x41, bRequest 83, wValue = switch value,
 *    no data stage. The wheel detaches and re-enumerates.
 *
 * Source: linux drivers/hid/hid-thrustmaster.c, which issues exactly these
 * two requests via usb_fill_control_urb().
 */
#define T150_RQ_MODEL_TYPE	0xc1u
#define T150_RQ_MODEL		73u
#define T150_RQ_MODEL_LEN	16u
#define T150_RQ_MODEL_OFF_ATTACH 6u
#define T150_RQ_MODEL_OFF_MODEL	7u

#define T150_RQ_SWITCH_TYPE	0x41u
#define T150_RQ_SWITCH		83u

/* The T150 row of hid-tminit's model table. */
#define T150_MODEL		0x03u
#define T150_ATTACHMENT		0x06u
#define T150_SWITCH_VALUE	0x0006u

/*
 * USB endpoints in firmware mode.
 *
 * NOTE: two sources disagree on the interrupt OUT address. t150_driver's
 * hid-t150.c discovers it at runtime rather than hardcoding it, while its
 * own traffic/old_caps/t150_test.py writes to 0x01. macoswheels recorded
 * 0x02. This only matters if the HID SetReport path fails and we fall back
 * to raw pipe access, so it is left unresolved until probe_ep0 says whether
 * raw access is even reachable.
 */
#define T150_EP_INTR_IN		0x81u
#define T150_EP_INTR_OUT_A	0x02u
#define T150_EP_INTR_OUT_B	0x01u

/*
 * HID output report declared by the wheel's own report descriptor.
 *
 * Decoded from t150_driver/traffic/old_caps/hid_report_fw35 (firmware 3.5):
 *
 *   85 0A           Report ID (0x0A)
 *   06 00 FF        Usage Page (vendor 0xFF00)
 *   09 0A           Usage (0x0A)
 *   75 08 95 0E     Report Size 8, Report Count 14
 *   26 FF 00        Logical Maximum 255
 *   46 FF 00        Physical Maximum 255
 *   91 02           Output (Data, Var, Abs)
 *
 * This is the report IOHIDDeviceSetReport() would address. It is NOT how
 * the Linux driver talks to the wheel: hid-t150 bypasses the HID layer and
 * writes raw on the interrupt OUT pipe with no report ID prefix. Whether
 * the firmware accepts the HID-framed form is exactly what probe_setreport
 * exists to answer.
 */
#define T150_OUT_REPORT_ID	0x0au
#define T150_OUT_REPORT_LEN	14u

/*
 * Settings packets. Every setting except gain shares one 4-byte form:
 *
 *   [0x40, op, arg_lo, arg_hi]      little-endian uint16 argument
 *
 * Source: scarburato/t150_driver hid-t150/settings.c.
 */
#define T150_OP_SETTINGS	0x40u
#define T150_OP_AUTOCENTER_FORCE	0x03u	/* arg 0..100, a hardware percent */
#define T150_OP_AUTOCENTER_ENABLE	0x04u	/* arg 0 = off, 1 = on */
#define T150_OP_RANGE			0x11u	/* arg = degrees * 0xFFFF / 1080 */

#define T150_OP_GAIN		0x43u	/* [0x43, gain], one byte, not two */

/* Rotation range accepted by the firmware. Values below the minimum are
 * capped by the wheel itself. */
#define T150_RANGE_MIN		270u
#define T150_RANGE_MAX		1080u

/* Scale a rotation range in degrees to the wheel's 16-bit argument. */
static inline uint16_t
t150_range_arg(unsigned int degrees)
{
	if (degrees > T150_RANGE_MAX)
		degrees = T150_RANGE_MAX;
	return (uint16_t)(((uint32_t)degrees * 0xffffu) / T150_RANGE_MAX);
}

#endif /* T150_H */
