#!/bin/bash
#
# Tool to generate test results for various Jitter RNG memory settings
#
# This tool is only needed if you have insufficient entropy. See ../README.md
# for details
#

OUTDIR="../results-measurements"

for blocks in 64 128 256 512 1024 2048 4096 8192 16384
do
	for blocksize in 32 64 128 256 512 1024 2048 4096 8192 16384
	do
		export CFLAGS="-DJENT_MEMORY_BLOCKS=$blocks -DJENT_MEMORY_BLOCKSIZE=$blocksize"

		./invoke_testing.sh

		mv $OUTDIR $OUTDIR-${blocks}blocks-${blocksize}blocksize
	done
done
