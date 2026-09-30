#!/usr/bin/env bash
#
# Process the entropy data

# Only sourced, by processdata*.sh: run by itself, its return statements would
# not stop it but fail and let it go on, and NONIID_DATA exported by anything
# else would pass the check below.
(return 0 2>/dev/null) || {
	echo "This script cannot be called by itself."
	exit 1
}

if [ -z "${NONIID_DATA:-}" ]
then
	echo "This script cannot be called by itself."
	exit 1
fi

############################################################
# Configuration values                                     #
############################################################

# location of log file
LOGFILE="$RESULTS_DIR/processdata.log"

# point to the min entropy tool
EATOOL=${EATOOL:-"../../SP800-90B_EntropyAssessment/cpp/ea_restart"}

# specify if you want to compile the extractlsb program in this script
BUILD_EXTRACT=${BUILD_EXTRACT:-"yes"}

# specify the list of significant bits and length that you want to analize.
# Indicate first the mask in hexa format and then the number of
# bits separated by a colon.
# The tool generates one data file, and the EA results, for each element.
# The mask can have a maximum of 8 bits on, the EA tool only manages samples
# up to one byte.

# List of masks usually analyzed (4 and 8 LSB)
#MASK_LIST="0F:4 FF:8"
MASK_LIST="FF:8"

# List used for ARM Cortext A9 and A7 processors
#MASK_LIST="FF:4,8 7F8:4,8"

# Maximum number of entries taken from each restart file
MAX_EVENTS=1000

############################################################
# Code only after this line -- do not change               #
############################################################

#############################
# Preparation
#############################

# The extraction and analysis tools are invoked in pipelines with tee for
# logging; without pipefail their failures would be masked by tee's exit code.
set -o pipefail

INPUTCONSOLIDATED="$RESULTS_DIR/jent-raw-noise-restart-consolidated.data"

EXTRACT="extractlsb"

if [ ! -d $ENTROPYDATA_DIR ]
then
	echo "Directory with raw entropy data $ENTROPYDATA_DIR is missing"
	exit 1
fi

if [ ! -d $RESULTS_DIR ]
then
	if ! mkdir $RESULTS_DIR
	then
		echo "Directory for results $RESULTS_DIR cannot be created"
		exit 1
	fi
fi

# The results are cleared below with rm -f $RESULTS_DIR/*.data: in the
# directory of the recordings that would delete them.
if [ "$(cd "$ENTROPYDATA_DIR" && pwd -P)" = "$(cd "$RESULTS_DIR" && pwd -P)" ]
then
	echo "ERROR: Results directory $RESULTS_DIR is the directory of the raw entropy data"
	exit 1
fi

if [ ! -f "$EATOOL" ]
then
	echo "ERROR: Path of Entropy Data tool $EATOOL is missing"
	exit 1
fi


# Evaluated at exit, not when set: processdata_ntg1.sh sources this once per
# set, and only the first builds extractlsb. A signal exits, so that the
# EXIT trap cleans up rather than the script going on.
trap 'if [ "${EXTRACT_BUILT:-}" = "yes" ]; then make clean; fi' 0
trap 'exit 1' 1 2 3 15

if [ "$BUILD_EXTRACT" = "yes" ]
then
	echo "Building $EXTRACT ..."
	make clean
	make
	EXTRACT_BUILT="yes"
else
	make
fi

if [ ! -x $EXTRACT ]
then
	echo "ERROR: Cannot execute $EXTRACT program"
	exit 1
fi

#############################
# Actual data processing
#############################
#
# Step 1: Concatenate all individual restart files into single file
#
# Also remove everything of a previous (possibly interrupted) run: extractlsb
# creates its output with O_EXCL and would fail on existing files.
#
# The cap applies per restart: on the consolidated file it would keep only
# the first restart, not the matrix ea_restart needs.
rm -f $RESULTS_DIR/*.txt $RESULTS_DIR/*.json $RESULTS_DIR/*.data \
      $RESULTS_DIR/*.log
#
# A set without recordings, or one whose extraction fails, is reported and
# left: processdata_ntg1.sh sources this once per set and goes on with the
# others, EA_FAILED making its status a failure. Sourced, return leaves only
# this set - with status 0, as set -e would end the caller on any other.
restarts=0
for i in $NONIID_DATA
do
	# the pattern itself when nothing matches
	[ -f "$i" ] || continue
	echo "Process recorded entropy data $i"

	head -n $MAX_EVENTS $i >> $INPUTCONSOLIDATED
	restarts=$((restarts + 1))
done

if [ "$restarts" -eq 0 ]
then
	echo "ERROR: No raw entropy data $NONIID_DATA" | tee -a $LOGFILE
	EA_FAILED=1
	return 0
fi

#
# Step 2: extract data
#
for file in $INPUTCONSOLIDATED
do
	filepath=$RESULTS_DIR/`basename ${file%%.data}`
	echo "Converting recorded entropy data $file into different bit output" | tee -a $LOGFILE

	for item in $MASK_LIST
	do
		mask=${item%:*}
		bits=${item#*:}

		if ! ./$EXTRACT $file $filepath.${mask}bitout.data $((MAX_EVENTS * restarts)) $mask 2>&1 | tee -a $LOGFILE
		then
			echo "ERROR: Extraction of $file (mask $mask) failed" | tee -a $LOGFILE
			EA_FAILED=1
			return 0
		fi
	done
done

#
# Step 3: Calculate SP800-90B
#
# Just like in step 2, we calculate the entropy column-wise
#

echo "" | tee -a $LOGFILE
echo "Extraction finished. Now analyzing entropy for noise source ..." | tee -a $LOGFILE
echo "" | tee -a $LOGFILE

for file in $INPUTCONSOLIDATED
do
	filepath=$RESULTS_DIR/`basename ${file%%.data}`

	for item in $MASK_LIST
	do
		mask=${item%:*}
		bits_field=${item#*:}
		bits_list=`echo $bits_field | sed -e "s/,/ /g"`

		infile=$filepath.${mask}bitout.data

		for bits in $bits_list
		do
			outfile=${filepath}.minentropy_${mask}_${bits}bits.txt
			echo "Analyzing entropy for $infile ${bits}-bit single" | tee -a $LOGFILE
			# The verdict also as JSON, next to the text.
			if ! $EATOOL -n -v -o ${outfile%.txt}.json $infile ${bits} 0.333 \
				> $outfile
			then
				# ea_restart also fails on a failed sanity or
				# validation test: keep its verdict, and go on with
				# the other sets of processdata_ntg1.sh.
				tail -n 5 $outfile | tee -a $LOGFILE
				echo "ERROR: Entropy analysis of $infile (${bits} bits) failed" | tee -a $LOGFILE
				EA_FAILED=1
			fi
		done
	done
done
