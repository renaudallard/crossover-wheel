/*
 * usage_check - every option a program advertises must be one it accepts.
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

#ifndef TOOL_SRC_DIR
#define TOOL_SRC_DIR "src/tools"
#endif

#ifndef DAEMON_SRC_DIR
#define DAEMON_SRC_DIR "src/t150d"
#endif

#define MAX_SRC		(256 * 1024)

/*
 * Every program that takes options, not just the probes. t150ctl and t150boot
 * ship to users and cannot run here either, so the same trap applies to them.
 *
 * t150d builds and runs here, which is not the same as being checked: nothing
 * ran it with an option until this listed it, and the trap does not need macOS.
 * An option can sit in usage() and in the switch with no letter in the getopt
 * string, and the compiler stays quiet because the case is merely unreachable.
 */
static const char *const sources[] = {
	PROBE_SRC_DIR "/probe_hid.c",
	PROBE_SRC_DIR "/probe_setreport.c",
	PROBE_SRC_DIR "/probe_ep0.c",
	PROBE_SRC_DIR "/probe_intr.c",
	TOOL_SRC_DIR "/t150ctl.c",
	TOOL_SRC_DIR "/t150boot.c",
	DAEMON_SRC_DIR "/main.c"
};

static int failures;

static char *
slurp(const char *path, char *buf, size_t buflen)
{
	size_t n;
	FILE *f;

	if ((f = fopen(path, "r")) == NULL) {
		fprintf(stderr, "FAIL: cannot open %s\n", path);
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

	for (i = 0; i < sizeof(sources) / sizeof(sources[0]); i++)
		check(sources[i]);

	if (failures != 0) {
		fprintf(stderr, "usage_check: %d failure(s)\n", failures);
		return 1;
	}

	printf("usage_check: ok\n");
	return 0;
}
