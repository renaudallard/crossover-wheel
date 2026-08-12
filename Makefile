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
# -MMD -MP writes a .d beside every .o listing the headers it used, and the
# include at the end of this file feeds them back. Without it a change to
# include/t150/*.h leaves every object stale, which is how a corrected gain
# constant once passed its own test suite unnoticed.
CPPFLAGS += -Iinclude -Isrc -Isrc/probe -Isrc/t150d -D_POSIX_C_SOURCE=200809L -MMD -MP
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
MACOBJ	= $(OBJ)/mac

PROBE_NAMES  = probe_hid probe_setreport probe_ep0 probe_intr
PROBE_BINS   = $(addprefix $(BIN)/,$(PROBE_NAMES))
PROBE_COMMON = $(OBJ)/common.o

# The shipped command line tools. macOS only, like the probes, and they share
# the probes' enumeration because they have the same lifecycle: find the
# wheel, do one thing, let go. The daemon does not, which is why it has its
# own.
TOOL_NAMES = t150ctl t150boot
TOOL_BINS  = $(addprefix $(BIN)/,$(TOOL_NAMES))

# macOS only code that is not a probe, a tool or the daemon, because more than
# one of them needs it. Taking the wheel out of boot mode is the whole of it:
# t150boot does it on demand and the daemon does it after a replug.
MAC_OBJS =
ifeq ($(UNAME_S),Darwin)
MAC_OBJS += $(MACOBJ)/bootswitch.o
endif

# The portable half: no I/O, no platform calls, tested everywhere.
LIB_NAMES = encode proto
LIB_OBJS  = $(addprefix $(LIBOBJ)/,$(addsuffix .o,$(LIB_NAMES)))

# The daemon, minus main, so the tests can drive the session directly. The
# real backend only exists on macOS, and the tests never link it: they drive
# the logging one, which is the point of having it.
DAEMON_LIB_NAMES = session backend_fake wirequeue
DAEMON_EXTRA_OBJS =
ifeq ($(UNAME_S),Darwin)
DAEMON_EXTRA_OBJS += $(DAEMONOBJ)/hid_darwin.o $(MAC_OBJS)
endif
DAEMON_LIB_OBJS  = $(addprefix $(DAEMONOBJ)/,$(addsuffix .o,$(DAEMON_LIB_NAMES)))
DAEMON_BIN	 = $(BIN)/t150d

TEST_NAMES = header_check encode_check proto_check daemon_check socket_check \
             usage_check wirequeue_check
TEST_BINS  = $(addprefix $(BIN)/,$(TEST_NAMES))

# The in-bottle proxy. A PE, so it is cross compiled, and only when the
# cross compiler is here: it is not needed to build or test anything else.
#
# x86_64 is the only sensible target. CrossOver 26 bottles are x86_64 under
# Rosetta, and CrossOver 27's ARM64 bottles are ARM64EC, which still loads an
# x86_64 PE. 32-bit bottles are gone in 27, so i386 is never worth building.
DLL_CC	  ?= x86_64-w64-mingw32-gcc
HAVE_DLL_CC := $(shell command -v $(DLL_CC) >/dev/null 2>&1 && echo yes)
DLL_BIN	   = $(BIN)/t150-dinput8.dll
DLL_CHECK_BIN = $(BIN)/dll_check.exe
DINPUT_PROBE_BIN = $(BIN)/probe_dinput.exe
DLL_SRCS   = src/dll/main.c src/dll/device.c src/dll/effect.c \
	     src/dll/client.c src/lib/proto.c
# The proxy logs this at wrap time, so a log always says which build is in
# the bottle. git describe is factual and needs no version decision.
DLL_VERSION := $(shell git describe --tags --always 2>/dev/null || echo unknown)
DLL_CPPFLAGS = -Iinclude -Isrc/dll -DT150_PROXY_VERSION=\"$(DLL_VERSION)\"
DLL_CFLAGS   = -O2 -std=c11 $(WARNINGS)
DLL_LIBS     = -ldxguid -luuid -lole32 -lws2_32

.PHONY: all probes tools daemon dll test check strict clean help install app

