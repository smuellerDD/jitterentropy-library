#!/bin/sh
# rngd-signals: SIGUSR1 and SIGUSR2 of jitterentropy-rngd on FreeBSD, NetBSD,
# OpenBSD and macOS. --device on a regular file stands in for /dev/random, so
# that it needs neither root nor the kernel; run as root, it switches to
# nobody and enters Capsicum or pledge(2) as it does in service. Linux has no
# --device: the nix VM tests check its daemon.
#
#   sh signals.sh <jitterentropy-rngd> <scratch directory>
#
# POSIX sh and utilities only: no seq, no stat, whose options differ.

set -u

rngd=$1
dir=$2
seed=$dir/seed
log=$dir/log
pid=

rm -rf "$dir"
mkdir -p "$dir" || exit 1

fail()
{
	echo "FAIL: $*"
	echo "--- log of the daemon"
	cat "$log"
	[ -n "$pid" ] && kill -s KILL "$pid" 2>/dev/null
	exit 1
}

# Bytes in the seed file
size()
{
	wc -c < "$seed" | tr -d ' '
}

# Lines of the log holding @1
logged()
{
	grep -c -F -- "$1" "$log"
}

has_size()
{
	[ "$(size)" -ge "$1" ]
}

has_logged()
{
	[ "$(logged "$1")" -ge "$2" ]
}

# wait_for SECONDS COMMAND...: COMMAND every 0.1 s until it succeeds
wait_for()
{
	tries=$(($1 * 10))
	shift
	while ! "$@"; do
		tries=$((tries - 1))
		[ "$tries" -gt 0 ] || return 1
		sleep 0.1
	done
}

# The daemon in the foreground with options @..., seeding $seed
start()
{
	: > "$seed"
	"$rngd" -F -d "$seed" "$@" > "$log" 2>&1 &
	pid=$!
}

signal()
{
	kill -s "$1" "$pid" || fail "the daemon was gone before $1"
}

# Not stop(): OpenBSD's sh, a ksh, has stop as an alias of kill -STOP, which
# it expands in the definition.
terminate()
{
	signal TERM
	wait "$pid"
	ret=$?
	pid=
	[ "$ret" -eq 0 ] || fail "SIGTERM ended the daemon with $ret"
}

# The startup of the Jitter RNG is slow in an emulated guest: generous limits.
echo "== SIGUSR1 and SIGUSR2 at the default verbosity"
start -t 600
wait_for 120 has_size 64 || fail "no first seed"
signal USR1
wait_for 60 has_logged "Notice] Status of the Jitter RNG on SIGUSR1:" 1 ||
	fail "no status on SIGUSR1"
signal USR2
wait_for 60 has_logged "Seeded $seed with 64 bytes, as SIGUSR2 asked" 1 ||
	fail "no seed on SIGUSR2"
[ "$(size)" -eq 128 ] || fail "$(size) bytes after the seed of SIGUSR2, not 128"
terminate

echo "== SIGUSR1 and SIGUSR2 during the startup"
start -t 600 -vv
# Logged after rngd_os_start() blocked them, before the daemon waits
wait_for 120 has_logged "Status of the Jitter RNG after its initialization" 1 ||
	fail "no initialization"
signal USR1
signal USR2
wait_for 60 has_logged "Status of the Jitter RNG on SIGUSR1" 1 ||
	fail "no status on SIGUSR1 sent during the startup"
wait_for 60 has_logged "as SIGUSR2 asked" 1 ||
	fail "no seed on SIGUSR2 sent during the startup"
terminate

echo "== a SIGUSR1 every second does not put off a 2 s interval"
start -t 2 -vv
wait_for 120 has_size 64 || fail "no first seed"
# Each SIGUSR1 ends the wait: one of 2 s every time would never end.
sent=0
while [ "$(logged "bytes written to")" -lt 3 ]; do
	[ "$sent" -lt 30 ] ||
		fail "$(logged "bytes written to") seeds of the interval in 30 s"
	sleep 1
	signal USR1
	sent=$((sent + 1))
done
# Each reported - two make one only where a seed kept the daemon busy
wait_for 30 has_logged "on SIGUSR1" $((sent - 1)) ||
	fail "$(logged "on SIGUSR1") of $sent SIGUSR1 reported"
echo "3 seeds of the interval with $sent SIGUSR1 sent"
terminate

echo "PASS"
