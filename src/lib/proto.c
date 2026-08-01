/*
 * proto.c - the wire format between the proxy DLL and the daemon.
 *
 * Structs never reach a socket: their padding is implementation defined and
 * the two ends are built by different compilers for different platforms. The
 * functions here own every byte, and tests/proto_check.c owns them back.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>

#include "t150/proto.h"

#define PARAM_SLOTS	6
#define PARAM_OFF	35

static void
put_u16(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)(v & 0xff);
	b[1] = (uint8_t)(v >> 8);
}

static void
put_u32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v & 0xff);
	b[1] = (uint8_t)((v >> 8) & 0xff);
	b[2] = (uint8_t)((v >> 16) & 0xff);
	b[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint16_t
get_u16(const uint8_t *b)
{
	return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t
get_u32(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/*
 * Signed values travel as two's complement. Going out is well defined by
 * conversion, but coming back is not until C23, so do it by hand rather than
 * trusting a cast on a value above INT32_MAX.
 */
static void
put_i32(uint8_t *b, int32_t v)
{
	put_u32(b, (uint32_t)v);
}

static int32_t
get_i32(const uint8_t *b)
{
	uint32_t v = get_u32(b);

	if (v <= (uint32_t)INT32_MAX)
		return (int32_t)v;

	return (int32_t)(v - (uint32_t)INT32_MAX - 1) + INT32_MIN;
}

size_t
t150_proto_pack_hdr(uint8_t *buf, size_t buflen, const struct t150_proto_hdr *hdr)
{
	if (buflen < T150_PROTO_HDR_LEN)
		return 0;
	if (hdr->length > T150_PROTO_MAX_PAYLOAD)
		return 0;

	put_u32(buf, hdr->magic);
	buf[4] = hdr->version;
	buf[5] = hdr->op;
	put_u16(buf + 6, hdr->length);

	return T150_PROTO_HDR_LEN;
}

/*
 * Decode a header, rejecting anything structurally wrong: a short read, a
 * bad magic, or a length this protocol could never produce. The version is
 * decoded but not judged, because the caller answers a wrong one with
 * T150_ERR_BAD_VERSION rather than by hanging up.
 */
int
t150_proto_unpack_hdr(const uint8_t *buf, size_t buflen, struct t150_proto_hdr *hdr)
{
	if (buflen < T150_PROTO_HDR_LEN)
		return -1;

	hdr->magic = get_u32(buf);
	hdr->version = buf[4];
	hdr->op = buf[5];
	hdr->length = get_u16(buf + 6);

	if (hdr->magic != T150_PROTO_MAGIC)
		return -1;
	if (hdr->length > T150_PROTO_MAX_PAYLOAD)
		return -1;

	return 0;
}

size_t
t150_proto_pack_effect(uint8_t *buf, size_t buflen, const struct t150_effect *ef)
{
	int32_t p[PARAM_SLOTS];
	size_t i;

	if (buflen < T150_PROTO_EFFECT_LEN)
		return 0;

	memset(p, 0, sizeof(p));

	switch (ef->kind) {
	case T150_EFFECT_CONSTANT:
		p[0] = ef->u.constant.magnitude;
		break;
	case T150_EFFECT_RAMP:
		p[0] = ef->u.ramp.start;
		p[1] = ef->u.ramp.end;
		break;
	case T150_EFFECT_SQUARE:
	case T150_EFFECT_SINE:
	case T150_EFFECT_TRIANGLE:
	case T150_EFFECT_SAWTOOTH_UP:
	case T150_EFFECT_SAWTOOTH_DOWN:
		p[0] = ef->u.periodic.magnitude;
		p[1] = ef->u.periodic.offset;
		p[2] = (int32_t)ef->u.periodic.phase;
		p[3] = (int32_t)ef->u.periodic.period;
		break;
	case T150_EFFECT_SPRING:
	case T150_EFFECT_DAMPER:
	case T150_EFFECT_FRICTION:
	case T150_EFFECT_INERTIA:
		p[0] = ef->u.condition.center;
		p[1] = ef->u.condition.pos_coeff;
		p[2] = ef->u.condition.neg_coeff;
		p[3] = ef->u.condition.pos_saturation;
		p[4] = ef->u.condition.neg_saturation;
		p[5] = ef->u.condition.deadband;
		break;
	default:
		break;
	}

	buf[0] = ef->kind;
	buf[1] = ef->slot;
	put_u32(buf + 2, ef->duration);
	put_u32(buf + 6, ef->start_delay);
	put_u32(buf + 10, ef->gain);
	put_u32(buf + 14, ef->direction);
	put_u32(buf + 18, ef->envelope.attack_time);
	put_i32(buf + 22, ef->envelope.attack_level);
	put_u32(buf + 26, ef->envelope.fade_time);
	put_i32(buf + 30, ef->envelope.fade_level);
	buf[34] = ef->envelope.present ? 1 : 0;

	for (i = 0; i < PARAM_SLOTS; i++)
		put_i32(buf + PARAM_OFF + i * 4, p[i]);

	return T150_PROTO_EFFECT_LEN;
}

int
t150_proto_unpack_effect(const uint8_t *buf, size_t buflen, struct t150_effect *ef)
{
	int32_t p[PARAM_SLOTS];
	size_t i;

	if (buflen < T150_PROTO_EFFECT_LEN)
		return -1;

	memset(ef, 0, sizeof(*ef));

	for (i = 0; i < PARAM_SLOTS; i++)
		p[i] = get_i32(buf + PARAM_OFF + i * 4);

	ef->kind = buf[0];
	ef->slot = buf[1];
	ef->duration = get_u32(buf + 2);
	ef->start_delay = get_u32(buf + 6);
	ef->gain = get_u32(buf + 10);
	ef->direction = get_u32(buf + 14);
	ef->envelope.attack_time = get_u32(buf + 18);
	ef->envelope.attack_level = get_i32(buf + 22);
	ef->envelope.fade_time = get_u32(buf + 26);
	ef->envelope.fade_level = get_i32(buf + 30);
	ef->envelope.present = buf[34] ? 1 : 0;

	switch (ef->kind) {
	case T150_EFFECT_CONSTANT:
		ef->u.constant.magnitude = p[0];
		break;
	case T150_EFFECT_RAMP:
		ef->u.ramp.start = p[0];
		ef->u.ramp.end = p[1];
		break;
	case T150_EFFECT_SQUARE:
	case T150_EFFECT_SINE:
	case T150_EFFECT_TRIANGLE:
	case T150_EFFECT_SAWTOOTH_UP:
	case T150_EFFECT_SAWTOOTH_DOWN:
		ef->u.periodic.magnitude = p[0];
		ef->u.periodic.offset = p[1];
		ef->u.periodic.phase = (uint32_t)p[2];
		ef->u.periodic.period = (uint32_t)p[3];
		break;
	case T150_EFFECT_SPRING:
	case T150_EFFECT_DAMPER:
	case T150_EFFECT_FRICTION:
	case T150_EFFECT_INERTIA:
		ef->u.condition.center = p[0];
		ef->u.condition.pos_coeff = p[1];
		ef->u.condition.neg_coeff = p[2];
		ef->u.condition.pos_saturation = p[3];
		ef->u.condition.neg_saturation = p[4];
		ef->u.condition.deadband = p[5];
		break;
	default:
		/* An unknown kind is not a frame error: the daemon answers
		 * T150_ERR_UNSUPPORTED, which a game can survive. */
		break;
	}

	return 0;
}
