/*
 * proto.h - wire protocol between the in-bottle proxy DLL and the daemon.
 *
 * The DLL runs inside CrossOver, the daemon runs on macOS, so the transport
 * is TCP on 127.0.0.1. The daemon binds an ephemeral port and writes the
 * port plus a per-run token to
 *
 *   ~/Library/Application Support/t150ffb/endpoint
 *
 * which the DLL reads through Wine's Z: mapping. The token is there so that
 * any other local process cannot drive the motors just by guessing a port;
 * it is not a security boundary against a process running as the same user,
 * and the daemon's watchdog is what actually bounds the damage.
 *
 * Framing is one fixed header followed by a payload. Every field is
 * little-endian and every struct in this header is an in-memory form only:
 * structs are never written to a socket directly, because their padding is
 * implementation defined. t150_proto_pack_* and t150_proto_unpack_* own the
 * byte layout.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef T150_PROTO_H
#define T150_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "t150/effect.h"

#define T150_PROTO_MAGIC	0x54313530u	/* "T150" */
#define T150_PROTO_VERSION	1u

/* Header is magic:4 version:1 op:1 length:2, all little-endian. */
#define T150_PROTO_HDR_LEN	8u
#define T150_PROTO_MAX_PAYLOAD	256u

#define T150_TOKEN_LEN		32u

/*
 * Operations, with the exact payload each one carries. The version travels
 * in the header, so HELLO does not repeat it.
 *
 *   HELLO           32   the token, as ASCII hex
 *   BYE              0
 *   EFFECT_UPLOAD   59   see the effect layout below
 *   EFFECT_START     2   slot, iterations
 *   EFFECT_STOP      1   slot
 *   EFFECT_DESTROY   1   slot
 *   SET_GAIN         4   uint32
 *   SET_AUTOCENTER   4   uint32
 *   SET_RANGE        4   uint32 degrees
 *   RESET            0
 *   KEEPALIVE        0
 *   OK               0
 *   ERROR            2   uint16 t150_proto_err
 *   STATE            4   present, firmware mode, uint16 slot bitmap
 */
enum t150_proto_op {
	/* client to daemon */
	T150_OP_HELLO = 1,	/* token */
	T150_OP_BYE,
	T150_OP_EFFECT_UPLOAD,	/* struct t150_effect */
	T150_OP_EFFECT_START,	/* slot, iterations */
	T150_OP_EFFECT_STOP,	/* slot */
	T150_OP_EFFECT_DESTROY,	/* slot */
	T150_OP_SET_GAIN,	/* device gain, 0 .. 10000 */
	T150_OP_SET_AUTOCENTER,	/* 0 .. 10000, 0 disables */
	T150_OP_SET_RANGE,	/* degrees */
	T150_OP_RESET,		/* stop everything, release every slot */
	T150_OP_KEEPALIVE,	/* feeds the watchdog, see below */
	T150_OP_STOP_ALL,	/* stop everything, keep every slot loaded */

	/* daemon to client */
	T150_OP_OK = 128,
	T150_OP_ERROR,		/* uint16 t150_proto_err */
	T150_OP_STATE		/* wheel present, firmware mode, slot map */
};

/*
 * An effect on the wire is fixed length, because a variable one would buy
 * twenty bytes and cost a parser that has to be right about six shapes.
 *
 *    0  kind                    u8
 *    1  slot                    u8
 *    2  duration                u32
 *    6  start_delay             u32
 *   10  gain                    u32
 *   14  direction               u32
 *   18  envelope.attack_time    u32
 *   22  envelope.attack_level   i32
 *   26  envelope.fade_time      u32
 *   30  envelope.fade_level     i32
 *   34  envelope.present        u8
 *   35  six i32 parameters, read according to kind
 *
 * The six parameter slots carry, in order:
 *
 *   constant   magnitude
 *   ramp       start, end
 *   periodic   magnitude, offset, phase, period
 *   condition  center, pos_coeff, neg_coeff, pos_saturation,
 *              neg_saturation, deadband
 */
#define T150_PROTO_EFFECT_LEN	59u

enum t150_proto_err {
	T150_ERR_NONE = 0,
	T150_ERR_BAD_FRAME,
	T150_ERR_BAD_VERSION,
	T150_ERR_BAD_TOKEN,
	T150_ERR_NO_DEVICE,	/* wheel absent, or still at the boot PID */
	T150_ERR_BAD_SLOT,
	T150_ERR_UNSUPPORTED,	/* effect kind the wheel cannot render */
	T150_ERR_DEVICE_IO,	/* IOHIDDeviceSetReport failed */
	T150_ERR_DEVICE_SEIZED	/* another process holds the wheel */
};

/*
 * Watchdog.
 *
 * Nothing in this stack learns that a game has exited. Wine's hidclass
 * consumes IRP_MJ_CLOSE at the PDO and never tells anything below it, and
 * DirectInput only sends its reset on a graceful Unacquire, so a crashed or
 * force-quit game leaves the last commanded force latched on a wheel that
 * pulls hard enough to hurt. The daemon therefore treats silence as a fault:
 * if no frame arrives for this long it stops every effect and releases the
 * autocenter, whether or not the socket is still open, which leaves the
 * wheel limp rather than latched or fighting.
 */
#define T150_WATCHDOG_MS	500u

struct t150_proto_hdr {
	uint32_t	magic;
	uint8_t		version;
	uint8_t		op;
	uint16_t	length;		/* payload bytes, excludes the header */
};

/* The codec. src/lib/proto.c owns every byte, tests/proto_check.c checks it. */
size_t	t150_proto_pack_hdr(uint8_t *buf, size_t buflen,
	    const struct t150_proto_hdr *hdr);
int	t150_proto_unpack_hdr(const uint8_t *buf, size_t buflen,
	    struct t150_proto_hdr *hdr);
size_t	t150_proto_pack_effect(uint8_t *buf, size_t buflen,
	    const struct t150_effect *ef);
int	t150_proto_unpack_effect(const uint8_t *buf, size_t buflen,
	    struct t150_effect *ef);

#endif /* T150_PROTO_H */
