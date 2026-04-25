#!/bin/sh
#
# Process the entropy data

############################################################
# Configuration values common                              #
############################################################

# point to the directory that contains the results from the entropy collection
ENTROPYDATA_DIR=${ENTROPYDATA_DIR:-"../results-measurements"}

# this is where the resulting data and the entropy analysis will be stored
RESULTS_DIR="../results-analysis-restart"

NONIID_DATA="$ENTROPYDATA_DIR/jent-raw-noise-restart*.data"

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh

############################################################
# Configuration values hash loop                           #
############################################################

# this is where the resulting data and the entropy analysis will be stored
RESULTS_DIR="../results-analysis-hashloop-restart"

BUILD_EXTRACT="no"

NONIID_DATA="$ENTROPYDATA_DIR/jent-raw-noise-hashloop-restart*.data"

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh

############################################################
# Configuration values memory access loop                  #
############################################################

# this is where the resulting data and the entropy analysis will be stored
RESULTS_DIR="../results-analysis-memaccloop-restart"

BUILD_EXTRACT="no"

NONIID_DATA="$ENTROPYDATA_DIR/jent-raw-noise-memaccloop-restart*.data"

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh
