#!/bin/bash
#
# Process the entropy data

############################################################
# Configuration values                                     #
############################################################

NONIID_DATA="$(for i in ../results-measurements/*; do basename $i; done)"

############################################################
# Code only after this line -- do not change               #
############################################################

. ./processdata_helper.sh
