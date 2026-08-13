#!/bin/sh
#
# update.sh - swap a new copy of the application in for the running one.
#
# Run by the application as its last act, with its own process id, so this
# waits for it to be gone before touching anything. An application cannot
# replace itself while it is running, but it can ask a shell to do it after.
#
# The old bundle is moved aside rather than deleted, and only removed once the
# new one is in place. If the move in fails, the old one goes back. There is
# no window in which the machine has neither: the failure mode of a self
# updater has to be "nothing happened", never "the application is gone".
#
# Usage: update.sh <new-app> <target-app> <pid>
#
# Copyright (c) 2026 Renaud Allard
# SPDX-License-Identifier: BSD-2-Clause

set -eu

[ $# -eq 3 ] || { echo "usage: $0 <new-app> <target-app> <pid>" >&2; exit 2; }

new=$1
target=$2
pid=$3
old=$target.crossover-wheel-previous

[ -d "$new" ] || { echo "no new application at $new" >&2; exit 1; }

# Wait for it to quit, but not forever: something has gone wrong if it has not
# gone in twenty seconds, and swapping a bundle under a running process is how
# you get a half-updated application.
i=0
while kill -0 "$pid" 2>/dev/null; do
	i=$((i + 1))
	if [ "$i" -gt 100 ]; then
		echo "it is still running after 20s, leaving it alone" >&2
		exit 1
	fi
	sleep 0.2
done

rm -rf "$old"

if [ -d "$target" ] && ! mv "$target" "$old"; then
	echo "cannot move the old application aside" >&2
	exit 1
fi

if ! mv "$new" "$target"; then
	# Put it back. Better the version they had than none at all.
	[ -d "$old" ] && mv "$old" "$target"
	echo "cannot move the new application in, the old one is back" >&2
	exit 1
fi

rm -rf "$old"

# Nothing downloaded by the application carries the quarantine flag a browser
# sets, so this one opens without any of the dialogs the first install needed.
command -v open >/dev/null 2>&1 && open "$target"

exit 0
