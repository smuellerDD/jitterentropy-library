#!/bin/sh
#
# This test is intended to analyze the entropy rate of the common operation
# when adjusting the hashloop count and memory size. It invokes the common
# operation with all supported memory sizes and hashloop iteration counts and
# measures its execution time.
#
# The testing disables the maximum memory check to allow analyzing all
# memory sizes.

. ./invoke_testing_helper.sh

raw_entropy_ntg1_memloop()
{
	local memsize=$1
	shift
	local testtype=$1
	shift

	echo "---"
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--max-mem ${memsize} $@"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --disable-internal-timer"
	fi

	$JENT_HASHTIME $NUM_EVENTS 1 $OUTDIR/${NONIID_MEMLOOP_DATA}_${testtype}${memsize} $cmdopts

	echo "---"
}

raw_entropy_ntg1_hashloop()
{
	local hashloop=$1
	shift

	echo "---"
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--hloopcnt ${hashloop} $@"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --disable-internal-timer"
	fi

	$JENT_HASHTIME $NUM_EVENTS 1 $OUTDIR/${NONIID_HASH_DATA}_${hashloop} $cmdopts

	echo "---"
}

initialization

################################################################################
make -s -f Makefile.hashtime

size=0
while [ $size -le 7 ]
do
	raw_entropy_ntg1_hashloop $size --ntg1
	size=$((size+1))
done

make -s -f Makefile.hashtime clean

################################################################################
# Measure with random memory access
CFLAGS="-DJENT_TESTING_MEMSIZE_NO_BOUNDSCHECK" make -s -f Makefile.hashtime

size=1
while [ $size -le 20 ]
do
	raw_entropy_ntg1_memloop $size "deterministic" --ntg1
	size=$((size+1))
done

make -s -f Makefile.hashtime clean

# add a marker that this is the common operation
touch $OUTDIR/jent-commonop-testing
