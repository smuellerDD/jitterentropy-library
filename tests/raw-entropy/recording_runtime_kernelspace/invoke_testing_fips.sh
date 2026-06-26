#!/bin/sh

. ./invoke_testing_helper.sh

initialization
raw_entropy --force-fips
raw_entropy_restart --force-fips
