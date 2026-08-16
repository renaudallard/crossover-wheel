#!/bin/sh
#
# update_check.sh - drive dist/update.sh against synthetic application bundles.
#
# update.sh is the only code in this project whose failure mode is that the
# person has no application at all. It runs unattended on every user's machine,
# after the application has already quit, with nothing watching stderr, which
# is the reason its own comment gives for every exit having to end with
# something back on screen. It has already had one fault of exactly that shape,
# and a regression of it is invisible: the swap still works, and only the
# person whose swap failed ever finds out.
#
# Nothing here needs a Mac. The bundles are directories, told apart by a file
# inside them, and "open" is a script on PATH that records what it was asked to
# open, which is the only externally visible thing update.sh does on its way
# out.
#
# Copyright (c) 2026 Renaud Allard
# SPDX-License-Identifier: BSD-2-Clause

set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$root/tmp/update_check.$$
script=$root/dist/update.sh
failures=0

trap 'chmod u+w "$work"/*/ 2>/dev/null; rm -rf "$work"' EXIT INT TERM

fail()
{
	printf 'FAIL %s\n' "$*" >&2
	failures=$((failures + 1))
}

# Two cases below force a failed move in by taking write permission off a
# directory, and root ignores directory write permission, so there they would
# report a failure of update.sh where the only thing that failed is the way
# the case sets itself up. Skipped out loud rather than left to pass or fail
# by accident: a build as root is an ordinary thing in a container.
as_root=0
[ "$(id -u)" = 0 ] && as_root=1

skip()
{
	printf 'SKIP %s\n' "$*" >&2
}

# A bundle is a directory with a name inside it, which is how a swap that put
# the wrong one in place is told from one that worked.
make_bundle()
{
	mkdir -p "$1/Contents/MacOS"
	printf '%s\n' "$2" > "$1/Contents/version"
}

bundle_is()
{
	[ -f "$1/Contents/version" ] &&
	    [ "$(cat "$1/Contents/version")" = "$2" ]
}

# A fresh tree, and an "open" that records rather than opening. update.sh
# resolves it with command -v, so putting it first on PATH is enough.
build_tree()
{
	rm -rf "$work"
	mkdir -p "$work/bin" "$work/staged" "$work/apps"
	cat > "$work/bin/open" <<'EOF'
#!/bin/sh
printf '%s\n' "$1" >> "$OPEN_LOG"
EOF
	chmod +x "$work/bin/open"
	: > "$work/open.log"
	: > "$work/err.log"
	make_bundle "$work/apps/crossover-wheel.app" old
	make_bundle "$work/staged/crossover-wheel.app" new
}

run_update()
{
	PATH="$work/bin:$PATH" OPEN_LOG="$work/open.log" \
	    sh "$script" "$@" 2>>"$work/err.log"
}

# A process that has already gone, which is what the application is by the time
# this runs. $$ of a subshell that has exited is not reliably free, so a real
# one is started and waited for.
dead_pid()
{
	sh -c 'exit 0' &
	p=$!
	wait "$p" 2>/dev/null
	printf '%s\n' "$p"
}

test_a_swap_that_works()
{
	build_tree
	run_update "$work/staged/crossover-wheel.app" \
	    "$work/apps/crossover-wheel.app" "$(dead_pid)" ||
	    fail "a swap that should work reported failure"

	bundle_is "$work/apps/crossover-wheel.app" new ||
	    fail "the new application is not at the target"
	[ ! -e "$work/apps/crossover-wheel.app.crossover-wheel-previous" ] ||
	    fail "the old application was left beside the new one"
	[ ! -e "$work/staged/crossover-wheel.app" ] ||
	    fail "the staged copy was left behind"
	grep -q "^$work/apps/crossover-wheel.app$" "$work/open.log" ||
	    fail "nothing was put back on screen after a successful swap"
}

# The one branch that can lose the application. The move in is made to fail by
# taking write permission off the staged copy's own directory, which is what
# rename needs to remove the source entry: the target's directory stays
# writable, so the move aside before it still succeeds and the failure lands
# exactly where it is wanted.
test_a_failed_move_in_puts_the_old_one_back()
{
	if [ "$as_root" = 1 ]; then
		skip "a failed move in: root ignores the write permission"
		return
	fi

	build_tree
	chmod a-w "$work/staged"

	run_update "$work/staged/crossover-wheel.app" \
	    "$work/apps/crossover-wheel.app" "$(dead_pid)" &&
	    fail "a swap that could not move the new one in reported success"

	chmod u+w "$work/staged"

	bundle_is "$work/apps/crossover-wheel.app" old ||
	    fail "the old application did not come back to the target"
	[ ! -e "$work/apps/crossover-wheel.app.crossover-wheel-previous" ] ||
	    fail "the old application was left aside rather than put back"
	grep -q "^$work/apps/crossover-wheel.app$" "$work/open.log" ||
	    fail "nothing was put back on screen after a failed swap"
}

