#!/bin/sh
#
# This test is intended to analyze the memory access entropy rate. It invokes
# the memory access with all supported memory sizes and measures its execution
# time.
#
# The testing disables the maximum memory check to allow analyzing all
# memory sizes.
#
# Specifically with the deterministic memory access pattern, the measurement
# is intended to show the access variations of the "just" the cache that
# can retain the allocated memory, i.e. if L1 data cache is 128kBytes and
# the test invocation allocates 32kByte, the L1 data cache variations are
# measured. Contrary, if 1MByte memory is allocated, only the L2 cache is
# measured, provided the L2 cache is larger than 1MByte. The reason for this
# is the following:
#
# 1. The deterministic memory access performs very few operation outside the
#    actually measured memory read and write operations: an ADD (for the loop
#    counter), an ADD and an AND for the update of the memory value and an
#    ADD and modulo operation to adjust the memory pointer. These operations
#    are fed from the L1 data and instruction cache, but its impact is assumed
#    to not significantly impact the actual memory access variation measurement.
#
# 2. The memory access pattern evenly tries to access bytes spaced blocksize
#    bytes apart. Blocksize is larger than a cacheline. That means that (a)
#    every byte requires at least a new cacheline to be populated, and (b)
#    reading the same byte again only happens if all other bytes are accessed
#    which implies that by using a memory size that is larger than L1, there
#    will always be L1 data cache-misses for accessing the bytes in the memory.
#

. ./invoke_testing_helper.sh

raw_entropy_ntg1_memloop()
{
	local memsize=$1
	shift
	local testtype=$1
	shift

	echo "---"
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--max-mem ${memsize} --memaccess $@"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --disable-internal-timer"
	fi

	$JENT_HASHTIME $NUM_EVENTS 1 $OUTDIR/${NONIID_MEMLOOP_DATA}_${testtype}${memsize} $cmdopts

	echo "---"
}

initialization

################################################################################
# Measure with deterministic memory access
CFLAGS="-DJENT_TEST_MEASURE_RAW_MEMORY_ACCESS -DJENT_TESTING_MEMSIZE_NO_BOUNDSCHECK" make -s -f Makefile.hashtime

size=1
while [ $size -le 20 ]
do
	raw_entropy_ntg1_memloop $size "deterministic" --ntg1
	size=$((size+1))
done

make -s -f Makefile.hashtime clean
