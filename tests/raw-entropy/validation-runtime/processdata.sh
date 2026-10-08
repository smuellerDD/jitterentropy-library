#!/usr/bin/env bash
#
# Process the entropy data

############################################################
# Configuration values                                     #
############################################################

# The first and second argument, or the environment; processdata_helper.sh
# supplies the defaults.
ENTROPYDATA_DIR=${1:-$ENTROPYDATA_DIR}
RESULTS_DIR=${2:-$RESULTS_DIR}

if [ -n "$RESULTS_DIR" ]
then
	BUILD_EXTRACT="no"
fi

NONIID_DATA="jent-raw-noise-0001.data"

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh
