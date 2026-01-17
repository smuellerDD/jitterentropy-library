#!/bin/sh

. ./invoke_testing_helper.sh

initialization
#lfsroutput
raw_entropy --ntg1
raw_entropy_restart --ntg1
raw_entropy_ntg1_hash --ntg1
raw_entropy_ntg1_memacc --ntg1
