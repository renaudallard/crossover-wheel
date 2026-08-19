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
# override, so that these survive a command line CFLAGS. Without it "make
# CFLAGS=-O0" silently dropped the language standard and every warning flag.
override CFLAGS += -std=c11 $(WARNINGS) $(EXTRA_CFLAGS)

UNAME_S := $(shell uname -s)

#
# The oldest macOS this builds for, which is the one dist/Info.plist promises.
# Saying it to the compiler is what makes it enforce it: an API newer than this
# becomes a warning, and strict turns that into an error.
#
# Twenty six because that is the only release any of this has been measured
# on, and the measurement is the design. RESEARCH.md B6: macOS 26 added an
# fClientSeized check to setReport, so a non-seizing write fails the moment
# anything else seizes the device, and B6 says in as many words that older
# IOHIDFamily sources are misleading here. The whole non-seizing open rests on
# that reading. HANDOFF.md and README.md have said "macOS 26 or newer" since
# the beginning; this makes the build and the bundle say it too.
#
# It was briefly 12, on the narrower ground that kIOMainPortDefault is named
# kIOMasterPortDefault before then. True, and beside the point: naming an
# older floor claims releases nobody has ever run this on.
#
MACOS_MIN = 26.0

ifeq ($(UNAME_S),Darwin)
# Apple hides err(3) and parts of IOKit behind the Darwin namespace, which
# -std=c11 would otherwise switch off.
CPPFLAGS  += -D_DARWIN_C_SOURCE
override CFLAGS += -mmacosx-version-min=$(MACOS_MIN)
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

