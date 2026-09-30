#!/usr/bin/env bash

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

# The recording tool. One the caller names here - an installed getrawentropy,
# say - is used as it is: never built over and never deleted. Without one,
# build() compiles it into a directory of its own, which cleanup() removes.
JENT_GETRAWENTROPY=${JENT_GETRAWENTROPY:-}
JENT_BUILD_DIR=

# Define the maximum memory size
# 0 -> use default
# 1 -> JENT_MAX_MEMSIZE_1kB
# ...
# 20 -> JENT_MAX_MEMSIZE_512MB
MAX_MEMORY_SIZE=${MAX_MEMORY_SIZE:-0}

PARAM_DIR="/sys/module/jitter_rng/parameters"
DEBUGFS_DIR="/sys/kernel/debug/jitter_rng/jent_raw_hires"

# A failed build or recording would leave a short or missing data set behind.
fail()
{
	echo "ERROR: $*" >&2
	exit 1
}

build()
{
	if [ -n "$JENT_GETRAWENTROPY" ]
	then
		command -v "$JENT_GETRAWENTROPY" >/dev/null ||
			fail "$JENT_GETRAWENTROPY is not an executable"
		return 0
	fi

	JENT_BUILD_DIR=$(mktemp -d) ||
		fail "creating a build directory for getrawentropy failed"
	JENT_GETRAWENTROPY=$JENT_BUILD_DIR/getrawentropy
	gcc -Wall -pedantic -Wextra -I../../../ -I../../../linux_kernel/ -DRAW_DATATYPE_U64 -o $JENT_GETRAWENTROPY getrawentropy.c ||
		fail "building getrawentropy failed"
}

# processdata_hashloop.sh and processdata_memloop.sh take this marker in
# $OUTDIR - the same directory as the user space recorders' by default - to
# mean the data are those of recording_userspace/invoke_testing_commonop.sh.
# The recorders of those data sets remove it before they start, so a marker
# left by an earlier run cannot mislabel their data. With a marker set, they
# also remove the common operation's per-size files of both sets, which carry
# the same names as theirs: once the marker is gone, those of the set they do
# not record would pass for theirs (see the user space helper).
COMMONOP_MARKER=jent-commonop-testing

hashloop_set_remove()
{
	rm -f "$OUTDIR/${NONIID_HASH_DATA}"_[0-9]*-[0-9]*.data \
	      "$OUTDIR/${NONIID_HASH_DATA}"_[0-9]*-[0-9]*-u64.bin
}

memloop_set_remove()
{
	rm -f "$OUTDIR/${NONIID_MEMLOOP_DATA}"_deterministic[0-9]*-[0-9]*.data \
	      "$OUTDIR/${NONIID_MEMLOOP_DATA}"_deterministic[0-9]*-[0-9]*-u64.bin
}

commonop_sets_remove()
{
	hashloop_set_remove
	memloop_set_remove
}

commonop_marker_remove()
{
	if [ -e "$OUTDIR/$COMMONOP_MARKER" ]
	then
		commonop_sets_remove
	fi
	rm -f "$OUTDIR/$COMMONOP_MARKER"
}

# The data a recording is writing, the file of record() or the set of
# record_restart(): set before it starts and cleared once it is complete, so
# that the exit trap removes what an abnormal exit - a signal, a fail()
# elsewhere - interrupted, rather than leave a partial data set for the
# validation to take.
JENT_PARTIAL_FILE=
JENT_PARTIAL_SET=
# Set by the trap signal_hold() installs, for a signal held back.
JENT_SIGNALLED=

partial_remove()
{
	if [ -n "$JENT_PARTIAL_FILE" ]
	then
		rm -f "$JENT_PARTIAL_FILE"
	fi
	if [ -n "$JENT_PARTIAL_SET" ]
	then
		rm -f "$JENT_PARTIAL_SET"-[0-9]*.data
	fi
	JENT_PARTIAL_FILE=
	JENT_PARTIAL_SET=
}

# Runs the recorder "$@" with a signal to the script held back. bash acts on a
# signal only once the foreground command has returned, and then, with the
# recording complete but its marker still set, the exit trap would remove it
# as partial. Held back, the caller clears its marker on success first and
# then calls signal_release(), which exits for a signal that came meanwhile -
# removing the data only if the recorder did not complete it.
signal_hold()
{
	JENT_SIGNALLED=
	trap 'JENT_SIGNALLED=1' 1 2 3 15
	"$@"
}

