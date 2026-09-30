#!/usr/bin/env bash
#
# Process the entropy data

# Only sourced, by processdata*.sh, which set its configuration: the check of
# NONIID_DATA below would take one exported by anything else for it.
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

# point to the directory that contains the results from the entropy collection
ENTROPYDATA_DIR=${ENTROPYDATA_DIR:-"../results-measurements"}

# this is where the resulting data and the entropy analysis will be stored
RESULTS_DIR=${RESULTS_DIR:-"../results-analysis-runtime"}

# location of log file
LOGFILE="$RESULTS_DIR/processdata.log"

# point to the min entropy tool
EATOOL_NONIID=${EATOOL_NONIID:-"../../SP800-90B_EntropyAssessment/cpp/ea_non_iid"}

# specify if you want to compile the extractlsb program in this script
BUILD_EXTRACT=${BUILD_EXTRACT:-"yes"}

# specify the list of significant bits and length that you want to analize.
# Indicate first the mask in hexa format and then the number of
# bits separated by a colon.
# The tool generates one data file, and the EA results, for each element.
# The mask can have a maximum of 8 bits on, the EA tool only manages samples
# up to one byte.

# List of masks usually analyzed (4 and 8 LSB)
MASK_LIST="FF:8"
#MASK_LIST="0F:4 FF:8"

# List used for ARM Cortext A9 and A7 processors
#MASK_LIST="FF:4,8 7F8:4,8"

# Number of entries to be extracted from each original file: a recording
# made with a NUM_EVENTS override (see recording_userspace/README.md) is
# analyzed with the same NUM_EVENTS. extractlsb fails on a file with fewer.
MAX_EVENTS=${MAX_EVENTS:-${NUM_EVENTS:-1000000}}

############################################################
# Code only after this line -- do not change               #
############################################################

# The extraction and analysis tools are invoked in pipelines with tee for
# logging; without pipefail their failures would be masked by tee's exit code.
set -o pipefail

EXTRACT=${EXTRACT:-"./extractlsb"}

if [ ! -d $ENTROPYDATA_DIR ]
then
	echo "ERROR: Directory with raw entropy data $ENTROPYDATA_DIR is missing"
	exit 1
fi

if [ ! -d $RESULTS_DIR ]
then
	if ! mkdir $RESULTS_DIR
	then
		echo "ERROR: Directory with raw entropy data $RESULTS_DIR could not be created"
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

if [ ! -f "$EATOOL_NONIID" ]
then
	echo "ERROR: Path of Entropy Data tool $EATOOL_NONIID is missing"
	exit 1
fi


rm -f $RESULTS_DIR/*.txt $RESULTS_DIR/*.data  $RESULTS_DIR/*.log

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

for file in $NONIID_DATA
do
	file="$ENTROPYDATA_DIR/$file"
	filepath=$RESULTS_DIR/`basename ${file%%.data}`
	echo "Converting recorded entropy data $file into different bit output" | tee -a $LOGFILE

	for item in $MASK_LIST
	do
		mask=${item%:*}
		bits=${item#*:}

		if ! $EXTRACT $file $filepath.${mask}bitout.data "$MAX_EVENTS" $mask 2>&1 | tee -a $LOGFILE
		then
			echo "ERROR: Extraction of $file (mask $mask) failed" | tee -a $LOGFILE
			exit 1
		fi

	done
done

echo "" | tee -a $LOGFILE
echo "Extraction finished. Now analyzing entropy for noise source ..." | tee -a $LOGFILE
echo "" | tee -a $LOGFILE

for file in $NONIID_DATA
do
	file="$ENTROPYDATA_DIR/$file"
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
			echo "Analyzing entropy for $infile ${bits}-bit" | tee -a $LOGFILE
			if ! $EATOOL_NONIID -i -a -v $infile ${bits} > $outfile
			then
				# keep what the tool said, the results are cleared
				# above on every run
				tail -n 5 $outfile | tee -a $LOGFILE
				echo "ERROR: Entropy analysis of $infile (${bits} bits) failed" | tee -a $LOGFILE
				exit 1
			fi
		done
	done
done
