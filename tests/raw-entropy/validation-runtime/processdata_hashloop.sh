#!/usr/bin/env bash
#
# Process the entropy data

set -euxo pipefail

############################################################
# Configuration values                                     #
############################################################

ENTROPYDATA_DIR=${1:-}
RESULTS_DIR=${2:-}

if [ -n "$RESULTS_DIR" ]
then
	BUILD_EXTRACT="no"
fi

ENTROPYDATA_DIR=${ENTROPYDATA_DIR:-"../results-measurements"}
RESULTS_DIR=${RESULTS_DIR:-"../results-analysis-runtime"}

NONIID_DATA="$(for i in $ENTROPYDATA_DIR/jent-raw-noise_hashloop_*; do basename $i; done)"

############################################################
# Code only after this line -- do not change               #
############################################################

# The NTG.1 hash loop alone runs JENT_HASH_LOOP_INIT (3) times the configured
# count per time delta, the common operation the count itself.
diagrammain='Hash Loop Entropy Rate'
loopfactor=3
if [ -f $ENTROPYDATA_DIR/jent-commonop-testing ]
then
	diagrammain='Jitter RNG Entropy Rate'
	loopfactor=1
fi

# All 8 hash loop counts must be recorded, checked before the long analysis: a
# missing one would be an empty value, which the R script reads as NA and plots
# without notice.
size=0
while [ $size -le 7 ]
do
	data=$ENTROPYDATA_DIR/jent-raw-noise_hashloop_${size}-0001.data
	if [ ! -f "$data" ]
	then
		echo "ERROR: Recording $data of hash loop count $size is missing" >&2
		exit 1
	fi
	size=$((size+1))
done

. ./processdata_helper.sh

size=0
deterministic=""
min_deterministic=""
min_pair_deterministic=""
min_triple_deterministic=""
while [ $size -le 7 ]
do
	data=$ENTROPYDATA_DIR/jent-raw-noise_hashloop_${size}-0001.data

	# || true: a missing result is reported below, set -e would end here.
	det=$(grep H_original $RESULTS_DIR/jent-raw-noise_hashloop_${size}-0001.minentropy_*_8bits.txt 2>/dev/null | grep min | cut -f2 -d":") || true

	tmp_det=$(Rscript --vanilla processdata_minentropy.r $data 2>/dev/null| cut -d " " -f 2) || true

	min_det=$(echo $tmp_det | cut -d " " -f 1)

	min_pair_det=$(echo $tmp_det | cut -d " " -f 2)

	min_triple_det=$(echo $tmp_det | cut -d " " -f 3)

	if [ -z "$det" ] || [ -z "$min_det" ] || [ -z "$min_pair_det" ] ||
	   [ -z "$min_triple_det" ]
	then
		echo "ERROR: Entropy results of $data are missing" >&2
		exit 1
	fi

	if [ $size -eq 0 ]
	then
		deterministic="$det"

		min_deterministic="$min_det"

		min_pair_deterministic="$min_pair_det"

		min_triple_deterministic="$min_triple_det"
	else
		deterministic="$deterministic, $det"

		min_deterministic="$min_deterministic, $min_det"

		min_pair_deterministic="$min_pair_deterministic, $min_pair_det"

		min_triple_deterministic="$min_triple_deterministic, $min_triple_det"
	fi

	size=$((size+1))
done

echo $deterministic > $RESULTS_DIR/minentropy_collected_hashloop
echo $min_deterministic >> $RESULTS_DIR/minentropy_collected_hashloop
echo $min_pair_deterministic >> $RESULTS_DIR/minentropy_collected_hashloop
echo $min_triple_deterministic >> $RESULTS_DIR/minentropy_collected_hashloop
Rscript --vanilla processdata_hashloop.r "$diagrammain" $RESULTS_DIR/minentropy_collected_hashloop $loopfactor
