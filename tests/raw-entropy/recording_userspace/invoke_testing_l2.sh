#!/bin/bash

set -x

. ./invoke_testing_helper.sh

raw_entropy_ntg1_l2()
{
	local memsize=$1
	shift

	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--max-mem ${memsize} --memaccess $@"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --disable-internal-timer"
	fi

	$JENT_HASHTIME $NUM_EVENTS 1 $OUTDIR/${NONIID_MEMLOOP_DATA}_deterministic${memsize} $cmdopts
}

initialization

CFLAGS=-DJENT_MEASURE_RAW_MEMORY_ACCESS make -s -f Makefile.hashtime

size=1
while [ $size -le 15 ]
do
	raw_entropy_ntg1_l2 $size --ntg1
	size=$((size+1))
done

make -s -f Makefile.hashtime clean
