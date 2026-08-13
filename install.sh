#!/bin/sh
#
# install.sh - put crossover-wheel where macOS and a CrossOver bottle expect it.
#
# Two halves, because the software has two halves. The macOS binaries go
# somewhere on your PATH; the proxy goes inside a bottle, next to the builtin
# it chain-loads, with a registry override telling Wine to prefer it. The
# second half is the part nobody gets right by hand: the file to copy as
# dinput8_orig.dll is CrossOver's builtin and not the placeholder of the same
# name already sitting in system32, and copying the wrong one fails later and
# more confusingly than it fails here.
#
# Written for /bin/sh. macOS ships bash 3.2 and zsh and there is no reason to
# need either.
#
# Copyright (c) 2026 Renaud Allard
# SPDX-License-Identifier: BSD-2-Clause

set -eu

SELF=${0##*/}
PREFIX=${PREFIX:-$HOME/.local}
CX_ROOT=${CX_ROOT:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver}
BOTTLE_ROOT=${BOTTLE_ROOT:-$HOME/Library/Application Support/CrossOver/Bottles}
BOTTLE=""
DLL=""
DRYRUN=0
WANT_BOTTLE=1
WANT_BINARIES=1
WANT_APP=1

MACOS_BINARIES="t150d t150ctl t150boot probe_hid probe_setreport probe_ep0 probe_intr"

usage()
{
	cat >&2 <<EOF
usage: $SELF [-n] [-p prefix] [-b bottle] [-d dll] [--no-bottle] [--no-binaries]

  -p prefix      where the macOS binaries go (default $HOME/.local)
  -b bottle      which CrossOver bottle to install the proxy into, by name.
                 Without this you are shown a list and asked.
  -d dll         path to t150-dinput8.dll. Found automatically if it is
                 beside this script or under build/bin.
  -n             say what would happen and change nothing
  --no-bottle    install only the macOS binaries
  --no-binaries  install only the proxy into a bottle
  --no-app       do not install crossover-wheel.app
  -h             this

Environment: PREFIX, CX_ROOT and BOTTLE_ROOT override the paths above, which
is how this is tested on a machine that is not a Mac.
EOF
	exit 2
}

say()   { printf '%s\n' "$*"; }

# macOS gates an application reaching inside another application's bundle, and
# CrossOver's builtin dinput8.dll is inside CrossOver.app. Blocked, it does not
# announce itself: the file simply cannot be read, which looks exactly like it
# not being there. Saying this is the difference between a person granting one
# permission and a person concluding the installer is broken.
app_management_hint()
{
	warn ""
	warn "If this was run from the crossover-wheel application, macOS has"
	warn "most likely blocked it from reaching inside CrossOver.app."
	warn "Open System Settings, Privacy and Security, App Management, turn"
	warn "on crossover-wheel, and try again."
	warn ""
	warn "Running this script from a terminal instead is the other way"
	warn "round it, since a terminal already has that permission."
}
step()  { printf '\n==> %s\n' "$*"; }
warn()  { printf '%s: %s\n' "$SELF" "$*" >&2; }
die()   { warn "$*"; exit 1; }

# Everything that writes goes through here, so -n is honest by construction.
run()
{
	if [ "$DRYRUN" -eq 1 ]; then
		printf '  would: %s\n' "$*"
		return 0
	fi
	"$@"
}

# A Wine builtin says so in its DOS stub, starting at byte 64. Reading 128
# bytes covers it; 64 covers nothing on any file, right or wrong.
is_wine_builtin()
{
	head -c 128 "$1" 2>/dev/null | LC_ALL=C grep -a -q 'Wine builtin DLL'
}

# Our proxy is a PE, and carries strings no builtin has. Both halves matter:
# this repository's own README mentions T150_ENDPOINT, so the marker alone
# says yes to a text file, and -d pointed at the wrong thing should fail here
# rather than inside a game.
is_our_proxy()
{
	[ "$(head -c 2 "$1" 2>/dev/null)" = "MZ" ] || return 1
	LC_ALL=C grep -a -q 'T150_ENDPOINT' "$1" 2>/dev/null
}

while [ $# -gt 0 ]; do
	case $1 in
	-p)	[ $# -ge 2 ] || usage; PREFIX=$2; shift 2 ;;
	-b)	[ $# -ge 2 ] || usage; BOTTLE=$2; shift 2 ;;
	-d)	[ $# -ge 2 ] || usage; DLL=$2; shift 2 ;;
	-n)	DRYRUN=1; shift ;;
	--no-bottle)	WANT_BOTTLE=0; shift ;;
	--no-binaries)	WANT_BINARIES=0; shift ;;
	--no-app)	WANT_APP=0; shift ;;
	-h|--help)	usage ;;
	*)	warn "unknown argument: $1"; usage ;;
	esac
