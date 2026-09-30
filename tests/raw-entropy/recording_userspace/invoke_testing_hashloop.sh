#!/usr/bin/env bash
#
# This test is intended to analyze the hash lopp entropy rate. It invokes
# the hash operation with all supported hash loop iterations and measures its
# execution time.
#
# By providing the measurement of the hash loop iteration behavior, the impact
# of the iteration count on the entropy rate can be analyzed.
#

set -euxo pipefail

. ./invoke_testing_helper.sh

raw_entropy_ntg1_hashloop()
{
	local hashloop=$1
	shift

	echo "---"
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--hloopcnt ${hashloop} --hashloop $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --force-internal-timer"
	fi

	hashtime_record $NUM_EVENTS 1 $OUTDIR/${NONIID_HASH_DATA}_${hashloop} $cmdopts

	echo "---"
}

initialization
commonop_marker_remove
# A run replaces the whole set: one stopped halfway leaves no sizes of an
# earlier run for the analysis to take as part of its own.
hashloop_set_remove

################################################################################
hashtime_build

size=0
while [ $size -le 7 ]
do
	raw_entropy_ntg1_hashloop $size --ntg1
	size=$((size+1))
done

make -s -f Makefile.hashtime clean
