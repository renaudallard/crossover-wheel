/*
 * wirequeue.c - packets waiting for a wheel that is slower than the daemon.
 *
 * The daemon can build force feedback packets faster than the wheel will
 * take them. Measured: the emitter flushes up to four dirty slots every four
 * milliseconds, and a constant costs two packets because Thrustmaster's own
 * driver pairs every update with a control, so a game holding three slots
 * asks for about 1250 packets a second. The fastest the vendor's driver ever
 * puts two packets on that wire, in tmp/oldffb/directX_constforce.pcapng, is
 * 1.344 ms apart, so the wheel takes on the order of 740 a second.
 *
 * The excess has to go somewhere. A plain queue makes it latency: 0.1.23
 * shipped one and the tester's log ends "the writer dropped 12860 packet(s)",
 * which is a queue that spent the session full, so every force waited behind
 * a hundred and twenty seven others before reaching the wheel. He felt it as
 * a wheel with no resistance to a quick turn, which is what a force arriving
 * a fifth of a second late is.
 *
 * So the queue coalesces. The wheel holds one value per parameter, so a
 * packet still waiting when a newer one arrives for the same parameter was
 * never going to be felt: the newer value takes its place, in its place. The
 * depth is then bounded by how many distinct packets exist rather than by
 * how fast the game talks, and the wheel always gets the freshest state the
 * daemon has.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>

#include "wirequeue.h"

/*
 * What makes two packets the same parameter. Every packet the daemon builds
 * starts with an opcode and then names what it addresses: ff_first and
 * ff_update carry a little endian parameter id at bytes 1 and 2, ff_commit
 * and the control packet carry the slot at byte 1, and the settings, gain
 * and input packets are short and identify themselves by their own second
 * byte. Three bytes and the length cover all of them.
 *
 * The length matters because a condition's ff_first and its ff_update share
 * the opcode 0x05 and are told apart by the ids at bytes 1 and 2, and a
 * length that also differed would be a second guard. It costs nothing to
 * have both.
 *
 * A control packet's mode is byte 2, so a play and a stop for one slot are
 * different keys and neither can swallow the other. That is the one merge
 * that would be a bug rather than an economy.
 */
static int
same_parameter(const struct t150_wire *a, const uint8_t *buf, size_t len)
{
	size_t n;

	if (a->len != len)
		return 0;

	n = len < 3 ? len : 3;

	return memcmp(a->buf, buf, n) == 0;
}

void
t150_wq_init(struct t150_wirequeue *q)
{
	memset(q, 0, sizeof(*q));
}

/*
 * Everything waiting is thrown away. Called when the wheel has gone: there
 * is nowhere to put these, and a wheel that comes back is re-uploaded from
 * the session rather than from whatever was in flight when it left.
 */
void
t150_wq_clear(struct t150_wirequeue *q)
{
	q->tail = q->head;
}

unsigned int
t150_wq_depth(const struct t150_wirequeue *q)
{
	return q->head - q->tail;
}

/*
 * Newest value, oldest position. Replacing a waiting packet where it stands
 * is what keeps the order the daemon asked for: a queue that moved the merged
 * packet to the back could put an update after the commit that defines it, or
 * a level after the stop that ended it, and both of those are wrong on the
 * wire.
 *
 * The cost of holding the position is that a value can be written earlier
 * than the daemon asked for it, ahead of another packet queued in between.
 * That is sound here because the wheel keeps one value per parameter and the
 * order between different parameters is untouched: whatever the daemon last
 * said about each is what the wheel ends up holding.
 *
 * Returns 0 whether the packet was appended or merged, and -1 only if it
 * cannot be represented.
 */
int
t150_wq_push(struct t150_wirequeue *q, const uint8_t *buf, size_t len)
{
	unsigned int i;

	if (len == 0 || len > sizeof(q->ring[0].buf))
		return -1;

	for (i = q->tail; i != q->head; i++) {
		struct t150_wire *w = &q->ring[i % T150_WQ_MAX];

		if (!same_parameter(w, buf, len))
			continue;

		memcpy(w->buf, buf, len);
		q->merged++;

		return 0;
	}

	/*
	 * Nothing waiting addresses this, so it is new work. Full here means
	 * the wheel has stopped taking writes altogether rather than merely
	 * lagging, since coalescing has already collapsed everything it can:
	 * the oldest goes, because it is the one most likely to have been
	 * overtaken by the state of the car.
	 */
	if (q->head - q->tail >= T150_WQ_MAX) {
		q->tail++;
		q->dropped++;
	}

	q->ring[q->head % T150_WQ_MAX].len = (uint8_t)len;
	memcpy(q->ring[q->head % T150_WQ_MAX].buf, buf, len);
	q->head++;

	return 0;
}

/* One packet out, oldest first, or 0 if there is nothing waiting. */
int
t150_wq_pop(struct t150_wirequeue *q, struct t150_wire *out)
{
	if (q->head == q->tail)
		return 0;

	*out = q->ring[q->tail % T150_WQ_MAX];
	q->tail++;

	return 1;
}
