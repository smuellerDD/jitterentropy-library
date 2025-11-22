#!/bin/bash
#
# Process the entropy data

############################################################
# Configuration values                                     #
############################################################

NONIID_DATA="$(for i in ../results-measurements/jent*; do basename $i; done)"

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh

size=1
deterministic=""
quasirandom=""
while [ $size -le 20 ]
do
	det=$(grep H_original ../results-analysis-runtime/jent-raw-noise_memaccloop_deterministic${size}-0001.minentropy_FF_8bits.txt | grep min | cut -f2 -d":")

	rnd=$(grep H_original ../results-analysis-runtime/jent-raw-noise_memaccloop_quasirandom${size}-0001.minentropy_FF_8bits.txt | grep min | cut -f2 -d":")

	if [ $size -eq 1 ]
	then
		deterministic="$det"
		quasirandom="$rnd"
	else
		deterministic="$deterministic, $det"
		quasirandom="$quasirandom, $rnd"
	fi

	size=$((size+1))
done

echo $deterministic > ../results-analysis-runtime/minentropy_collected
echo $quasirandom >> ../results-analysis-runtime/minentropy_collected
Rscript --vanilla processdata_l2.r ../results-analysis-runtime/minentropy_collected