done

[ "$WANT_BINARIES" -eq 1 ] || [ "$WANT_BOTTLE" -eq 1 ] ||
    die "--no-bottle and --no-binaries together leave nothing to do"

# Where we were run from, so a release archive and a source tree both work.
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -d "$here/build/bin" ]; then
	SRC=$here/build/bin
	MAN=$here/man
else
	SRC=$here
	MAN=$here
fi

################################################################
# The macOS binaries.
################################################################

install_binaries()
{
	step "macOS binaries into $PREFIX"

	missing=
	for b in $MACOS_BINARIES; do
		[ -f "$SRC/$b" ] || missing="$missing $b"
	done
	if [ -n "$missing" ]; then
		warn "not found in $SRC:$missing"
		die "run make first, or run this from an extracted release"
	fi

	run mkdir -p "$PREFIX/bin" "$PREFIX/share/man/man1" \
	    "$PREFIX/share/man/man7" "$PREFIX/share/man/man8"

	for b in $MACOS_BINARIES; do
		run cp "$SRC/$b" "$PREFIX/bin/$b"
		run chmod 755 "$PREFIX/bin/$b"
		say "  $b"
	done

	# Downloaded through a browser these are quarantined and macOS refuses
	# to run them, because they are unsigned and not notarized. Harmless
	# where there is no quarantine to clear.
	if command -v xattr >/dev/null 2>&1; then
		for b in $MACOS_BINARIES; do
			run xattr -d com.apple.quarantine \
			    "$PREFIX/bin/$b" 2>/dev/null || :
		done
		say "  quarantine cleared"
	fi

	for m in "$MAN"/*.1 "$MAN"/*.7 "$MAN"/*.8; do
		[ -f "$m" ] || continue
		case $m in
		*.1) run cp "$m" "$PREFIX/share/man/man1/" ;;
		*.7) run cp "$m" "$PREFIX/share/man/man7/" ;;
		*.8) run cp "$m" "$PREFIX/share/man/man8/" ;;
		esac
	done
	say "  man pages"

	case ":$PATH:" in
	*":$PREFIX/bin:"*)	;;
	*)	say ""
		say "  $PREFIX/bin is not on your PATH. Add this to your shell:"
		say "    export PATH=\"\$PATH:$PREFIX/bin\"" ;;
	esac
}

################################################################
# The proxy, into a bottle.
################################################################

find_dll()
{
	[ -n "$DLL" ] && { [ -f "$DLL" ] || die "no such file: $DLL"; return; }

	for c in "$here/t150-dinput8.dll" "$SRC/t150-dinput8.dll" \
	    "$here/build/bin/t150-dinput8.dll"; do
		if [ -f "$c" ]; then
			DLL=$c
			return
		fi
	done

	warn "t150-dinput8.dll not found beside this script"
	warn "it is in the windows zip on the releases page; pass it with -d"
	die "or build it with: make dll"
}

# Bottles are directories with a cxbottle.conf in them.
choose_bottle()
{
	[ -d "$BOTTLE_ROOT" ] || die "no bottles directory at $BOTTLE_ROOT"

	set --
	for d in "$BOTTLE_ROOT"/*; do
		[ -f "$d/cxbottle.conf" ] || continue
		set -- "$@" "${d##*/}"
	done
	[ $# -gt 0 ] || die "no bottles found under $BOTTLE_ROOT"

	if [ -n "$BOTTLE" ]; then
		for b in "$@"; do
			[ "$b" = "$BOTTLE" ] && return
		done
		die "no bottle named '$BOTTLE' under $BOTTLE_ROOT"
	fi

	if [ $# -eq 1 ]; then
		BOTTLE=$1
		say "  one bottle found: $BOTTLE"
		return
	fi

	say ""
	say "Which bottle is the game in?"
	i=0
	for b in "$@"; do
		i=$((i + 1))
		printf '  %d) %s\n' "$i" "$b"
	done
	printf 'bottle [1-%d]: ' "$i"

	if ! read -r reply; then
		die "no answer and no -b given, stopping"
	fi
	case $reply in
	''|*[!0-9]*)	die "not a number: $reply" ;;
	esac
	[ "$reply" -ge 1 ] && [ "$reply" -le $# ] || die "out of range: $reply"

	i=0
	for b in "$@"; do
		i=$((i + 1))
		[ "$i" -eq "$reply" ] && BOTTLE=$b
	done
}

# CrossOver can move its builtins with DllPath in cxbottle.conf, so look
# rather than assume.
find_builtin()
{
	conf=$1/cxbottle.conf
	dllpath=$(sed -n 's/^"*DllPath"* *= *"*\([^"]*\)"*.*/\1/p' "$conf" \
	    2>/dev/null | head -1)

	for c in "$dllpath/dinput8.dll" \
	    "$CX_ROOT/lib/wine/x86_64-windows/dinput8.dll"; do
		[ -n "$c" ] || continue
		if [ -f "$c" ] && is_wine_builtin "$c"; then
			printf '%s\n' "$c"
			return 0
		fi
	done

	found=$(find "$CX_ROOT" -name dinput8.dll 2>/dev/null | while read -r f; do
		is_wine_builtin "$f" && { printf '%s\n' "$f"; break; }
	done)
	[ -n "$found" ] || return 1
	printf '%s\n' "$found"
}

# "SDL_JOYSTICK_HIDAPI" = "0" in [EnvironmentVariables], or the wheel never
# appears inside the bottle at all.
set_bottle_env()
{
	conf=$1

	if grep -q 'SDL_JOYSTICK_HIDAPI' "$conf" 2>/dev/null; then
		say "  SDL_JOYSTICK_HIDAPI already set, left alone"
		return
	fi

	if [ "$DRYRUN" -eq 1 ]; then
		say "  would add SDL_JOYSTICK_HIDAPI=0 to $conf"
		return
	fi

	cp "$conf" "$conf.crossover-wheel.bak"
	awk '
		/^\[EnvironmentVariables\]/ {
			print; print "\"SDL_JOYSTICK_HIDAPI\" = \"0\""
			done = 1; next
		}
		{ print }
		END {
			if (!done) {
				print ""
				print "[EnvironmentVariables]"
				print "\"SDL_JOYSTICK_HIDAPI\" = \"0\""
			}
		}
	' "$conf.crossover-wheel.bak" > "$conf"
	say "  SDL_JOYSTICK_HIDAPI=0 added (old file kept as .crossover-wheel.bak)"
}

install_proxy()
{
	step "the proxy into a bottle"

	[ -d "$CX_ROOT" ] || die "CrossOver not found at $CX_ROOT, set CX_ROOT"
	find_dll
	is_our_proxy "$DLL" || die "$DLL does not look like the t150 proxy"
	choose_bottle

	bdir=$BOTTLE_ROOT/$BOTTLE
	sys32=$bdir/drive_c/windows/system32
	[ -d "$sys32" ] ||
	    die "no system32 in bottle '$BOTTLE', is it a 64-bit bottle?"

	if ! builtin=$(find_builtin "$bdir"); then
		warn "cannot find CrossOver's builtin dinput8.dll under"
		warn "  $CX_ROOT"
		app_management_hint
		die "nothing was changed in the bottle"
	fi
	say "  builtin: $builtin"

	# The one mistake worth refusing outright. Copying the proxy, or the
	# placeholder already in system32, as dinput8_orig.dll gives a chain
	# that loads and then does nothing.
	is_our_proxy "$builtin" &&
	    die "that is the proxy, not CrossOver's builtin. Refusing."

	if [ -f "$sys32/dinput8.dll" ] && ! is_our_proxy "$sys32/dinput8.dll" &&
	    ! is_wine_builtin "$sys32/dinput8.dll" &&
	    [ ! -f "$sys32/dinput8.dll.crossover-wheel.bak" ]; then
		run cp "$sys32/dinput8.dll" \
		    "$sys32/dinput8.dll.crossover-wheel.bak"
		say "  kept the old dinput8.dll as .crossover-wheel.bak"
	fi

	# Look at dinput8_orig.dll before overwriting it, because it is not
	# necessarily ours. Three things it can be, and only one of them is
	# safe to replace without saying anything:
	#
	#   a Wine builtin   ours, or the same file from an older CrossOver.
	#                    Replacing it is the documented thing to do after a
	#                    CrossOver upgrade, since it stays at the version it
	#                    was copied from.
	#   our proxy        somebody, possibly an earlier run of this, copied
	#                    the wrong file here. That is the exact mistake this
	#                    installer exists to prevent, and replacing it with
	#                    the real builtin is the repair.
	#   anything else    another dinput8 wrapper chain-loads through this
	#                    name too. Overwriting it silently would break that
	#                    tool and lose a file nobody else has a copy of.
	if [ -f "$sys32/dinput8_orig.dll" ]; then
		if is_our_proxy "$sys32/dinput8_orig.dll"; then
			say "  dinput8_orig.dll was the proxy, not the builtin:"
			say "  replacing it with the real one, which repairs it"
		elif is_wine_builtin "$sys32/dinput8_orig.dll"; then
			say "  dinput8_orig.dll was already a builtin, refreshing"
		elif [ -f "$sys32/dinput8_orig.dll.crossover-wheel.bak" ]; then
			warn "dinput8_orig.dll is not a Wine builtin and a"
			warn "backup of an earlier one already exists."
			die "move $sys32/dinput8_orig.dll aside yourself first"
		else
			run cp "$sys32/dinput8_orig.dll" \
			    "$sys32/dinput8_orig.dll.crossover-wheel.bak"
			say "  dinput8_orig.dll was something else, and is kept"
			say "  as .crossover-wheel.bak. If another tool put it"
			say "  there, it chain-loads through this name too and"
			say "  the two cannot both use it."
		fi
	fi

	# Reading this file means reaching inside CrossOver.app, and macOS
	# gates that: an application which touches another application's
	# bundle is refused until it is allowed under App Management, and the
	# refusal arrives as an ordinary permission error. Saying so here is
	# the difference between a person granting one permission and a person
	# concluding the installer is broken.
	if [ "$DRYRUN" -eq 0 ] &&
	    ! cp "$builtin" "$sys32/dinput8_orig.dll" 2>/dev/null; then
		warn "could not read CrossOver's builtin:"
		warn "  $builtin"
		app_management_hint
		die "nothing was changed in the bottle"
	fi
	[ "$DRYRUN" -eq 1 ] && run cp "$builtin" "$sys32/dinput8_orig.dll"
	say "  dinput8_orig.dll  <- CrossOver's builtin"
	run cp "$DLL" "$sys32/dinput8.dll"
	say "  dinput8.dll       <- the proxy"

	if [ "$DRYRUN" -eq 0 ]; then
		is_wine_builtin "$sys32/dinput8_orig.dll" ||
		    die "dinput8_orig.dll is not the builtin after copying"
		is_our_proxy "$sys32/dinput8.dll" ||
		    die "dinput8.dll is not the proxy after copying"
		say "  both files verified"
	fi

	step "the registry override"
	if [ -x "$CX_ROOT/bin/wine" ]; then
		run "$CX_ROOT/bin/wine" --bottle "$BOTTLE" --cx-app reg.exe add \
		    'HKCU\Software\Wine\DllOverrides' /v dinput8 /t REG_SZ \
		    /d native,builtin /f
		say "  dinput8 = native,builtin"
	else
		warn "no wine at $CX_ROOT/bin/wine, set the override by hand:"
		warn "  wine --bottle '$BOTTLE' --cx-app reg.exe add \\"
		warn "    'HKCU\\Software\\Wine\\DllOverrides' /v dinput8 \\"
		warn "    /t REG_SZ /d native,builtin /f"
	fi

	step "the bottle's environment"
	set_bottle_env "$bdir/cxbottle.conf"
}

################################################################

################################################################
# The application, when there is one beside us.
################################################################

# A downloaded app carries com.apple.quarantine, and macOS tells the person
# it is damaged and offers to move it to the bin: that message means the
# bundle is quarantined and not signed by a known developer, not that
# anything is wrong with it, and right-click Open does not get past it.
# Clearing the attribute is what does. The app cannot do this for itself,
# which is why it is worth doing from here.
install_app()
{
	src=$here/crossover-wheel.app
	dst=$HOME/Applications/crossover-wheel.app

	[ -d "$src" ] || return 0

	step "the application into ~/Applications"
	run mkdir -p "$HOME/Applications"
	run rm -rf "$dst"
	run cp -R "$src" "$dst"

	if command -v xattr >/dev/null 2>&1; then
		run xattr -dr com.apple.quarantine "$dst" 2>/dev/null || :
		say "  quarantine cleared, it will open by double-click"
	fi
	say "  $dst"
}

[ "$WANT_BINARIES" -eq 1 ] && install_binaries
[ "$WANT_APP" -eq 1 ] && install_app
[ "$WANT_BOTTLE" -eq 1 ] && install_proxy

step "done"

# Name them by path only if this run is what put them there.
if [ "$WANT_BINARIES" -eq 1 ]; then
	boot=$PREFIX/bin/t150boot
	daemon=$PREFIX/bin/t150d
else
	boot=t150boot
	daemon=t150d
fi

cat <<EOF

Start the daemon before the game, and leave it running. The proxy looks for
it once, when the game creates its wheel, and never again.

  $boot          # after every plug-in, sleep and wake
  $daemon -v -w       # then start the game

Set the game's own steering rotation to 1080 degrees, which is what this
wheel is. A game left at its default of 900 steers by 900/1080 of what it
means to, because nothing can tell a wheel what range to be at.

Sit at the machine: writing to the wheel is gated on being the console user
and fails over SSH.
EOF