signal_release()
{
	trap 'exit 1' 1 2 3 15
	if [ -n "$JENT_SIGNALLED" ]
	then
		exit 1
	fi
}

# $1: output file, remaining arguments passed on to getrawentropy
record()
{
	out=$1
	shift

	JENT_PARTIAL_FILE=$out
	if ! signal_hold $JENT_GETRAWENTROPY "$@" > $out
	then
		signal_release
		partial_remove
		fail "getrawentropy failed recording $out"
	fi
	JENT_PARTIAL_FILE=
	signal_release
}

# $1: set name, $2: number of restarts, remaining arguments passed on to
# getrawentropy for each of the files $1-0001.data to $1-<$2>.data, numbered
# from 1 as jitterentropy-hashtime numbers its repeats. A failed recording
# leaves nothing behind for the validation to take as a data set: the files of
# the restarts recorded before the failure are removed with the failed one, as
# the user space hashtime_record() does, and so are they on any other abnormal
# exit before the set is complete (see partial_remove()). The restart number
# starts with a digit, which no other data set's name does after $1-.
record_restart()
{
	local set=$1 num=$2 ctr=1 out
	shift 2

	JENT_PARTIAL_SET=$set
	while [ $ctr -le $num ]
	do
		out=$set-$(printf "%04d" "$ctr").data
		if ! signal_hold $JENT_GETRAWENTROPY "$@" > $out
		then
			signal_release
			partial_remove
			fail "getrawentropy failed recording $out"
		fi

		ctr=$((ctr+1))
		# Before the last, the set is not complete yet: a signal held
		# back exits, and the exit trap removes what was recorded.
		if [ $ctr -le $num ]
		then
			signal_release
		fi
	done
	JENT_PARTIAL_SET=
	signal_release
}

# Removes only what build() created; the next build() creates it anew.
cleanup()
{
	if [ -n "$JENT_BUILD_DIR" ]
	then
		rm -rf "$JENT_BUILD_DIR"
		JENT_BUILD_DIR=
		JENT_GETRAWENTROPY=
	fi
}

initialization()
{
	local uid

	uid=$(id -u)
	if [ $uid -ne 0 ]
	then
		echo "Execute script as root!"
		exit 1
	fi

	if [ ! -d $OUTDIR ]
	then
		if ! mkdir $OUTDIR
		then
			echo "Creation of $OUTDIR failed"
			exit 1
		fi
	fi

	# Keep the exit status of the script across the cleanup, which set -e
	# must not cut short.
	trap 'rc=$?; set +e; partial_remove; cleanup; exit $rc' 0
	trap 'exit 1' 1 2 3 15
}

raw_entropy_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE -f $DEBUGFS_DIR --param-dir $PARAM_DIR $*"

	build
	record_restart $OUTDIR/$NONIID_RESTART_DATA $NUM_RESTART -s $NUM_EVENTS_RESTART $cmdopts

	cleanup
}

raw_entropy()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE -f $DEBUGFS_DIR --param-dir $PARAM_DIR $*"

	build
	record $OUTDIR/$NONIID_DATA-0001.data -s $NUM_EVENTS $cmdopts
	cleanup
}

raw_entropy_ntg1_hash()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --hashloop -f $DEBUGFS_DIR --param-dir $PARAM_DIR $*"

	build
	record $OUTDIR/$NONIID_HASH_DATA-0001.data -s $NUM_EVENTS $cmdopts
	cleanup
}

raw_entropy_ntg1_hash_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --hashloop -f $DEBUGFS_DIR --param-dir $PARAM_DIR $*"

	build
	record_restart $OUTDIR/$NONIID_HASH_RESTART_DATA $NUM_RESTART -s $NUM_EVENTS_RESTART $cmdopts

	cleanup
}

raw_entropy_ntg1_memacc()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --memaccess -f $DEBUGFS_DIR --param-dir $PARAM_DIR $*"

	build
	record $OUTDIR/$NONIID_MEMLOOP_DATA-0001.data -s $NUM_EVENTS $cmdopts
	cleanup
}

raw_entropy_ntg1_memacc_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --memaccess -f $DEBUGFS_DIR --param-dir $PARAM_DIR $*"

	build
	record_restart $OUTDIR/$NONIID_MEMLOOP_RESTART_DATA $NUM_RESTART -s $NUM_EVENTS_RESTART $cmdopts

	cleanup
}
