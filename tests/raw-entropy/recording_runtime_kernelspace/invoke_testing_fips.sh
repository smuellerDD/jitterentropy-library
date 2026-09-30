#!/usr/bin/env bash

set -euxo pipefail

. ./invoke_testing_helper.sh

initialization
raw_entropy --force-fips
raw_entropy_restart --force-fips