# A previous run that died between moving the old one aside and removing it
# leaves that copy behind. It has to be cleared rather than block the swap.
test_a_stale_previous_copy_does_not_block_the_swap()
{
	build_tree
	make_bundle "$work/apps/crossover-wheel.app.crossover-wheel-previous" \
	    stale

	run_update "$work/staged/crossover-wheel.app" \
	    "$work/apps/crossover-wheel.app" "$(dead_pid)" ||
	    fail "a stale previous copy stopped a swap that should work"

	bundle_is "$work/apps/crossover-wheel.app" new ||
	    fail "the new application is not at the target after a stale copy"
	[ ! -e "$work/apps/crossover-wheel.app.crossover-wheel-previous" ] ||
	    fail "the stale copy survived the swap"
}

# The two together, which is the combination that can nest the application one
# directory deeper than its own path and so lose it.
#
# A stale copy left by an earlier run is a directory, and mv moves a source
# into a directory that already exists rather than renaming onto it. So a swap
# that does not clear the stale copy first puts the old bundle at
# $old/crossover-wheel.app, and the recovery that follows a failed move in
# brings that back to $target, leaving the application at
# $target/crossover-wheel.app where nothing looks for it.
test_a_stale_copy_and_a_failed_move_in_still_restore_the_application()
{
	if [ "$as_root" = 1 ]; then
		skip "a stale copy and a failed move in: root ignores it too"
		return
	fi

	build_tree
	make_bundle "$work/apps/crossover-wheel.app.crossover-wheel-previous" \
	    stale
	chmod a-w "$work/staged"

	run_update "$work/staged/crossover-wheel.app" \
	    "$work/apps/crossover-wheel.app" "$(dead_pid)" &&
	    fail "a swap that could not move the new one in reported success"

	chmod u+w "$work/staged"

	bundle_is "$work/apps/crossover-wheel.app" old ||
	    fail "the old application is not back at its own path"
	[ ! -e "$work/apps/crossover-wheel.app/crossover-wheel.app" ] ||
	    fail "the old application came back nested inside its own path"
}

# Nothing to install is not a reason to touch what is there.
test_a_missing_new_application_changes_nothing()
{
	build_tree
	rm -rf "$work/staged/crossover-wheel.app"

	run_update "$work/staged/crossover-wheel.app" \
	    "$work/apps/crossover-wheel.app" "$(dead_pid)" &&
	    fail "a missing new application reported success"

	bundle_is "$work/apps/crossover-wheel.app" old ||
	    fail "a missing new application disturbed the target"
	[ ! -s "$work/open.log" ] ||
	    fail "a missing new application still moved the target aside"
}

test_the_arguments_are_checked()
{
	build_tree
	if run_update "$work/staged/crossover-wheel.app"; then
		fail "one argument was accepted"
	fi

	# set -eu ends the script on an unset $2 whether or not the guard is
	# there, so a non-zero exit proves nothing about the guard. Only its
	# own message does.
	grep -q "^usage: " "$work/err.log" ||
	    fail "one argument was refused by set -u rather than by the check"
}

# It must not swap while the application it is replacing is still running: a
# bundle moved under a live process is how a half updated application happens.
test_it_waits_for_the_process_to_go()
{
	build_tree
	sleep 2 &
	live=$!

	run_update "$work/staged/crossover-wheel.app" \
	    "$work/apps/crossover-wheel.app" "$live" &
	updater=$!

	sleep 1
	bundle_is "$work/apps/crossover-wheel.app" old ||
	    fail "the swap happened while the application was still running"

	wait "$updater" 2>/dev/null
	bundle_is "$work/apps/crossover-wheel.app" new ||
	    fail "the swap did not happen once the application had gone"
}

test_a_swap_that_works
test_a_failed_move_in_puts_the_old_one_back
test_a_stale_previous_copy_does_not_block_the_swap
test_a_stale_copy_and_a_failed_move_in_still_restore_the_application
test_a_missing_new_application_changes_nothing
test_the_arguments_are_checked
test_it_waits_for_the_process_to_go

if [ "$failures" -ne 0 ]; then
	printf 'update_check: %d failure(s)\n' "$failures" >&2
	exit 1
fi

printf 'update_check: ok\n'
