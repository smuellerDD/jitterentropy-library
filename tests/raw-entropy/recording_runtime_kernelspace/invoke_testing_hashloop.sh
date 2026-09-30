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

	local cmdopts="--hloopcnt ${hashloop} --hashloop -f $DEBUGFS_DIR --param-dir $PARAM_DIR $*"

	record $OUTDIR/${NONIID_HASH_DATA}_${hashloop}-0001.data -s $NUM_EVENTS $cmdopts

	echo "---"
}

initialization
commonop_marker_remove
# A run replaces the whole set: one stopped halfway leaves no sizes of an
# earlier run for the analysis to take as part of its own.
hashloop_set_remove

################################################################################
build

size=0
while [ $size -le 7 ]
do
	raw_entropy_ntg1_hashloop $size --ntg1
	size=$((size+1))
done

cleanup
