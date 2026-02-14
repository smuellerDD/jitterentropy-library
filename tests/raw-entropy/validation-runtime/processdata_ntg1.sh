#!/bin/sh
#
# Process the entropy data

############################################################
# Configuration values                                     #
############################################################

ENTROPYDATA_DIR=$1
RESULTS_DIR=$2

if [ -n "$RESULTS_DIR" ]
then
	BUILD_EXTRACT="no"
fi

NONIID_DATA="jent-raw-noise-0001.data jent-raw-noise_memaccloop-0001.data jent-raw-noise_hashloop-0001.data"

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh
