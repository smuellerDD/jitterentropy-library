#!/usr/bin/env bash

set -euxo pipefail

. ./invoke_testing_helper.sh

initialization
#lfsroutput
raw_entropy
raw_entropy_restart
