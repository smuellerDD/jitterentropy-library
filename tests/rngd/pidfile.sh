#!/bin/sh
# rngd-pidfile: the PID file of jitterentropy-rngd on FreeBSD, NetBSD, OpenBSD
# and macOS. --device on a regular file stands in for /dev/random, so that it
# needs neither root nor the kernel.
#
#  - the detached daemon writes its PID, and a second instance naming the
#    same file refuses to start;
#  - a stale PID file of the daemon's own user is taken over;
#  - a relative --pid and --device mean the directory the daemon started in;
#  - a hard link, a symbolic link or a FIFO in its place ends the start with
#    1 and leaves the file behind it as it was.
#
# Linux needs root and the kernel to start, which writes the file after the
# first injection: its daemon is not tested here.
#
#   sh pidfile.sh <jitterentropy-rngd> <scratch directory, absolute>
#
# POSIX sh and utilities only, and no function named after a default alias
# of a ksh, such as stop.

set -u

rngd=$1
dir=$2
pidfile=$dir/pid
victim=$dir/victim
log=$dir/log
running=

rm -rf "$dir"
mkdir -p "$dir" || exit 1
: > "$dir/seed"

fail()
{
	echo "FAIL: $*"
	echo "--- log of the daemon"
	cat "$log"
	[ -n "$running" ] && kill -s KILL "$running" 2>/dev/null
	exit 1
}

# The daemon detached with its PID file at @1: the exit status of its start
launch()
{
	"$rngd" -p "$1" -t 600 -d "$dir/seed" >> "$log" 2>&1
}

# SIGTERM to the daemon of PID @1, which is not our child: wait for it to go
finish()
{
	kill -s TERM "$1" || fail "daemon $1 was gone"
	tries=300
	while kill -0 "$1" 2>/dev/null; do
		tries=$((tries - 1))
		[ "$tries" -gt 0 ] || fail "daemon $1 did not end"
		sleep 0.1
	done
}

# @1 in place of the PID file: the start refused with @2 logged, and $victim
# as it was
refused()
{
	: > "$log"
	if launch "$pidfile"; then
		running=$(cat "$pidfile" 2>/dev/null)
		fail "the daemon started over $1"
	fi
	grep -q -F -- "$2" "$log" || fail "$1: not refused for \"$2\""
	[ "$(cat "$victim")" = victim ] || fail "$1: the file behind it was written"
}

# The startup of the Jitter RNG is slow in an emulated guest: no limits on it.
echo "== the detached daemon writes its PID, a second instance refuses"
launch "$pidfile" || fail "the start failed with $?"
running=$(cat "$pidfile")
kill -0 "$running" 2>/dev/null || fail "no daemon of PID $running"
launch "$pidfile" && fail "a second instance started"
[ "$(cat "$pidfile")" = "$running" ] ||
	fail "the second instance changed the PID file"
finish "$running"
running=

echo "== a stale PID file of the daemon's user is taken over"
rm -f "$pidfile"
echo 99999 > "$pidfile"
launch "$pidfile" || fail "the start over a stale PID file failed with $?"
running=$(cat "$pidfile")
[ "$running" != 99999 ] || fail "the stale PID file was kept"
finish "$running"
running=

printf 'victim\n' > "$victim"

echo "== a relative path means the directory the daemon started in"
rm -f "$pidfile"
(cd "$dir" && "$rngd" -p pid -t 600 -d seed >> "$log" 2>&1) ||
	fail "the start with relative paths failed with $?"
running=$(cat "$pidfile")
kill -0 "$running" 2>/dev/null || fail "no daemon of PID $running"
finish "$running"
running=

echo "== a hard link in place of the PID file"
rm -f "$pidfile"
ln "$victim" "$pidfile"
refused "a hard link" "is not a regular file of user"

echo "== a symbolic link in place of the PID file"
rm -f "$pidfile"
ln -s "$victim" "$pidfile"
refused "a symbolic link" "Cannot open pid file"

echo "== a FIFO in place of the PID file"
rm -f "$pidfile"
mkfifo "$pidfile"
refused "a FIFO" "is not a regular file of user"

echo "PASS"
