#!/usr/bin/env bash
#
# Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
#
# Test for analyzing the boot time entropy by power cycling the test machine
# many times and record the first time stamps.
#
# The test applies to the Jitter RNG of the vanilla kernel (crypto/), whose
# test interface buffers the first entropy events since boot. The out-of-tree
# module in linux_kernel/ has no such boot time buffer; with it, each boot
# records the first time deltas of a raw-noise instance its debugfs file
# allocates on open - see "Out-of-tree module" below.
#
# Test execution:
#	1. Enable kernel option `CONFIG_CRYPTO_JITTERENTROPY_TESTINTERFACE`
#	   (offered with `CONFIG_CRYPTO_FIPS` and `CONFIG_EXPERT`),
#	   enable configuration option `CONFIG_CRYPTO_USER_API_RNG`,
#	   compile, install and reboot the kernel, and ensure that the
#	   Linux kernel command line contains
#	   `jitterentropy_testing.boot_raw_hires_test=1`
#	2. Compile getrawentropy.c for the kernel version as documented in
#	   that file and install into /usr/local/sbin
#	3. Copy this file to /usr/local/sbin and make it executable and do not
#	   forget restorecon if applicable
#	4. Copy boottime_test_record.service to /etc/systemd/system/
#	5. systemctl enable boottime_test_record
#	6. reboot and wait until reboot test completes
#	7. Pick up $OUTFILE and analyze
#
# If you want to restart the test, do:
#	1. Clean out $OUTFILE.*, $STATE and $FAILURES
#	2. start with step 4 from above
# Enabled again without step 1, a complete test records nothing: the service
# disables itself again at the next boot.
#
# Failed runs:
#	A run whose getrawentropy failed, timed out or delivered fewer than
#	its 1001 samples does not count towards the $TESTS runs: its file is
#	renamed to $OUTFILE.<run>.data.failed.<n>, the n-th failure in a row,
#	out of the way of the analysis, and the next boot records that run
#	again. After
#	$MAX_FAILURES failed runs in a row the test stops as if complete, as
#	it does without kcapi-rng, rather than reboot the machine forever.
#
# Settings, each taken from the environment when set there (Environment= or
# EnvironmentFile= of the service), defaulting to the vanilla kernel:
#	GETRAWENTROPY	the recording tool (/usr/local/sbin/getrawentropy)
#	KCAPIRNG	libkcapi's kcapi-rng (/usr/bin/kcapi-rng)
#	KCAPI_NAME	the RNG kcapi-rng drives (jitterentropy_rng)
#	DEBUGFS_FILE	the raw data file
#			(/sys/kernel/debug/jitterentropy_testing/jent_raw_hires)
#	RAW_OPTS	options for getrawentropy (--timestamps, as the vanilla
#			interface delivers time stamps; set, even empty, it
#			replaces them)
#	TESTS		the number of runs (1000)
#
# Out-of-tree module:
#	Load jitter_rng with CONFIG_EXTERNAL_JITTERENTROPY_TESTINTERFACE at boot
#	and set DEBUGFS_FILE=/sys/kernel/debug/jitter_rng/jent_raw_hires,
#	RAW_OPTS= (it delivers time deltas) and KCAPI_NAME=jitter_rng.
#
# Test interruption:
#	Boot with kernel command line option of boottime_test_stop. After this
#	interruption, the next reboot will continue collecting data for this
#	test. The interruption does not affect the test data.
#

# No -e: a failed step is accounted for below, and the script must still
# reach the reboot - stopping short ends the test on a machine nobody
# watches.
set -uxo pipefail

OUTDIR="/root/results-measurements"
OUTFILE="$OUTDIR/jent-raw-noise-restart"
STATE="$OUTDIR/jent_state"
FAILURES="$OUTDIR/jent_failures"
TESTS=${TESTS:-1000}
SAMPLES=1001
MAX_FAILURES=10

