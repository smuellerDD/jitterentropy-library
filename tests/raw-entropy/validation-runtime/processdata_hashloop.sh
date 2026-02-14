#!/bin/sh
#
# Process the entropy data

############################################################
# Configuration values                                     #
############################################################

ENTROPYDATA_DIR=$1
RESULTS_DIR=$2

if [ -n "$RESULTS_DIR" ]
then
	BUILD_EXTRACT="no"
fi

ENTROPYDATA_DIR=${ENTROPYDATA_DIR:-"../results-measurements"}
RESULTS_DIR=${RESULTS_DIR:-"../results-analysis-runtime"}

NONIID_DATA="$(for i in $ENTROPYDATA_DIR/jent*; do basename $i; done)"

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh

size=0
deterministic=""
min_deterministic=""
min_pair_deterministic=""
min_triple_deterministic=""
while [ $size -le 7 ]
do
	det=$(grep H_original $RESULTS_DIR/jent-raw-noise_hashloop_${size}-0001.minentropy_FF_8bits.txt | grep min | cut -f2 -d":")

	tmp_det=$(Rscript --vanilla processdata_minentropy.r $ENTROPYDATA_DIR/jent-raw-noise_hashloop_${size}-0001.data 2>/dev/null| cut -d " " -f 2)

	min_det=$(echo $tmp_det | cut -d " " -f 1)

	min_pair_det=$(echo $tmp_det | cut -d " " -f 2)

	min_triple_det=$(echo $tmp_det | cut -d " " -f 3)

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
Rscript --vanilla processdata_hashloop.r $RESULTS_DIR/minentropy_collected_hashloop
