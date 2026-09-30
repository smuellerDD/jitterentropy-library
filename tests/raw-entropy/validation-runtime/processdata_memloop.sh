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

NONIID_DATA="$(for i in $ENTROPYDATA_DIR/jent-raw-noise_memaccloop_deterministic*; do basename $i; done)"

############################################################
# Code only after this line -- do not change               #
############################################################

diagrammain='Memory Access Entropy Rate'
if [ -f $ENTROPYDATA_DIR/jent-commonop-testing ]
then
	diagrammain='Jitter RNG Entropy Rate'
fi

# The recorded memory sizes, checked before the long analysis. A sweep may end
# early - the kernel's invoke_testing_memloop.sh stops at the first size the
# kernel cannot allocate - so the smallest sizes up to the last one recorded are
# analyzed and each larger one is reported as not recorded. A gap, or no size
# at all, is an error: an empty value would be read as NA by the R script and
# plotted without notice.
size=1
maxsize=0
while [ $size -le 20 ]
do
	data=$ENTROPYDATA_DIR/jent-raw-noise_memaccloop_deterministic${size}-0001.data
	if [ -f "$data" ]
	then
		if [ $maxsize -ne $((size-1)) ]
		then
			echo "ERROR: Recording of memory size $((maxsize+1)) is missing, but memory size $size is recorded" >&2
			exit 1
		fi
		maxsize=$size
	fi
	size=$((size+1))
done

if [ $maxsize -eq 0 ]
then
	echo "ERROR: No memory size is recorded in $ENTROPYDATA_DIR" >&2
	exit 1
fi

size=$((maxsize+1))
while [ $size -le 20 ]
do
	echo "WARNING: Memory size $size is not recorded, it is left out of the analysis" >&2
	size=$((size+1))
done

. ./processdata_helper.sh

size=1
deterministic=""
min_deterministic=""
min_pair_deterministic=""
min_triple_deterministic=""
while [ $size -le $maxsize ]
do
	data=$ENTROPYDATA_DIR/jent-raw-noise_memaccloop_deterministic${size}-0001.data

	# || true: a missing result is reported below, set -e would end here.
	det=$(grep H_original $RESULTS_DIR/jent-raw-noise_memaccloop_deterministic${size}-0001.minentropy_*_8bits.txt 2>/dev/null | grep min | cut -f2 -d":") || true

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

	if [ $size -eq 1 ]
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

echo $deterministic > $RESULTS_DIR/minentropy_collected_memloop
echo $min_deterministic >> $RESULTS_DIR/minentropy_collected_memloop
echo $min_pair_deterministic >> $RESULTS_DIR/minentropy_collected_memloop
echo $min_triple_deterministic >> $RESULTS_DIR/minentropy_collected_memloop
Rscript --vanilla processdata_memloop.r "$diagrammain" $RESULTS_DIR/minentropy_collected_memloop