# The headers both ends of the wire protocol share. The cross build compiles
# straight from sources to a PE, so it produces no .o for -MMD to write a .d
# beside and the include at the bottom of this file never learns about it.
# Naming them is what keeps the two ends together: without this a change to
# include/t150/proto.h relinked the daemon and left the proxy at the previous
# build, with make reporting success and nothing anywhere saying the two no
# longer agreed about the protocol.
SHARED_HDRS = $(wildcard include/t150/*.h)

# Everything the proxy is built from, named once. The rule below and the
# version stamped into it are the same list and have to stay the same list.
DLL_DEPS   = $(DLL_SRCS) src/dll/proxy.h src/dll/dinput8.def $(SHARED_HDRS)

# The proxy logs this at wrap time, so a log always says which build is in
# the bottle.
#
# The last commit that touched the list above, rather than git describe. The
# application decides whether a bottle holds an old proxy by comparing it with
# the one in its own bundle byte for byte, which is the only record there is,
# so a release stamped in here made every release a new proxy: 0.3.0 and 0.3.1
# have not one commit between them under src/dll, and the menu still offered to
# replace the one in the bottle, the install still replaced it, and nothing
# about the game changed. This changes when the proxy changes and at no other
# time. Empty as well as failed becomes "unknown": git log succeeds and prints
# nothing for a checkout with no history for those paths.
DLL_VERSION := $(shell v=$$(git log -1 --abbrev=12 --format=%h -- \
	         $(DLL_DEPS) 2>/dev/null); echo "$${v:-unknown}")

# Which tree a build came from, for tools that are not the proxy. git describe
# is factual and needs no version decision.
BUILD_VERSION := $(shell git describe --tags --always 2>/dev/null || \
		 echo unknown)

DLL_CPPFLAGS = -Iinclude -Isrc/dll -DT150_PROXY_VERSION=\"$(DLL_VERSION)\"
DLL_CFLAGS   = -O2 -std=c11 $(WARNINGS) $(EXTRA_CFLAGS)
DLL_LIBS     = -ldxguid -luuid -lole32 -lws2_32

.PHONY: all probes tools daemon dll test check check-mac strict clean help \
        install app dmg probe-zip

ifeq ($(HAVE_DLL_CC),yes)
DLL_TARGET = dll
else
DLL_TARGET =
endif

ifeq ($(UNAME_S),Darwin)
APP_TARGET = app
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
	@echo "  dmg      the disk image people download: the app and a"
	@echo "           shortcut to Applications, no password (macOS only)"
	@echo "  probe-zip  the other published archive: probe_dinput.exe and"
	@echo "           dist/README.probe, for looking inside a bottle"
	@echo "           (needs mingw-w64 and zip)"
	@echo "  install  run install.sh, which puts the binaries on your PATH"
	@echo "           and the proxy into a bottle you pick (macOS)"
	@echo "           pass arguments with INSTALL_ARGS, e.g."
	@echo "           make install INSTALL_ARGS='-n -b Games'"
	@echo "  strict   same as all, but warnings are errors (used by CI)"
	@echo "  check-mac  syntax check the Cocoa and IOKit sources against"
	@echo "           tests/stubs, so they can be checked off a Mac"
	@echo "           (needs clang)"
	@echo "  clean    remove the build directory"

daemon: $(DAEMON_BIN)

# Whether these can be built at all is decided by whether the rules exist, not
# by a test inside a recipe, for the reason the probes and the tools give
# below: a recipe cannot stop make building what it was told a target depends
# on. It matters here because the bundle names the proxy among the files it
# packages. A Mac has no cross compiler and takes its proxy prebuilt from the
# job that does, and that copy is older than the checkout it lands in, so a
# rule here would have make try to rebuild it and stop on a compiler that was
# never going to be there. With no rule a proxy already in place is a file like
# any other.
ifeq ($(HAVE_DLL_CC),yes)
dll: $(DLL_BIN) $(DLL_CHECK_BIN) $(DINPUT_PROBE_BIN)

$(DLL_BIN): $(DLL_DEPS) | $(BIN)
	$(DLL_CC) $(DLL_CPPFLAGS) $(DLL_CFLAGS) -shared -o $@ $(DLL_SRCS) \
	    src/dll/dinput8.def -static-libgcc $(DLL_LIBS)

# The proxy's own test, which links the sources it checks and loads the DLL
# it was built alongside. Only runnable on Windows, so CI runs it there.
$(DLL_CHECK_BIN): tests/dll_check.c $(DLL_SRCS) src/dll/proxy.h $(SHARED_HDRS) \
    | $(BIN)
	$(DLL_CC) $(DLL_CPPFLAGS) $(DLL_CFLAGS) -o $@ tests/dll_check.c \
	    src/dll/device.c src/dll/effect.c src/dll/client.c \
	    src/lib/proto.c -static-libgcc $(DLL_LIBS)

# The in-bottle diagnostic. It links no project source, only the shared
# header for the wheel's ids, because it exists to report what DirectInput
# says rather than what this project believes.
$(DINPUT_PROBE_BIN): src/tools/probe_dinput.c $(SHARED_HDRS) | $(BIN)
	$(DLL_CC) $(DLL_CPPFLAGS) -DT150_BUILD_VERSION=\"$(BUILD_VERSION)\" \
	    $(DLL_CFLAGS) -o $@ src/tools/probe_dinput.c \
	    -static-libgcc -ldinput8 $(DLL_LIBS)
else
dll:
	@echo "dll: $(DLL_CC) not found, install gcc-mingw-w64-x86-64" >&2
	@false
endif

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

$(OBJ)/%.o: src/tools/%.c | $(OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN)/t150ctl $(BIN)/t150boot: $(BIN)/%: $(OBJ)/%.o $(PROBE_COMMON) \
    $(LIB_OBJS) $(MAC_OBJS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(FRAMEWORKS)

# probe_intr builds its packets with the shared encoders and probe_ep0 sends
# the mode switch, so the probes link the portable library and the macOS half
# beside it, which is the same set the tools link.
$(BIN)/probe_%: $(OBJ)/probe_%.o $(PROBE_COMMON) $(LIB_OBJS) $(MAC_OBJS) \
    | $(BIN)
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

# usage_check reads the sources rather than linking them, because the probes
# themselves only build on macOS. It needs to know where they are. The daemon
# is in there too: it builds anywhere, which is not the same as having its
# options checked anywhere.
$(BIN)/usage_check: tests/usage_check.c | $(BIN)
	$(CC) $(CPPFLAGS) -DPROBE_SRC_DIR='"$(CURDIR)/src/probe"' \
	    -DTOOL_SRC_DIR='"$(CURDIR)/src/tools"' \
	    -DDAEMON_SRC_DIR='"$(CURDIR)/src/t150d"' \
	    -DMAN_DIR='"$(CURDIR)/man"' $(CFLAGS) \
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

# install.sh is shell, so its test is shell. It builds a synthetic CrossOver
# tree under tmp/ and drives the real script against it, which needs no Mac,
# no CrossOver and no wheel: PREFIX, CX_ROOT and BOTTLE_ROOT are what the
# script takes from the environment for exactly this.
test: $(TEST_BINS)
	@for t in $(TEST_BINS); do "$$t" || exit 1; done
	@sh "$(CURDIR)/tests/install_check.sh"
	@sh "$(CURDIR)/tests/update_check.sh"

check: test

# Syntax check the sources no build machine here can compile.
#
# Everything macOS only needs Cocoa or IOKit, so anywhere but a Mac it was
# checked by being read, and reading does not find what -Werror finds. An
# unused parameter left behind by a change is a hard failure of the macOS job
# and therefore of the release that job builds, and it reached that job once.
# tests/stubs holds fake headers declaring only what these files name; what
# this proves is that they parse and are warning clean under the project's own
# flags, and nothing whatever about behaviour.
#
# It began as the application and the HID backend, which left the daemon's boot
# switch, both command line tools and all four probes checkable only by the
# macOS job. They are all here now: a change to any of them used to reach a
# compiler for the first time in CI.
#
# clang rather than $(CC), because GCC's Objective-C front end cannot do
# blocks and that file is full of them. -fobjc-runtime=macosx is the whole
# trick for the .m: ARC is refused on the GNU runtime, and asking for a Darwin
# target instead breaks every libc header the file includes.
#
# Not part of all or of strict. It needs a compiler that may not be installed,
# and on a Mac the real build is a better check than any stub.
STUBS = tests/stubs

# Every macOS only C source, which is the daemon's backend and boot switch,
# both tools and all four probes.
MAC_ONLY_SRCS = src/t150d/hid_darwin.c src/mac/bootswitch.c \
		src/tools/t150ctl.c src/tools/t150boot.c \
		src/probe/common.c src/probe/probe_hid.c \
		src/probe/probe_ep0.c src/probe/probe_setreport.c \
		src/probe/probe_intr.c

check-mac:
	@command -v clang >/dev/null 2>&1 || { \
	    echo "check-mac: needs clang (apt-get install clang)" >&2; exit 1; }
	clang -fsyntax-only -x objective-c -fobjc-runtime=macosx-10.13 \
	    -fobjc-arc -fblocks -I$(STUBS) -Iinclude -Isrc \
	    $(WARNINGS) -Werror src/mac/t150menu.m
	@for f in $(MAC_ONLY_SRCS); do \
	    echo "clang -fsyntax-only ... $$f"; \
	    clang -fsyntax-only -std=c11 -I$(STUBS) -Iinclude -Isrc \
	        -Isrc/probe -Isrc/t150d -D_POSIX_C_SOURCE=200809L \
	        -D_DARWIN_C_SOURCE -D__APPLE__ $(WARNINGS) -Werror "$$f" \
	        || exit 1; \
	done
	@echo "check-mac: ok"

# Warnings as errors, everywhere they can be. EXTRA_CFLAGS rather than
# re-exporting CFLAGS: a command line assignment overrides the += above, so
# "make CFLAGS=... strict" used to hand the sub-make a CFLAGS with neither
# -std=c11 nor a single warning flag in it, leaving -Werror with nothing to
# promote while the target still claimed to be the strict one.
#
# It reaches the application too. That is 1200 lines of Objective-C with no
# tests, which CI compiles in a separate step precisely because compiling it
# is the whole of the automated confidence in it, and it was the one file
# -Werror did not cover.
strict:
	@$(MAKE) --no-print-directory EXTRA_CFLAGS=-Werror all $(APP_TARGET)

# The installer is a shell script rather than a make rule because half of what
# it does is asking a person which bottle they mean, and because it has to
# work the same from an extracted release where there is no Makefile at all.
install:
	@sh "$(CURDIR)/install.sh" $(INSTALL_ARGS)

# crossover-wheel.app: the menu bar item and the graphical installer.
#
# The bundle carries install.sh, the binaries, the man pages and the proxy in
# Resources, because install.sh resolves all of those relative to itself. That
# is what lets the app be a front end over the shell script rather than a
# second implementation of it: the part that can go wrong stays in the script
# that is tested.
# The fallback has to be inside the pipeline, not after it. Written the other
# way round the || bound to the whole pipeline, whose status is sed's, and sed
# succeeds on empty input: echo never ran and the version became the empty
# string. That is the DMG filename and CFBundleShortVersionString, and the
# update check reads the latter back and treats anything unequal to the
# published tag as out of date, so a build from a source tarball nagged about
# an update on every launch. BUILD_VERSION above has the same intent and no
# pipe, which is why it was right.
#
# The tag exactly, never a description of how far past one this is. With
# --always a build three commits along called itself 0.2.0-3-g126100c, which is
# not a release and is not equal to one either, so the update check treated it
# as out of date and offered the tag it had been built from on every launch.
# Anything that is not a release now says 0, which is what the application
# already reads as a source build and declines to compare. BUILD_VERSION keeps
# --always on purpose: it names the build in a log rather than being compared
# against anything. DLL_VERSION is not that, and this comment used to say it
# was: the application compares the proxy byte for byte, so what is stamped
# into it is compared, and stamping the release in there made every release a
# proxy the menu offered to replace for nothing.
REL_VERSION := $(shell { git describe --tags --exact-match 2>/dev/null || \
	         echo 0; } | sed 's/^v//')
#
# The bottle probe archive, the second thing the releases page publishes.
#
# A rule rather than something zipped by hand at release time, because
# README.md promises that each archive's README is "packaged verbatim at
# release time so they cannot drift from what is written here" and nothing
# anywhere made that true for this one: the disk image copies
# dist/README.macos in its own recipe and CI asserts it is there, and this
# archive had no rule, no check and no README in the artifact it is built
# from.
#
PROBE_ZIP = $(BIN)/crossover-wheel-$(REL_VERSION)-bottle-probe.zip

probe-zip: $(PROBE_ZIP)

$(PROBE_ZIP): $(DINPUT_PROBE_BIN) dist/README.probe | $(BIN)
	@command -v zip >/dev/null 2>&1 || { \
	    echo "probe-zip: needs zip" >&2; exit 1; }
	rm -rf $(BUILD)/probe-zip "$@"
	mkdir -p $(BUILD)/probe-zip
	cp $(DINPUT_PROBE_BIN) $(BUILD)/probe-zip/
	cp dist/README.probe $(BUILD)/probe-zip/README.txt
	cd $(BUILD)/probe-zip && zip -q -r "$(CURDIR)/$@" .
	@echo "built $@"

APP	   = $(BIN)/crossover-wheel.app
APP_RES	   = $(APP)/Contents/Resources

# Objective-C rather than C, so it does not take the C standard flag: -std=c11
# is strict ISO and turns off the GNU extensions Cocoa's own headers and ARC
# rely on. The warnings are the same ones everything else is built with.
#
# Built without CPPFLAGS and without -g on purpose. CPPFLAGS carries -MMD -MP,
# which drops a dependency file inside the bundle, and -g leaves a .dSYM
# directory there: both ship to users as debris in an application nobody can
# attach a debugger to anyway. It includes nothing but Cocoa, so it needs no
# include path either.
APP_CFLAGS = -O2 -fobjc-arc -mmacosx-version-min=$(MACOS_MIN) -Iinclude -Isrc \
	     $(WARNINGS) $(EXTRA_CFLAGS)

ifeq ($(UNAME_S),Darwin)
app: $(APP)

# The recipe copies more than it is built from, and make can only see a change
# in something it was told about. install.sh in particular is the file most
# likely to be edited between builds, because the bundle exists to be a front
# end over it, and a bundle that was not rebuilt shipped the previous copy.
#
# The proxy is named through wildcard, because it is the one entry that need
# not exist: a Mac has no cross compiler, so it arrives from the job that does,
# and naming it directly would have make try to build it and fail. Left out
# altogether, which is how this was, a proxy that arrived after the bundle was
# built was never packaged: the bundle was newer than everything make had been
# told about, so the recipe did not run and the copy inside it stayed as it was.
APP_RES_SRCS = install.sh dist/update.sh $(wildcard $(DLL_BIN)) \
	       $(wildcard man/*.1 man/*.7 man/*.8) \
	       $(wildcard dist/icons/AppIcon.iconset/*.png) \
	       $(wildcard dist/icons/menubar/*.png)

$(APP): src/mac/t150menu.m dist/Info.plist $(APP_RES_SRCS) $(TOOL_BINS) \
    $(PROBE_BINS) $(DAEMON_BIN) | $(BIN)
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP_RES)
	sed 's/@VERSION@/$(REL_VERSION)/g' dist/Info.plist \
	    > $(APP)/Contents/Info.plist
	iconutil -c icns dist/icons/AppIcon.iconset -o $(APP_RES)/AppIcon.icns
	cp dist/icons/menubar/*.png $(APP_RES)/
	$(CC) $(APP_CFLAGS) -o $(APP)/Contents/MacOS/t150menu \
	    src/mac/t150menu.m src/mac/bootswitch.c \
	    -framework Cocoa -framework IOKit
	cp install.sh dist/update.sh $(APP_RES)/
	cp $(DAEMON_BIN) $(TOOL_BINS) $(PROBE_BINS) $(APP_RES)/
	cp man/*.1 man/*.7 man/*.8 $(APP_RES)/
	@if [ -f $(DLL_BIN) ]; then cp $(DLL_BIN) $(APP_RES)/; \
	    else echo "note: no $(DLL_BIN), the app will ask for the proxy"; fi
	# Seal the bundle. The linker already ad-hoc signs the executable on
	# Apple Silicon, but nothing seals the bundle around it, and macOS
	# validates the bundle: a quarantined app whose bundle has no
	# _CodeSignature is reported to the user as damaged, with an offer to
	# move it to the bin and no way to proceed. Ad-hoc is not a Developer
	# ID and does not clear Gatekeeper by itself, but it turns that dead
	# end into the ordinary unverified-developer prompt.
	codesign --force --sign - $(APP)
	@codesign --verify --strict $(APP) && echo "bundle signature verifies"
	@echo "built $(APP)"

# The one download: a disk image holding the application and a shortcut to
# Applications, which is the drag-and-drop every Mac user already knows and
# which needs no password, because nothing is written outside the place the
# person drops it.
#
# The application is ad-hoc signed, which matters here. macOS marks anything a
# browser downloads, and a marked application with no signature at all is
# called damaged and offered to the bin with no way forward. With a signature
# it is the ordinary unverified-developer dialog instead, which has a way
# through. Signed by a paid identity it would have none of this, and that is
# the only thing missing.
DMG	    = $(BIN)/crossover-wheel-$(REL_VERSION).dmg
DMG_STAGE   = $(BUILD)/dmg

dmg: $(DMG)

$(DMG): $(APP) dist/README.macos
	rm -rf $(DMG_STAGE) $(DMG)
	mkdir -p $(DMG_STAGE)
	cp -R $(APP) $(DMG_STAGE)/
	ln -s /Applications $(DMG_STAGE)/Applications
	# The README the project's own README says every archive carries, and
	# which nothing copied anywhere until now.
	cp dist/README.macos $(DMG_STAGE)/README.txt
	hdiutil create -volname crossover-wheel -srcfolder $(DMG_STAGE) \
	    -fs HFS+ -format UDZO -ov $(DMG)
	@echo "built $(DMG)"

else
app:
	@echo "app: macOS only, nothing to do here" >&2

dmg:
	@echo "dmg: macOS only, nothing to do here" >&2
endif


clean:
	rm -rf $(BUILD)

DEPS = $(shell find $(OBJ) -name '*.d' 2>/dev/null)
-include $(DEPS)
