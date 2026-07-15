#!/bin/sh
#
# This test is intended to analyze the hash lopp entropy rate. It invokes
# the hash operation with all supported hash loop iterations and measures its
# execution time.
#
# By providing the measurement of the hash loop iteration behavior, the impact
# of the iteration count on the entropy rate can be analyzed.
#

. ./invoke_testing_helper.sh

raw_entropy_ntg1_hashloop()
{
	local hashloop=$1
	shift

	echo "---"
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--hloopcnt ${hashloop} --hashloop -f $DEBUGFS_DIR --param-dir $PARAM_DIR $@"

	$JENT_GETRAWENTROPY -s $NUM_EVENTS $cmdopts > $OUTDIR/${NONIID_HASH_DATA}_${hashloop}.data

	echo "---"
}

initialization

################################################################################
build

size=0
while [ $size -le 7 ]
do
	raw_entropy_ntg1_hashloop $size --ntg1
	size=$((size+1))
done

cleanup
