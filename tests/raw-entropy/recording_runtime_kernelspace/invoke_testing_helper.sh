#!/bin/sh

# Directory where to store the measurements
OUTDIR=${OUTDIR:-"../results-measurements"}

# Maximum number of entries to be extracted from the original file
NUM_EVENTS=1000000

# Number of restart tests
NUM_EVENTS_RESTART=1000
NUM_RESTART=1000

NONIID_RESTART_DATA="jent-raw-noise-restart"
NONIID_DATA="jent-raw-noise"
NONIID_HASH_DATA="jent-raw-noise_hashloop"
NONIID_HASH_RESTART_DATA="jent-raw-noise-hashloop-restart"
NONIID_MEMLOOP_DATA="jent-raw-noise_memaccloop"
NONIID_MEMLOOP_RESTART_DATA="jent-raw-noise-memaccloop-restart"
IID_DATA="jent-conditioned.data"

JENT_GETRAWENTROPY=${JENT_GETRAWENTROPY:-"./getrawentropy"}

# Define the maximum memory size
# 0 -> use default
# 1 -> JENT_MAX_MEMSIZE_1kB
# ...
# 20 -> JENT_MAX_MEMSIZE_512MB
MAX_MEMORY_SIZE=0

PARAM_DIR="/sys/module/jitter_rng/parameters"
DEBUGFS_DIR="/sys/kernel/debug/jitter_rng/jent_raw_hires"

build()
{
	gcc -Wall -pedantic -Wextra -I../../../ -I../../../linux_kernel/ -DRAW_DATATYPE_U64 -o $JENT_GETRAWENTROPY getrawentropy.c
}

cleanup()
{
	rm -f $JENT_GETRAWENTROPY
}

initialization()
{
	local uid=$(id -u)
	if [ $uid -ne 0 ]
	then
		echo "Execute script as root!"
		exit 1
	fi

	if [ ! -d $OUTDIR ]
	then
		mkdir $OUTDIR
		if [ $? -ne 0 ]
		then
			echo "Creation of $OUTDIR failed"
			exit 1
		fi
	fi

	trap "rm -f $JENT_GETRAWENTROPY; exit" 0 1 2 3 15
}

raw_entropy_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE -f $DEBUGFS_DIR --param-dir $PARAM_DIR $@"
	local ctr=0

	build

	while [ $ctr -lt $NUM_RESTART ]
	do
		printf -v ctrval "%04d" $ctr
		$JENT_GETRAWENTROPY -s $NUM_EVENTS_RESTART $cmdopts >  $OUTDIR/$NONIID_RESTART_DATA-$ctrval.data

		ctr=$((ctr+1))
	done

	cleanup
}

raw_entropy()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE -f $DEBUGFS_DIR --param-dir $PARAM_DIR $@"

	build
	$JENT_GETRAWENTROPY -s $NUM_EVENTS $cmdopts > $OUTDIR/$NONIID_DATA-0001.data
	cleanup
}

raw_entropy_ntg1_hash()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --hashloop -f $DEBUGFS_DIR --param-dir $PARAM_DIR $@"

	build
	$JENT_GETRAWENTROPY -s $NUM_EVENTS $cmdopts > $OUTDIR/$NONIID_HASH_DATA-0001.data
	cleanup
}

raw_entropy_ntg1_hash_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --hashloop -f $DEBUGFS_DIR --param-dir $PARAM_DIR $@"
	local ctr=0

	build

	while [ $ctr -lt $NUM_RESTART ]
	do
		printf -v ctrval "%04d" $ctr
		$JENT_GETRAWENTROPY -s $NUM_EVENTS_RESTART $cmdopts >  $OUTDIR/$NONIID_HASH_RESTART_DATA-$ctrval.data

		ctr=$((ctr+1))
	done

	cleanup
}

raw_entropy_ntg1_memacc()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --memaccess -f $DEBUGFS_DIR --param-dir $PARAM_DIR $@"

	build
	$JENT_GETRAWENTROPY -s $NUM_EVENTS $cmdopts > $OUTDIR/$NONIID_MEMLOOP_DATA-0001.data
	cleanup
}

raw_entropy_ntg1_memacc_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --memaccess -f $DEBUGFS_DIR --param-dir $PARAM_DIR $@"
	local ctr=0

	build

	while [ $ctr -lt $NUM_RESTART ]
	do
		printf -v ctrval "%04d" $ctr
		$JENT_GETRAWENTROPY -s $NUM_EVENTS_RESTART $cmdopts >  $OUTDIR/$NONIID_MEMLOOP_RESTART_DATA-$ctrval.data

		ctr=$((ctr+1))
	done

	cleanup
}
