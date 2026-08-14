/*
 * wirequeue.h - packets waiting for a wheel that is slower than the daemon.
 *
 * Separate from the backend that uses it because the backend is macOS only
 * and cannot be compiled, let alone tested, anywhere else. This is plain C
 * with no clock, no lock and no device, so tests/wirequeue_check.c drives
 * every rule in it on any machine.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef T150D_WIREQUEUE_H
#define T150D_WIREQUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "t150d.h"

/*
 * How many distinct packets may be waiting. Coalescing keeps the real depth
 * near the number of packets that exist at all, which is three per slot plus
 * a control, so this is a backstop rather than a working limit: reaching it
 * means the wheel has genuinely stopped taking writes.
 */
#define T150_WQ_MAX	128u

struct t150_wirequeue {
	struct t150_wire	ring[T150_WQ_MAX];
	unsigned int		head, tail;
	unsigned int		dropped;	/* refused, the queue was full */
	unsigned int		merged;		/* superseded one already in */
};

void		t150_wq_init(struct t150_wirequeue *q);
void		t150_wq_clear(struct t150_wirequeue *q);
unsigned int	t150_wq_depth(const struct t150_wirequeue *q);
int		t150_wq_push(struct t150_wirequeue *q, const uint8_t *buf,
		    size_t len);
int		t150_wq_pop(struct t150_wirequeue *q, struct t150_wire *out);

#endif /* T150D_WIREQUEUE_H */
