#!/usr/bin/env bash

# Directory where to store the measurements
OUTDIR=${OUTDIR:-"../results-measurements"}

# Maximum number of entries to be extracted from the original file
NUM_EVENTS=${NUM_EVENTS:-1000000}

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

JENT_HASHTIME=${JENT_HASHTIME:-"./jitterentropy-hashtime"}

# Define the maximum memory size
# 0 -> use default
# 1 -> JENT_MAX_MEMSIZE_1kB
# ...
# 20 -> JENT_MAX_MEMSIZE_512MB
MAX_MEMORY_SIZE=${MAX_MEMORY_SIZE:-0}

# If this variable is set to any value, the timer-less entropy source - the
# internal timer thread in place of the platform clock - is forced and tested
# (--force-internal-timer, JENT_FORCE_INTERNAL_TIMER). NTG.1 forbids it.
FORCE_NOTIME_NOISE_SOURCE=${FORCE_NOTIME_NOISE_SOURCE:-}

# A failed build or recording would leave a short or missing data set behind.
fail()
{
	echo "ERROR: $*" >&2
	exit 1
}

# Arguments are passed on to make, e.g. CFLAGS=-D... for a variant. Always a
# build from scratch: make goes by time stamps, not by flags, so a binary left
# behind by a build of the other variant would be taken as up to date and
# record the wrong memory access pattern.
hashtime_build()
{
	make -s -f Makefile.hashtime clean
	make -s -f Makefile.hashtime "$@" ||
		fail "building jitterentropy-hashtime failed"
}

# processdata_hashloop.sh and processdata_memloop.sh take this marker in
# $OUTDIR to mean the data are invoke_testing_commonop.sh's. The per-size files
# of the hash loop and memory access sets carry the same names whichever
# recorder wrote them, so only the marker tells them apart, and a recorder
# starting on them removes what the other one left there. The common operation
# removes both sets and sets the marker before it records, so its files are
# labelled even if it fails halfway (commonop_marker_set()). The hash loop and
# memory access recorders remove the marker, and with a marker set both sets:
# the common operation's files of the set they do not record would otherwise
# pass for theirs, and those of their own set for a complete run of theirs
# (commonop_marker_remove()).
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

commonop_marker_set()
{
	commonop_sets_remove
	touch "$OUTDIR/$COMMONOP_MARKER" ||
		fail "creating $OUTDIR/$COMMONOP_MARKER failed"
}

commonop_marker_remove()
{
	if [ -e "$OUTDIR/$COMMONOP_MARKER" ]
	then
		commonop_sets_remove
	fi
	rm -f "$OUTDIR/$COMMONOP_MARKER"
}

# The data a recording is writing, the output file of lfsroutput() or the set
# of hashtime_record(): set before it starts and cleared once it is complete,
# so that the exit trap removes what an abnormal exit - a signal, a fail()
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
		rm -f "$JENT_PARTIAL_SET"-[0-9]*.data \
		      "$JENT_PARTIAL_SET"-[0-9]*-u64.bin
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

# Arguments as for jitterentropy-hashtime, $3 is the output name. A failed
# recording leaves nothing behind for the validation to take as a data set:
# the files of the repeats recorded before the failure, $3-<repeat>.data (or
# -u64.bin), are removed, as they are on any other abnormal exit before the
# set is complete. The repeat number starts with a digit, which no other data
# set's name does after $3-.
hashtime_record()
{
	JENT_PARTIAL_SET=$3
	if ! signal_hold $JENT_HASHTIME "$@"
	then
		signal_release
		partial_remove
		fail "jitterentropy-hashtime failed recording $3"
	fi
	JENT_PARTIAL_SET=
	signal_release
}

initialization()
{
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
	trap 'rc=$?; set +e; partial_remove; make -s -f Makefile.rng clean; make -s -f Makefile.hashtime clean; exit $rc' 0
	trap 'exit 1' 1 2 3 15
}

lfsroutput()
{
	echo "Obtaining $NUM_EVENTS blocks of output from Jitter RNG"

	make -s -f Makefile.rng || fail "building jitterentropy-rng failed"

	local cmdopts="--max-mem $MAX_MEMORY_SIZE"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --force-internal-timer"
	fi

	# A failed or interrupted run leaves no file behind for the validation
	# to take as output.
	JENT_PARTIAL_FILE=$OUTDIR/$IID_DATA
	if ! signal_hold ./jitterentropy-rng $NUM_EVENTS $cmdopts > $OUTDIR/$IID_DATA
	then
		signal_release
		partial_remove
		fail "jitterentropy-rng failed"
	fi
	JENT_PARTIAL_FILE=
	signal_release

	make -s -f Makefile.rng clean
}

raw_entropy_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	hashtime_build

	local cmdopts="--max-mem $MAX_MEMORY_SIZE $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --force-internal-timer"
	fi

	hashtime_record $NUM_EVENTS_RESTART $NUM_RESTART $OUTDIR/$NONIID_RESTART_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}

raw_entropy()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	hashtime_build

	local cmdopts="--max-mem $MAX_MEMORY_SIZE $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --force-internal-timer"
	fi

	hashtime_record $NUM_EVENTS 1 $OUTDIR/$NONIID_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}


raw_entropy_ntg1_hash()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	hashtime_build

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --hashloop $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --force-internal-timer"
	fi

	hashtime_record $NUM_EVENTS 1 $OUTDIR/$NONIID_HASH_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}

raw_entropy_ntg1_hash_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	hashtime_build

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --hashloop $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --force-internal-timer"
	fi

	hashtime_record $NUM_EVENTS_RESTART $NUM_RESTART $OUTDIR/$NONIID_HASH_RESTART_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}

raw_entropy_ntg1_memacc()
{
	echo "Obtaining $NUM_EVENTS raw entropy measurement from Jitter RNG"

	hashtime_build

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --memaccess $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --force-internal-timer"
	fi

	hashtime_record $NUM_EVENTS 1 $OUTDIR/$NONIID_MEMLOOP_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}

raw_entropy_ntg1_memacc_restart()
{
	echo "Obtaining $NUM_RESTART raw entropy measurement with $NUM_EVENTS_RESTART restarts from Jitter RNG"

	hashtime_build

	local cmdopts="--max-mem $MAX_MEMORY_SIZE --memaccess $*"

	if [ -n "$FORCE_NOTIME_NOISE_SOURCE" ]
	then
		cmdopts="$cmdopts --force-internal-timer"
	fi

	hashtime_record $NUM_EVENTS_RESTART $NUM_RESTART $OUTDIR/$NONIID_MEMLOOP_RESTART_DATA $cmdopts

	make -s -f Makefile.hashtime clean
}
