#!/usr/bin/env bash
#
# Process the entropy data

############################################################
# Configuration values                                     #
############################################################

# point to the directory that contains the results from the entropy collection
ENTROPYDATA_DIR=${ENTROPYDATA_DIR:-"../results-measurements"}

# this is where the resulting data and the entropy analysis will be stored;
# with it given by the caller (analyze_options.sh), which builds extractlsb
# itself, leave extractlsb alone
if [ -n "$RESULTS_DIR" ]
then
	BUILD_EXTRACT="no"
fi
RESULTS_DIR=${RESULTS_DIR:-"../results-analysis-restart"}

NONIID_DATA="$ENTROPYDATA_DIR/jent-raw-noise-restart*.data"

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh
