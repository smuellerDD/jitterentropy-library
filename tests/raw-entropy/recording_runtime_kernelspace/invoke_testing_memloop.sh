#!/usr/bin/env bash
#
# This test is intended to analyze the memory access entropy rate. It invokes
# the memory access with all supported memory sizes and measures its execution
# time.
#
# Each size is passed to the kernel as the JENT_MAX_MEMSIZE_* field of
# testing_flags, which the collector takes as given - there is no check to
# bypass, only the library's 512 MB maximum. The kernel must still be able to
# allocate the region, though: a size it cannot (such as one above about 64 MB
# on a 32-bit kernel, whose vmalloc area is small) fails the open of the test
# interface with ENOMEM, and record() then aborts the script. The sizes
# recorded before remain; the larger ones are not recorded, and
# validation-runtime/processdata_memloop.sh analyzes the recorded ones only.
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

	local cmdopts="--max-mem ${memsize} --memaccess -f $DEBUGFS_DIR --param-dir $PARAM_DIR $*"

	record $OUTDIR/${NONIID_MEMLOOP_DATA}_${testtype}${memsize}-0001.data -s $NUM_EVENTS $cmdopts

	echo "---"
}

initialization
commonop_marker_remove
# A run replaces the whole set: one stopped halfway leaves no sizes of an
# earlier run for the analysis to take as part of its own.
memloop_set_remove

################################################################################
build

size=1
while [ $size -le 20 ]
do
	raw_entropy_ntg1_memloop $size "deterministic" --ntg1
	size=$((size+1))
done

cleanup
