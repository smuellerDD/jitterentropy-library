#!/bin/sh

. ./invoke_testing_helper.sh

initialization
#lfsroutput
raw_entropy --force-fips
raw_entropy_restart --force-fips