ifeq ($(HAVE_DLL_CC),yes)
DLL_TARGET = dll
else
DLL_TARGET =
endif

ifeq ($(UNAME_S),Darwin)
all: probes tools daemon $(DLL_TARGET) test
else
all: daemon $(DLL_TARGET) test
	@echo
	@echo "Note: the probes and the t150ctl/t150boot tools are macOS only"
	@echo "      and were not built here."
	@echo "      Everything portable was built and tested."
endif

help:
	@echo "targets:"
	@echo "  all      build what this platform can build, then run tests"
	@echo "  probes   build the four Phase 0 probe tools (macOS only)"
	@echo "  tools    build t150ctl and t150boot (macOS only)"
	@echo "  daemon   build t150d"
	@echo "  dll      cross build the in-bottle proxy (needs mingw-w64)"
	@echo "  test     build and run the portable tests"
	@echo "  app      build crossover-wheel.app, the menu bar item and"
	@echo "           graphical installer (macOS only)"
	@echo "  install  run install.sh, which puts the binaries on your PATH"
	@echo "           and the proxy into a bottle you pick (macOS)"
	@echo "           pass arguments with INSTALL_ARGS, e.g."
	@echo "           make install INSTALL_ARGS='-n -b Games'"
	@echo "  strict   same as all, but warnings are errors (used by CI)"
	@echo "  clean    remove the build directory"

daemon: $(DAEMON_BIN)

dll: $(DLL_BIN) $(DLL_CHECK_BIN) $(DINPUT_PROBE_BIN)

$(DLL_BIN): $(DLL_SRCS) src/dll/proxy.h src/dll/dinput8.def | $(BIN)
ifneq ($(HAVE_DLL_CC),yes)
	@echo "dll: $(DLL_CC) not found, install gcc-mingw-w64-x86-64" >&2
	@false
endif
	$(DLL_CC) $(DLL_CPPFLAGS) $(DLL_CFLAGS) -shared -o $@ $(DLL_SRCS) \
	    src/dll/dinput8.def -static-libgcc $(DLL_LIBS)

# The proxy's own test, which links the sources it checks and loads the DLL
# it was built alongside. Only runnable on Windows, so CI runs it there.
$(DLL_CHECK_BIN): tests/dll_check.c $(DLL_SRCS) src/dll/proxy.h | $(BIN)
	$(DLL_CC) $(DLL_CPPFLAGS) $(DLL_CFLAGS) -o $@ tests/dll_check.c \
	    src/dll/device.c src/dll/effect.c src/dll/client.c \
	    src/lib/proto.c -static-libgcc $(DLL_LIBS)

# The in-bottle diagnostic. It links no project source, only the shared
# header for the wheel's ids, because it exists to report what DirectInput
# says rather than what this project believes.
$(DINPUT_PROBE_BIN): src/tools/probe_dinput.c include/t150/t150.h | $(BIN)
	$(DLL_CC) $(DLL_CPPFLAGS) $(DLL_CFLAGS) -o $@ src/tools/probe_dinput.c \
	    -static-libgcc -ldinput8 $(DLL_LIBS)

# Guarded by the prerequisites, not by the recipe. A recipe cannot stop make
# building what it was told the target depends on, so the old form still
# reached the compiler and died on a missing IOKit header before its own
# message could explain why.
ifeq ($(UNAME_S),Darwin)
probes: $(PROBE_BINS)
tools: $(TOOL_BINS)
else
probes tools:
	@echo "$@: macOS only, nothing to do on $(UNAME_S)" >&2
	@false
endif

$(BIN) $(OBJ) $(LIBOBJ) $(DAEMONOBJ) $(MACOBJ):
	@mkdir -p $@

