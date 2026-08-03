/*
 * usage_check - every option a probe advertises must be one it accepts.
 *
 * The probe tools are macOS only, so nothing on Linux ever runs them and CI
 * only ever compiles them. That leaves a whole class of bug invisible: an
 * option can be documented in usage(), given a case in the switch, and left
 * out of the getopt() string, and the compiler is perfectly happy because the
 * case is merely unreachable. probe_intr shipped exactly that for -H, and a
 * hardware session was spent discovering it from "illegal option -- H".
 *
 * So read the sources instead. This parses each probe's getopt string and its
 * usage text and checks they describe the same tool. It needs no wheel, no
 * Mac, and no linking against IOKit.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>

#ifndef PROBE_SRC_DIR
#define PROBE_SRC_DIR "src/probe"
#endif

#define MAX_SRC		(256 * 1024)

static const char *const probes[] = {
	"probe_hid", "probe_setreport", "probe_ep0", "probe_intr"
};

static int failures;

static char *
slurp(const char *name, char *buf, size_t buflen)
{
	char path[512];
	size_t n;
	FILE *f;

	if ((size_t)snprintf(path, sizeof(path), "%s/%s.c", PROBE_SRC_DIR,
	    name) >= sizeof(path)) {
		fprintf(stderr, "FAIL %s: path too long\n", name);
		failures++;
		return NULL;
	}
	if ((f = fopen(path, "r")) == NULL) {
		fprintf(stderr, "FAIL %s: cannot open %s\n", name, path);
		failures++;
		return NULL;
	}
	n = fread(buf, 1, buflen - 1, f);
	(void)fclose(f);
	buf[n] = '\0';

	return buf;
}

/*
 * Pull the option letters out of the getopt() string literal. It is the
 * argument after "argv, ", so find that rather than guessing at quoting.
 */
static int
getopt_letters(const char *src, const char *name, char *out, size_t outlen)
{
	const char *p, *q;
	size_t n = 0;

	if ((p = strstr(src, "getopt(argc, argv, \"")) == NULL) {
		fprintf(stderr, "FAIL %s: no getopt() call found\n", name);
		failures++;
		return -1;
	}
	p += strlen("getopt(argc, argv, \"");
	if ((q = strchr(p, '"')) == NULL) {
		fprintf(stderr, "FAIL %s: unterminated getopt string\n", name);
		failures++;
		return -1;
	}

	for (; p < q; p++) {
		if (*p == ':')
			continue;
		if (n + 1 >= outlen) {
			fprintf(stderr, "FAIL %s: getopt string too long\n",
			    name);
			failures++;
			return -1;
		}
		out[n++] = *p;
	}
	out[n] = '\0';

	return 0;
}

/*
 * Every option the usage text describes. These are written as "  -x " at the
 * start of a line inside the usage string, which is the shape every probe
 * uses for its option list.
 */
static int
usage_letters(const char *src, const char *name, char *out, size_t outlen)
{
	const char *p;
	size_t n = 0;

	for (p = src; (p = strstr(p, "\\n\"")) != NULL; ) {
		p += 3;
		while (*p == '\n' || *p == '\t' || *p == ' ')
			p++;
		if (*p != '"')
			continue;
		p++;
		if (p[0] != ' ' || p[1] != ' ' || p[2] != '-')
			continue;
		if (p[3] == '\0' || p[4] != ' ')
			continue;
		if (memchr(out, p[3], n) != NULL)
			continue;
		if (n + 1 >= outlen) {
			fprintf(stderr, "FAIL %s: too many usage options\n",
			    name);
			failures++;
			return -1;
		}
		out[n++] = p[3];
	}
	out[n] = '\0';

	(void)name;
	return 0;
}

static void
check(const char *name)
{
	static char src[MAX_SRC];
	char opts[64], doc[64];
	size_t i;

	if (slurp(name, src, sizeof(src)) == NULL)
		return;
	if (getopt_letters(src, name, opts, sizeof(opts)) != 0)
		return;
	if (usage_letters(src, name, doc, sizeof(doc)) != 0)
		return;

	if (doc[0] == '\0') {
		fprintf(stderr, "FAIL %s: usage text lists no options\n", name);
		failures++;
		return;
	}

	/* Advertised but unreachable, which is the bug this exists for. */
	for (i = 0; doc[i] != '\0'; i++) {
		if (strchr(opts, doc[i]) == NULL) {
			fprintf(stderr, "FAIL %s: usage documents -%c but "
			    "getopt does not accept it\n", name, doc[i]);
			failures++;
		}
	}

	/* Accepted but undocumented, which is milder and still worth knowing. */
	for (i = 0; opts[i] != '\0'; i++) {
		if (strchr(doc, opts[i]) == NULL) {
			fprintf(stderr, "FAIL %s: getopt accepts -%c but the "
			    "usage text does not mention it\n", name, opts[i]);
			failures++;
		}
	}
}

int
main(void)
{
	size_t i;

	for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
		check(probes[i]);

	if (failures != 0) {
		fprintf(stderr, "usage_check: %d failure(s)\n", failures);
		return 1;
	}

	printf("usage_check: ok\n");
	return 0;
}
