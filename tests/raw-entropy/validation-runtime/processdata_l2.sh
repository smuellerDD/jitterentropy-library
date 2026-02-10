#!/bin/sh
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
#quasirandom=""
min_deterministic=""
min_pair_deterministic=""
min_triple_deterministic=""
#min_quasirandom=""
while [ $size -le 20 ]
do
	det=$(grep H_original ../results-analysis-runtime/jent-raw-noise_memaccloop_deterministic${size}-0001.minentropy_FF_8bits.txt | grep min | cut -f2 -d":")

	#rnd=$(grep H_original ../results-analysis-runtime/jent-raw-noise_memaccloop_quasirandom${size}-0001.minentropy_FF_8bits.txt | grep min | cut -f2 -d":")

	tmp_det=$(Rscript --vanilla processdata_l2_minentropy.r ../results-measurements/jent-raw-noise_memaccloop_deterministic${size}-0001.data 2>/dev/null| cut -d " " -f 2)

	min_det=$(echo $tmp_det | cut -d " " -f 1)

	min_pair_det=$(echo $tmp_det | cut -d " " -f 2)

	min_triple_det=$(echo $tmp_det | cut -d " " -f 3)

	#min_rnd=$(Rscript --vanilla processdata_l2_minentropy.r ../results-measurements/jent-raw-noise_memaccloop_quasirandom${size}-0001.data 2>/dev/null| cut -d " " -f 2)

	if [ $size -eq 1 ]
	then
		deterministic="$det"
		#quasirandom="$rnd"

		min_deterministic="$min_det"
		#min_quasirandom="$min_rnd"

		min_pair_deterministic="$min_pair_det"

		min_triple_deterministic="$min_triple_det"
	else
		deterministic="$deterministic, $det"
		#quasirandom="$quasirandom, $rnd"

		min_deterministic="$min_deterministic, $min_det"
		#min_quasirandom="$min_quasirandom, $min_rnd"

		min_pair_deterministic="$min_pair_deterministic, $min_pair_det"

		min_triple_deterministic="$min_triple_deterministic, $min_triple_det"
	fi

	size=$((size+1))
done

echo $deterministic > ../results-analysis-runtime/minentropy_collected
#echo $quasirandom >> ../results-analysis-runtime/minentropy_collected
echo $min_deterministic >> ../results-analysis-runtime/minentropy_collected
#echo $min_quasirandom >> ../results-analysis-runtime/minentropy_collected
echo $min_pair_deterministic >> ../results-analysis-runtime/minentropy_collected
echo $min_triple_deterministic >> ../results-analysis-runtime/minentropy_collected
Rscript --vanilla processdata_l2.r ../results-analysis-runtime/minentropy_collected
