#!/bin/bash
#
# In some sources that display statistical dependence, the observed dependence can 
# only be prominent when samples are taken to be sufficiently close to each other.
# By increasing JENT_MEMORY_ACCESSLOOPS_EXP, the number of discarded measurements
# between used measurements for the memaccess source can be increased.
# This can be used to decrease the dependence between used samples
#
# This should only be run once the memory size and the distribution has been selected.
#

set -eu

OUTDIR="../results-measurements"
export sampleSize=1000000
export sampleRounds=147

#These parameters are hardware specific. These are the values used in the example.
MEMEXP=28
DISTMIN=100
DISTMAX=200

if (( (DISTMAX - DISTMIN) <= 255 )); then
	FILETYPE="sd.bin"
elif (( (DISTMAX - DISTMIN) <= 4294967295)); then
	FILETYPE="u32.bin"
else
	FILETYPE="u64.bin"
fi

for bits in {0..16}
do
	export CFLAGS="-DJENT_MEMORY_DEPTH_EXP=${bits} -DJENT_MEMORY_SIZE_EXP=${MEMEXP} -DJENT_DISTRIBUTION_MIN=${DISTMIN} -DJENT_DISTRIBUTION_MAX=${DISTMAX}"

	./invoke_testing.sh

	for round in $(seq -f "%04g" 1 $sampleRounds); do
		cat $OUTDIR/jent-raw-noise-${round}-${FILETYPE} >> $OUTDIR/jent-raw-noise-${FILETYPE}
		rm -f $OUTDIR/jent-raw-noise-${round}-${FILETYPE}
	done

	mv $OUTDIR $OUTDIR-random_memaccess-depth-${bits}

done