GETRAWENTROPY=${GETRAWENTROPY:-/usr/local/sbin/getrawentropy}
DEBUGFS_FILE=${DEBUGFS_FILE:-/sys/kernel/debug/jitterentropy_testing/jent_raw_hires}
RAW_OPTS=${RAW_OPTS---timestamps}

# Location of libkcapi helper tool, and the RNG it drives
KCAPIRNG=${KCAPIRNG:-/usr/bin/kcapi-rng}
KCAPI_NAME=${KCAPI_NAME:-jitterentropy_rng}

DIR=$(dirname "$OUTFILE")
if [ ! -d "$DIR" ]
then
	mkdir -p "$DIR"
fi

# $1 as a decimal number: 10# as a leading zero would otherwise make it
# octal. Anything but a decimal number fails, the empty string included.
decimal()
{
	local digits

	case $1 in
	''|*[!0-9]*)
		return 1 ;;
	esac
	# Leading zeros are no digits of the number: 00000000002 is 2.
	digits=${1#"${1%%[!0]*}"}
	digits=${digits:-0}
	# Beyond nine digits, bash arithmetic would wrap.
	[ ${#digits} -le 9 ] || return 1

	echo $((10#$digits))
}

# The counter in the file $1, decimal. A missing file is a test not yet begun,
# 0. An empty file fails, which is the truncation the replacement of the files
# below guards against.
read_counter()
{
	local val

	if [ ! -e "$1" ]
	then
		echo 0
		return 0
	fi

	val=$(cat "$1") || return 1
	decimal "$val"
}

# TESTS comes from the environment and is checked like the counters: taken
# as it is, a value such as 1e3 makes every comparison with it below fail, so
# the test never counts as complete and the machine reboots forever.
# Anything but a positive decimal number stops the test here, without a
# reboot.
if ! tests=$(decimal "$TESTS") || [ "$tests" -eq 0 ]
then
	echo "ERROR: TESTS=$TESTS is no positive decimal run count, stopping the test without a reboot - correct the setting and reboot" >&2
	exit 1
fi
TESTS=$tests

# A garbled counter stops the test here, before the reboot below, and says
# why: bash arithmetic would abort on it at every boot without a word, or,
# taken as 0, the test would start over, recording on top of its runs.
counter_garbled()
{
	echo "ERROR: $1 holds no run count, stopping the test without a reboot - correct or remove it and reboot" >&2
	exit 1
}

testruns=$(read_counter "$STATE") || counter_garbled "$STATE"
failures=$(read_counter "$FAILURES") || counter_garbled "$FAILURES"

#add leading zeros
# If leading zeros are missing, execute:
# for i in jent_raw_noise_restart.?.data; do mv $i $(echo $i | cut -d. -f1).0000$(echo $i | cut -d. -f2).$(echo $i | cut -d. -f3) ; done
# for i in jent_raw_noise_restart.??.data; do mv $i $(echo $i | cut -d. -f1).000$(echo $i | cut -d. -f2).$(echo $i | cut -d. -f3) ; done
# for i in jent_raw_noise_restart.???.data; do mv $i $(echo $i | cut -d. -f1).00$(echo $i | cut -d. -f2).$(echo $i | cut -d. -f3) ; done
# for i in jent_raw_noise_restart.????.data; do mv $i $(echo $i | cut -d. -f1).0$(echo $i | cut -d. -f2).$(echo $i | cut -d. -f3) ; done
# Zero-padded only for the file name, the counter stays decimal
printf -v run "%05d" "$testruns"

# The end of the test: the service disabled, the platform details written, no
# reboot.
test_complete()
{
	# No systemctl stop: this script is the service's ExecStart, and
	# stopping the service would kill it before it gets to the lines below.
	# It is done when the script exits.
	if ! systemctl disable boottime_test_record
	then
		echo "ERROR: The test is complete, but disabling boottime_test_record failed - disable it by hand" >&2
	fi

	uname -a > "$OUTDIR/platform.txt" &&
	cat /proc/cpuinfo >> "$OUTDIR/platform.txt" &&
	echo "" >> "$OUTDIR/platform.txt" &&
	echo "lspci" >> "$OUTDIR/platform.txt" &&
	lspci -vvv >> "$OUTDIR/platform.txt"

	exit 0
}

# A test already complete records nothing: the service still enabled - its
# disable failed, or it was enabled again without clearing out the results -
# would otherwise record run $TESTS, one more than the test holds.
[ $testruns -ge $TESTS ] && test_complete

if [ ! -x "$KCAPIRNG" ]
then
	# Not a .data file: the analysis takes every .data file for a run.
	echo "Test tool $KCAPIRNG not found" > "$OUTFILE.$run.error"
	echo "Test tool $KCAPIRNG not found" >&2
	testruns=$TESTS
else
	# The vanilla kernel interface delivers time stamps, --timestamps
	# records their deltas as consumed by the Jitter RNG (RAW_OPTS,
	# unquoted to split into its options; the paths quoted, as a setting
	# may hold a blank). The reader runs
	# while kcapi-rng drives the Jitter RNG, and is waited for before the
	# file system goes read-only and the machine restarts below: a reader
	# still writing then left a short or empty sample file. The timeout
	# keeps a reader that never gets its samples from stopping the loop,
	# and a kcapi-rng that hangs from keeping the reader waited for.
	timeout 120 "$GETRAWENTROPY" $RAW_OPTS -f "$DEBUGFS_FILE" -s $SAMPLES > "$OUTFILE.$run.data" &
	rawpid=$!
	timeout 120 "$KCAPIRNG" -n "$KCAPI_NAME" -b 2000

	# getrawentropy prints one sample per line and exactly --samples of
	# them; anything else is a failed run, whatever its exit status.
	if wait $rawpid &&
	   [ "$(wc -l < "$OUTFILE.$run.data")" -eq $SAMPLES ]
	then
		testruns=$((testruns+1))
		failures=0
	else
		echo "getrawentropy failed for run $run, recording it again" >&2
		failures=$((failures+1))
		# Numbered: a run that fails again keeps the earlier failure.
		mv -f "$OUTFILE.$run.data" "$OUTFILE.$run.data.failed.$failures"
		if [ $failures -ge $MAX_FAILURES ]
		then
			echo "$failures failed runs in a row, stopping the test" >&2
			testruns=$TESTS
		fi
	fi
	# Replaced, not rewritten in place: the machine is power cycled, and
	# a file truncated but not yet written reads as 0 - the test would
	# start over, recording on top of its runs.
	echo $testruns > "$STATE.tmp" && mv -f "$STATE.tmp" "$STATE"
	echo $failures > "$FAILURES.tmp" && mv -f "$FAILURES.tmp" "$FAILURES"
fi

# The run that completes the test
[ $testruns -ge $TESTS ] && test_complete

if (cat /proc/cmdline | grep -q boottime_test_stop) ; then
	exit 0
fi

# cannot kexec in VM (corruptions)
# Here's the snipped to run the VM:
# kvm -k de -vga vmware -usbdevice tablet -name bootloop -m 768 -smp 2 \
#      	-net nic,model=e1000,macaddr=00:50:45:00:34:0F -net user,hostfwd=tcp:127.0.0.1:24-:22
# 	-drive file=/vm-image-bootlooptests.img,format=raw,cache=writeback -boot c
#
mount -t proc proc /proc > /dev/null 2>&1
if ! grep hypervisor /proc/cpuinfo > /dev/null 2>&1 ; then
  if [ -f /boot/vmlinuz -a -f /boot/initrd ]; then
	e=$( cat /proc/cmdline)
	kexec -l /boot/vmlinuz --initrd=/boot/initrd --append="$e"
  fi
fi

# With kernel 4.9, the reboot may corrupt the file system.
# Hence, the following lines.
# Note, however, that it may be neccessary to enforce disc scan with outomatic repair on every reboot.

sync ; sync
mount -o remount,ro /

# kexec will only return upon error, like if not set up or fail of set up.
kexec -e

reboot -f