$(OBJ)/%.o: src/probe/%.c | $(OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(LIBOBJ)/%.o: src/lib/%.c | $(LIBOBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(DAEMONOBJ)/%.o: src/t150d/%.c | $(DAEMONOBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(MACOBJ)/%.o: src/mac/%.c | $(MACOBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# probe_intr builds its packets with the shared encoders, so the probes link
# the portable library too.
$(OBJ)/%.o: src/tools/%.c | $(OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN)/t150ctl $(BIN)/t150boot: $(BIN)/%: $(OBJ)/%.o $(PROBE_COMMON) \
    $(LIB_OBJS) $(MAC_OBJS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(FRAMEWORKS)

$(BIN)/probe_%: $(OBJ)/probe_%.o $(PROBE_COMMON) $(LIB_OBJS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(FRAMEWORKS)

$(DAEMON_BIN): $(DAEMONOBJ)/main.o $(DAEMON_LIB_OBJS) $(DAEMON_EXTRA_OBJS) \
    $(LIB_OBJS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(FRAMEWORKS)

# daemon_check drives the session, so it needs the daemon's own objects.
$(BIN)/daemon_check: tests/daemon_check.c $(DAEMON_LIB_OBJS) $(LIB_OBJS) | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# socket_check runs the real daemon, so it needs the binary rather than the
# objects. Order-only, because it execs it instead of linking it.
$(BIN)/socket_check: tests/socket_check.c $(LIB_OBJS) | $(BIN) $(DAEMON_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# usage_check reads the probe sources rather than linking them, because the
# probes themselves only build on macOS. It needs to know where they are.
$(BIN)/usage_check: tests/usage_check.c | $(BIN)
	$(CC) $(CPPFLAGS) -DPROBE_SRC_DIR='"$(CURDIR)/src/probe"' \
	    -DTOOL_SRC_DIR='"$(CURDIR)/src/tools"' $(CFLAGS) \
	    -o $@ tests/usage_check.c $(LDFLAGS)

# wirequeue_check drives the writer's queue, which lives with the daemon. The
# queue has no clock, no lock and no device, which is why it can be checked on
# a machine with none of them. It links the encoders too, so the rule that no
# two slots may look alike is checked against the packets the daemon really
# builds rather than against bytes copied into the test.
$(BIN)/wirequeue_check: tests/wirequeue_check.c $(DAEMONOBJ)/wirequeue.o \
    $(LIB_OBJS) | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN)/%_check: tests/%_check.c $(LIB_OBJS) | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do "$$t" || exit 1; done

check: test

strict:
	@$(MAKE) --no-print-directory CFLAGS="$(CFLAGS) -Werror" \
	    DLL_CFLAGS="$(DLL_CFLAGS) -Werror" all

# The installer is a shell script rather than a make rule because half of what
# it does is asking a person which bottle they mean, and because it has to
# work the same from an extracted release where there is no Makefile at all.
install:
	@sh $(CURDIR)/install.sh $(INSTALL_ARGS)

# crossover-wheel.app: the menu bar item and the graphical installer.
#
# The bundle carries install.sh, the binaries, the man pages and the proxy in
# Resources, because install.sh resolves all of those relative to itself. That
# is what lets the app be a front end over the shell script rather than a
# second implementation of it: the part that can go wrong stays in the script
# that is tested.
APP	   = $(BIN)/crossover-wheel.app
APP_RES	   = $(APP)/Contents/Resources

ifeq ($(UNAME_S),Darwin)
app: $(APP)

$(APP): src/mac/t150menu.m dist/Info.plist $(TOOL_BINS) $(PROBE_BINS) \
    $(DAEMON_BIN) | $(BIN)
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP_RES)
	cp dist/Info.plist $(APP)/Contents/Info.plist
	$(CC) $(CPPFLAGS) $(CFLAGS) -fobjc-arc -o $(APP)/Contents/MacOS/t150menu \
	    src/mac/t150menu.m -framework Cocoa
	cp install.sh $(APP_RES)/
	cp $(DAEMON_BIN) $(TOOL_BINS) $(PROBE_BINS) $(APP_RES)/
	cp man/*.1 man/*.7 man/*.8 $(APP_RES)/
	@if [ -f $(DLL_BIN) ]; then cp $(DLL_BIN) $(APP_RES)/; \
	    else echo "note: no $(DLL_BIN), the app will ask for the proxy"; fi
	@echo "built $(APP)"
else
app:
	@echo "app: macOS only, nothing to do here" >&2
endif


clean:
	rm -rf $(BUILD)

DEPS = $(shell find $(OBJ) -name '*.d' 2>/dev/null)
-include $(DEPS)
