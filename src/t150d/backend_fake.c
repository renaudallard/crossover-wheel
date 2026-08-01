/*
 * backend_fake.c - a backend that drives nothing and records everything.
 *
 * This is what makes the daemon falsifiable on a machine with no wheel and
 * no macOS: every packet the real backend would put on the wire lands in a
 * log instead, in the same form probe_setreport prints, so the two can be
 * compared by eye or by a test.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>

#include "t150d.h"

static int
fake_write(void *priv, const uint8_t *buf, size_t len)
{
	FILE *fp = priv;
	size_t i;

	if (fprintf(fp, "write %zu:", len) < 0)
		return -1;
	for (i = 0; i < len; i++) {
		if (fprintf(fp, " %02x", buf[i]) < 0)
			return -1;
	}
	if (fputc('\n', fp) == EOF)
		return -1;

	/*
	 * Flushed per packet: a test reading this through a pipe has to see
	 * the write that a subsequent hangup is supposed to follow.
	 */
	if (fflush(fp) != 0)
		return -1;

	return 0;
}

int
t150_backend_fake(struct t150_backend *be, FILE *fp)
{
	if (fp == NULL)
		return -1;

	be->name = "fake";
	be->write = fake_write;
	be->close = NULL;
	be->priv = fp;

	return 0;
}
