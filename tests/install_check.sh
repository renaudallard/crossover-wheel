#!/bin/sh
#
# install_check.sh - drive install.sh against a synthetic CrossOver tree.
#
# install.sh is the only part of this project that writes into somebody's
# bottle, and it is the part the application calls rather than reimplements,
# so it carries the risk. Until this existed the only thing any build checked
# was that "install.sh -h" prints its usage, and a bottle chooser that aborted
# the whole install unless you picked the last bottle in the list shipped
# behind that.
#
# Nothing here needs a Mac, CrossOver or a wheel: PREFIX, CX_ROOT and
# BOTTLE_ROOT are the three paths install.sh takes from the environment, which
# is what its own usage text says they are for.
#
# Copyright (c) 2026 Renaud Allard
# SPDX-License-Identifier: BSD-2-Clause

set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$root/tmp/install_check.$$
failures=0

trap 'rm -rf "$work"' EXIT INT TERM

fail()
{
	printf 'FAIL %s\n' "$*" >&2
	failures=$((failures + 1))
}

# A Wine builtin says so in its DOS stub; the proxy is a PE carrying the
# marker install.sh looks for. Neither has to be a real PE, only to answer the
# two questions install.sh asks of them.
make_builtin()
{
	{ printf 'MZ'; printf '\0%.0s' $(seq 1 62); \
	  printf 'This is a Wine builtin DLL, and not a real one.\n'; } > "$1"
}

make_proxy()
{
	{ printf 'MZ'; printf '\0%.0s' $(seq 1 62); \
	  printf 'not a builtin. T150_ENDPOINT\n'; } > "$1"
}

# One CrossOver install and as many bottles as asked for, named in order.
build_tree()
{
	rm -rf "$work"
	mkdir -p "$work/cx/lib/wine/x86_64-windows" "$work/bottles" "$work/src"
	make_builtin "$work/cx/lib/wine/x86_64-windows/dinput8.dll"
	make_proxy "$work/src/t150-dinput8.dll"

	for b in "$@"; do
		mkdir -p "$work/bottles/$b/drive_c/windows/system32"
		printf '[Bottle]\n"Template" = "win10_64"\n' \
		    > "$work/bottles/$b/cxbottle.conf"
	done
}

# Run install.sh with the synthetic tree, feeding it $1 on stdin.
run_install()
{
	reply=$1
	shift

	printf '%s\n' "$reply" | PREFIX=$work/prefix CX_ROOT=$work/cx \
	    BOTTLE_ROOT=$work/bottles \
	    sh "$root/install.sh" -d "$work/src/t150-dinput8.dll" "$@" \
	    > "$work/out" 2>&1
}

installed_in()
{
	sys32=$work/bottles/$1/drive_c/windows/system32

	[ -f "$sys32/dinput8.dll" ] && [ -f "$sys32/dinput8_orig.dll" ]
}

# The regression this file was written for. Choosing anything but the last
# bottle used to leave choose_bottle returning the status of a failed test,
# which set -e turned into a silent exit before a single file was copied.
test_every_bottle_in_the_list_can_be_chosen()
{
	n=1
	for want in alpha beta gamma; do
		build_tree alpha beta gamma
		run_install "$n" --no-binaries --no-app
		status=$?
		if [ "$status" -ne 0 ]; then
			fail "choosing bottle $n ($want) exited $status"
			sed 's/^/    /' "$work/out" >&2
		elif ! installed_in "$want"; then
			fail "chose bottle $n but nothing was put in '$want'"
		fi
		n=$((n + 1))
	done
}

# The proxy must go where it was asked and nowhere else.
test_the_choice_installs_into_that_bottle_only()
{
	build_tree alpha beta gamma
	run_install 2 --no-binaries --no-app || :

	installed_in beta || fail "the chosen bottle got nothing"
	if installed_in alpha || installed_in gamma; then
		fail "a bottle nobody chose was written to"
	fi
}

# -b names a bottle instead of being asked, which is how the application
# calls this. Every name in the list has to work, not just the last.
test_naming_a_bottle_works_for_every_name()
{
	for want in alpha beta gamma; do
		build_tree alpha beta gamma
		run_install "" -b "$want" --no-binaries --no-app
		status=$?
		if [ "$status" -ne 0 ]; then
			fail "-b $want exited $status"
			sed 's/^/    /' "$work/out" >&2
		elif ! installed_in "$want"; then
			fail "-b $want put nothing in '$want'"
		fi
	done
}

test_one_bottle_needs_no_question()
{
	build_tree solo

	# Nothing on stdin: with a single bottle it must not ask.
	if ! run_install "" --no-binaries --no-app; then
		fail "a single bottle still asked, or failed"
		sed 's/^/    /' "$work/out" >&2
	elif ! installed_in solo; then
		fail "the only bottle got nothing"
	fi
}

