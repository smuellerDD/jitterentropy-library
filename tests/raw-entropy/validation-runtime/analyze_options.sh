#!/bin/bash
#
# Tool to validate the test results for various Jitter RNG memory settings
#
# This tool is only needed if you have insufficient entropy. See ../README.md
# for details
#

RESULT="../results-runtime-multi"
ENT_DIR="../results-measurements"
RES_DIR="../results-analysis-runtime"
NUM_CPU=1
USED_CPUS=0

echo -e "Number of blocks\tBlocksize\tmin entropy" > $RESULT

trap "make clean" 0 1 2 3 15
make clean
make

calc() {
	local crunch=$1

	for blocks in 64 128 256 512 1024 2048 4096 8192 16384
	do
		for blocksize in 32 64 128 256 512 1024 2048 4096 8192 16384
		do
			local target="$RES_DIR-${blocks}blocks-${blocksize}blocksize"

			if [ $USED_CPUS -eq $NUM_CPU ]
			then
				# wait for all
				wait
				USED_CPUS=0
			fi

			if [ ! -d $target ]
			then
				USED_CPUS=$(($USED_CPUS+1))

				( BUILD_EXTRACT="no" ENTROPYDATA_DIR=$ENT_DIR-${blocks}blocks-${blocksize}blocksize RESULTS_DIR=$target ./processdata.sh ) &
			fi

			if [ $crunch -eq 0 ]
			then
				ent=$(grep min $target/jent-raw-noise-0001.minentropy_FF_8bits.var.txt | cut -d ":" -f 2)
				echo -e "$blocks\t$blocksize\t$ent" >> $RESULT
			fi
		done
	done
}

if [ -f /proc/cpuinfo ]
then
	NUM_CPU=$(cat /proc/cpuinfo  | grep processor | tail -n1 | cut -d":" -f 2)
	NUM_CPU=$(($NUM_CPU+1))
fi

# Number crunching
calc 1
wait
calc 0
