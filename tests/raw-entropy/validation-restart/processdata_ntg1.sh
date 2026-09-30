#!/usr/bin/env bash
#
# Process the entropy data

set -euxo pipefail

############################################################
# Configuration values common                              #
############################################################

# point to the directory that contains the results from the entropy collection
ENTROPYDATA_DIR=${ENTROPYDATA_DIR:-"../results-measurements"}

# this is where the resulting data and the entropy analysis will be stored,
# the hash loop and memory access sets go to directories beside it
if [ -n "${RESULTS_DIR:-}" ]
then
	# given by the caller: leave extractlsb alone, as processdata.sh does
	BUILD_EXTRACT="no"
	HASHLOOP_RESULTS_DIR="$RESULTS_DIR-hashloop"
	MEMACCLOOP_RESULTS_DIR="$RESULTS_DIR-memaccloop"
else
	RESULTS_DIR="../results-analysis-restart"
	HASHLOOP_RESULTS_DIR="../results-analysis-hashloop-restart"
	MEMACCLOOP_RESULTS_DIR="../results-analysis-memaccloop-restart"
fi

NONIID_DATA="$ENTROPYDATA_DIR/jent-raw-noise-restart*.data"

# set by processdata_helper.sh when a set fails: it has no recordings, or
# their extraction or analysis fails. The other sets are processed regardless.
EA_FAILED=0

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh

############################################################
# Configuration values hash loop                           #
############################################################

RESULTS_DIR="$HASHLOOP_RESULTS_DIR"

BUILD_EXTRACT="no"

NONIID_DATA="$ENTROPYDATA_DIR/jent-raw-noise-hashloop-restart*.data"

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh

############################################################
# Configuration values memory access loop                  #
############################################################

RESULTS_DIR="$MEMACCLOOP_RESULTS_DIR"

BUILD_EXTRACT="no"

NONIID_DATA="$ENTROPYDATA_DIR/jent-raw-noise-memaccloop-restart*.data"

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh

# The status over all sets: a failed one goes on to the next.
[ "$EA_FAILED" != 1 ]
