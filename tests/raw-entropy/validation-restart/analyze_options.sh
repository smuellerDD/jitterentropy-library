#!/usr/bin/env bash
#
# Validate the restart test results for various Jitter RNG memory settings
#
# This tool is only needed if you have insufficient entropy. See ../README.md
# for details
#
# It analyzes one measurement directory per memory size, recorded for the
# --max-mem values 1 (1 kB) to 20 (512 MB) of the recording tools with, e.g.:
#
#	cd ../recording_userspace
#	for n in $(seq 1 20); do
#		OUTDIR=../results-measurements-maxmem$n MAX_MEMORY_SIZE=$n \
#			./invoke_testing.sh || break
#	done
#
# Missing memory sizes are skipped. The table in ../results-restart-multi lists the memory
# size in powers of 2, i.e. the --max-mem value + 9.
#

set -euxo pipefail

RESULT="../results-restart-multi"
ENT_DIR="../results-measurements"
RES_DIR="../results-analysis-restart"
NUM_CPU=1
# "pid:target" of the running jobs, and the configurations that failed
JOBS=""
FAILED=""

# The --max-mem values with a measurement directory
MAXMEM=""
for maxmem in $(seq 1 20)
do
	if [ -d "$ENT_DIR-maxmem$maxmem" ]
	then
		MAXMEM="$MAXMEM $maxmem"
	fi
done

if [ -z "$MAXMEM" ]
then
	echo "ERROR: no measurement directory $ENT_DIR-maxmem<1..20> found, see $0 for how to record them" >&2
	exit 1
fi

trap "make clean" 0
trap "exit 1" 1 2 3 15
make clean
make

# Wait for every job and note each failed configuration.
reap() {
	for job in $JOBS
	do
		wait ${job%%:*} || FAILED="$FAILED ${job#*:}"
	done
	JOBS=""
}

crunch_numbers() {
	local source=$1
	local target=$2

	set -- $JOBS
	if [ $# -ge $NUM_CPU ]
	then
		reap
	fi

	# Every run recomputes: a result of earlier data must not be reported.
	rm -rf "$target"
	# processdata.sh leaves extractlsb, built above, alone.
	ENTROPYDATA_DIR="$source" RESULTS_DIR="$target" ./processdata.sh &
	JOBS="$JOBS $!:$target"
}

# min(H_r, H_c) of one configuration into ENT, noting a missing one as failed.
# Not the tool's min(H_r, H_c, H_I): the fixed H_I would hide all differences.
result() {
	ENT=$(awk -F': ' '/^H_r:/ { r = $2 } /^H_c:/ { c = $2 }
		END { if (r != "" && c != "") printf "%f", (r < c ? r : c) }' \
		"$1/jent-raw-noise-restart-consolidated.minentropy_FF_8bits.txt" 2>/dev/null) || true
	if [ -z "$ENT" ]
	then
		ENT="-"
		case " $FAILED " in
		*" $1 "*) ;;
		*) FAILED="$FAILED $1" ;;
		esac
	fi
}

calc() {
	local crunch=$1

	if [ "$crunch" -eq 0 ]
	then
		printf "Memory size in powers of 2\t%s\n" "min(H_r, H_c)" > "$RESULT"
	fi

	for maxmem in $MAXMEM
	do
		local target="$RES_DIR-maxmem$maxmem"
		local source="$ENT_DIR-maxmem$maxmem"

		if [ "$crunch" -eq 0 ]
		then
			result "$target"
			printf "%s\t%s\n" "$((maxmem + 9))" "$ENT" >> "$RESULT"
		else
			crunch_numbers "$source" "$target"
		fi
	done
}

# The online CPUs; /proc/cpuinfo only as a fallback, counting its "processor"
# lines (not the last one's number: s390x numbers differently, and a model
# name may contain the word).
ncpu=$(getconf _NPROCESSORS_ONLN 2>/dev/null) || true
if [ -z "$ncpu" ] && [ -f /proc/cpuinfo ]
then
	ncpu=$(grep -c '^processor' /proc/cpuinfo) || true
fi
case "$ncpu" in
	''|*[!0-9]*|0) ;;
	*) NUM_CPU=$ncpu ;;
esac

calc 1
reap
calc 0

if [ -n "$FAILED" ]
then
	for target in $FAILED
	do
		echo "ERROR: analysis of $target failed, see its processdata.log" >&2
	done
	exit 1
fi
