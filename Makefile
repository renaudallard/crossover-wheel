# crossover-wheel - force feedback for the Thrustmaster T150 under CrossOver
#
# Copyright (c) 2026 Renaud Allard
# SPDX-License-Identifier: BSD-2-Clause

CC	?= cc
CFLAGS	?= -O2 -g

WARNINGS = -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
	   -Wmissing-prototypes -Wmissing-declarations

# The probes use getopt, nanosleep and PATH_MAX, so ask for POSIX 2008
# explicitly rather than relying on whatever the compiler exposes by default.
CPPFLAGS += -Iinclude -Isrc/probe -Isrc/t150d -D_POSIX_C_SOURCE=200809L
CFLAGS   += -std=c11 $(WARNINGS)

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
# Apple hides err(3) and parts of IOKit behind the Darwin namespace, which
# -std=c11 would otherwise switch off.
CPPFLAGS  += -D_DARWIN_C_SOURCE
FRAMEWORKS = -framework IOKit -framework CoreFoundation
endif

BUILD	= build
BIN	= $(BUILD)/bin
OBJ	= $(BUILD)/obj
LIBOBJ	= $(OBJ)/lib
DAEMONOBJ = $(OBJ)/t150d

PROBE_NAMES  = probe_hid probe_setreport probe_ep0
PROBE_BINS   = $(addprefix $(BIN)/,$(PROBE_NAMES))
PROBE_COMMON = $(OBJ)/common.o

# The portable half: no I/O, no platform calls, tested everywhere.
LIB_NAMES = encode proto
LIB_OBJS  = $(addprefix $(LIBOBJ)/,$(addsuffix .o,$(LIB_NAMES)))

# The daemon, minus main, so the tests can drive the session directly.
DAEMON_LIB_NAMES = session backend_fake
DAEMON_LIB_OBJS  = $(addprefix $(DAEMONOBJ)/,$(addsuffix .o,$(DAEMON_LIB_NAMES)))
DAEMON_BIN	 = $(BIN)/t150d

TEST_NAMES = header_check encode_check proto_check daemon_check socket_check
TEST_BINS  = $(addprefix $(BIN)/,$(TEST_NAMES))

.PHONY: all probes daemon test check strict clean help

ifeq ($(UNAME_S),Darwin)
all: probes daemon test
else
all: daemon test
	@echo
	@echo "Note: the probe tools are macOS only and were not built here."
	@echo "      Everything portable was built and tested."
endif

help:
	@echo "targets:"
	@echo "  all      build what this platform can build, then run tests"
	@echo "  probes   build the three Phase 0 probe tools (macOS only)"
	@echo "  daemon   build t150d"
	@echo "  test     build and run the portable tests"
	@echo "  strict   same as all, but warnings are errors (used by CI)"
	@echo "  clean    remove the build directory"

daemon: $(DAEMON_BIN)

probes: $(PROBE_BINS)
ifneq ($(UNAME_S),Darwin)
	@echo "probes: macOS only, nothing to do on $(UNAME_S)" >&2
	@false
endif

$(BIN) $(OBJ) $(LIBOBJ) $(DAEMONOBJ):
	@mkdir -p $@

$(OBJ)/%.o: src/probe/%.c | $(OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(LIBOBJ)/%.o: src/lib/%.c | $(LIBOBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(DAEMONOBJ)/%.o: src/t150d/%.c | $(DAEMONOBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN)/probe_%: $(OBJ)/probe_%.o $(PROBE_COMMON) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(FRAMEWORKS)

$(DAEMON_BIN): $(DAEMONOBJ)/main.o $(DAEMON_LIB_OBJS) $(LIB_OBJS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# daemon_check drives the session, so it needs the daemon's own objects.
$(BIN)/daemon_check: tests/daemon_check.c $(DAEMON_LIB_OBJS) $(LIB_OBJS) | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# socket_check runs the real daemon, so it needs the binary rather than the
# objects. Order-only, because it execs it instead of linking it.
$(BIN)/socket_check: tests/socket_check.c $(LIB_OBJS) | $(BIN) $(DAEMON_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN)/%_check: tests/%_check.c $(LIB_OBJS) | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do "$$t" || exit 1; done

check: test

strict:
	@$(MAKE) --no-print-directory CFLAGS="$(CFLAGS) -Werror" all

clean:
	rm -rf $(BUILD)