test_bad_answers_are_refused()
{
	build_tree alpha beta

	run_install 9 --no-binaries --no-app &&
	    fail "an out of range answer was accepted"
	grep -q 'out of range' "$work/out" ||
	    fail "an out of range answer did not say so"

	build_tree alpha beta
	run_install nonsense --no-binaries --no-app &&
	    fail "a non-numeric answer was accepted"
	grep -q 'not a number' "$work/out" ||
	    fail "a non-numeric answer did not say so"

	build_tree alpha beta
	run_install 1 --no-binaries --no-app -b nosuch &&
	    fail "a bottle that does not exist was accepted"
	grep -q "no bottle named" "$work/out" ||
	    fail "a missing bottle did not say so"
}

# The builtin is what the proxy chain-loads, and copying the wrong file here
# is the mistake the whole script exists to prevent.
test_the_builtin_is_what_lands_beside_the_proxy()
{
	build_tree alpha
	run_install "" --no-binaries --no-app || :

	sys32=$work/bottles/alpha/drive_c/windows/system32
	head -c 128 "$sys32/dinput8_orig.dll" 2>/dev/null |
	    LC_ALL=C grep -aq 'Wine builtin DLL' ||
	    fail "dinput8_orig.dll is not CrossOver's builtin"
	LC_ALL=C grep -aq 'T150_ENDPOINT' "$sys32/dinput8.dll" 2>/dev/null ||
	    fail "dinput8.dll is not the proxy"
}

# The wheel does not appear inside a bottle unless this is 0, so the check
# has to be about the value. Matching the name alone reported success for a
# line saying the opposite, and for the PS3 and PS4 suffixed variables.
test_the_hidapi_setting_is_matched_by_value()
{
	conf=$work/bottles/alpha/cxbottle.conf

	build_tree alpha
	run_install "" --no-binaries --no-app || :
	grep -q '"SDL_JOYSTICK_HIDAPI" = "0"' "$conf" ||
	    fail "the variable was not added to a bottle without it"

	# Already correct: left alone, and said to be.
	build_tree alpha
	printf '[EnvironmentVariables]\n"SDL_JOYSTICK_HIDAPI" = "0"\n' \
	    >> "$work/bottles/alpha/cxbottle.conf"
	run_install "" --no-binaries --no-app || :
	grep -q 'already 0' "$work/out" ||
	    fail "a bottle that already has it was not recognised"

	# A different variable whose name starts the same must not count.
	build_tree alpha
	printf '[EnvironmentVariables]\n"SDL_JOYSTICK_HIDAPI_PS3" = "0"\n' \
	    >> "$work/bottles/alpha/cxbottle.conf"
	run_install "" --no-binaries --no-app || :
	grep -q '"SDL_JOYSTICK_HIDAPI" = "0"' "$conf" ||
	    fail "the PS3 variable was mistaken for this one"

	# Present with the wrong value: say so rather than claim success.
	build_tree alpha
	printf '[EnvironmentVariables]\n"SDL_JOYSTICK_HIDAPI" = "1"\n' \
	    >> "$work/bottles/alpha/cxbottle.conf"
	run_install "" --no-binaries --no-app || :
	grep -q 'something other than 0' "$work/out" ||
	    fail "a wrong value was not reported"
}

# Another tool's dinput8.dll is about to be overwritten, and the backup is the
# only copy of it. Skipping the backup because a stale one existed destroyed it
# on the second run.
test_a_third_party_wrapper_is_never_lost()
{
	sys32=$work/bottles/alpha/drive_c/windows/system32

	build_tree alpha
	printf 'MZ some other dinput8 wrapper\n' > "$sys32/dinput8.dll"
	run_install "" --no-binaries --no-app || :
	grep -aq 'some other dinput8 wrapper' \
	    "$sys32/dinput8.dll.crossover-wheel.bak" ||
	    fail "the wrapper was not kept on the first run"

	# A second wrapper appears, with the first run's backup still there.
	printf 'MZ a second wrapper nobody else has\n' > "$sys32/dinput8.dll"
	run_install "" --no-binaries --no-app &&
	    fail "overwriting a wrapper with a stale backup was allowed"
	grep -q 'move ' "$work/out" ||
	    fail "it did not say what to do about it"
	grep -aq 'a second wrapper nobody else has' "$sys32/dinput8.dll" ||
	    fail "the second wrapper was destroyed"
}

# -n has to be honest: it says what it would do and writes nothing.
test_dry_run_changes_nothing()
{
	build_tree alpha
	run_install "" -n --no-binaries --no-app || fail "-n exited nonzero"

	installed_in alpha && fail "-n wrote into the bottle"
	grep -q 'would' "$work/out" || fail "-n said nothing about what it would do"
}

test_every_bottle_in_the_list_can_be_chosen
test_the_choice_installs_into_that_bottle_only
test_naming_a_bottle_works_for_every_name
test_one_bottle_needs_no_question
test_bad_answers_are_refused
test_the_builtin_is_what_lands_beside_the_proxy
test_the_hidapi_setting_is_matched_by_value
test_a_third_party_wrapper_is_never_lost
test_dry_run_changes_nothing

if [ "$failures" -ne 0 ]; then
	printf 'install_check: %d failure(s)\n' "$failures" >&2
	exit 1
fi

printf 'install_check: ok\n'
