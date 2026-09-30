#!/usr/bin/env bash
#
# This test is intended to analyze the entropy rate of the common operation
# when adjusting the hashloop count and memory size. It invokes the common
# operation with all supported memory sizes and hashloop iteration counts and
# measures its execution time.
#
# The memory size is selected with --max-mem, which the library applies as
# given (up to JENT_MAX_MEMSIZE_MAX) rather than deriving it from the cache
# size, so all memory sizes can be analyzed.

set -euxo pipefail

. ./invoke_testing_helper.sh

raw_entropy_ntg1_memloop()
{
	local memsize=$1
	shift
	local testtype=$1
	shift

	echo "---"
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--max-mem ${memsize} $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --force-internal-timer"
	fi

	hashtime_record $NUM_EVENTS 1 $OUTDIR/${NONIID_MEMLOOP_DATA}_${testtype}${memsize} $cmdopts

	echo "---"
}

raw_entropy_ntg1_hashloop()
{
	local hashloop=$1
	shift

	echo "---"
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--hloopcnt ${hashloop} $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --force-internal-timer"
	fi

	hashtime_record $NUM_EVENTS 1 $OUTDIR/${NONIID_HASH_DATA}_${hashloop} $cmdopts

	echo "---"
}

initialization
# The marker that this is the common operation, set before recording: a run
# that fails halfway leaves files that are still labelled as its own.
commonop_marker_set

################################################################################
hashtime_build

size=0
while [ $size -le 7 ]
do
	raw_entropy_ntg1_hashloop $size --ntg1
	size=$((size+1))
done

make -s -f Makefile.hashtime clean

################################################################################
# Measure with random memory access
hashtime_build

size=1
while [ $size -le 20 ]
do
	raw_entropy_ntg1_memloop $size "deterministic" --ntg1
	size=$((size+1))
done

make -s -f Makefile.hashtime clean
