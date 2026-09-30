# Validation of Raw Entropy Data Restart Test

This validation tool processes the restart raw entropy data compliant to
SP800-90B section 3.1.4.

Each restart must be recorded in a single file where each raw entropy
value is stored on one line.

## Prerequisites

To execute the testing, you need:

	* NIST SP800-90B tool from:
		https://github.com/usnistgov/SP800-90B_EntropyAssessment

	* Obtain the sample data recorded on the target platforms

	* Configure processdata.sh with proper parameter values

### Executation

Use one of the following tools:

* Common case: `processdata.sh` obtains the entropy rate for the restart test
  data obtained with `invoke_testing.sh` or `invoke_testing_fips.sh`.
  
* NTG.1 case: `processdata_ntg1.sh` obtains the entropy rate for the restart
  test data obtained with `invoke_testing_ntg1.sh`.

## Conclusion

The conclusion you have to draw is the following: To generate a 256 bit block,
the Jitter RNG obtains 256 time deltas (one time delta per bit at least, unless
the Jitter RNG performs oversampling). So, if you obtain a result that the
minimum entropy is more than 1/OSR (common case) or 8/OSR (NTG.1 case) bits
of entropy (per time delta), the one Jitter RNG output data block is believed
to have (close to) full entropy. Otherwise it will have relatively less entropy.

### Parameters of processdata_helper.sh

LOGFILE: Name of the log file. The default is $RESULTS_DIR/processdata.log.

EATOOL: Path of the program used from the Entropy Assessment restart tool
(usually, ea_restart).

BUILD_EXTRACT: Indicates whether the script will rebuild the extractlsb program
from scratch (and remove it again on exit). The default is "yes", and "no"
when RESULTS_DIR is given: extractlsb is then only built when missing or
outdated.

MASK_LIST: Indicates the extraction method from each sample item. You can
indicate one or more methods; the script will generate one bit stream data
file for each extraction method. See `../validation-runtime/README.md` for a
more detailed explanation.

MAX_EVENTS: the number of samples taken from each restart file. The default is
1000, which with the 1000 restarts recorded gives the 1000 x 1000 matrix
required by SP800-90B section 3.1.4.

### Parameters of processdata.sh

ENTROPYDATA_DIR: Location of the sample data files (with .data extension)

RESULTS_DIR: Location for the interim data bit streams and results.
processdata_ntg1.sh stores the hash loop and memory access
results in `$RESULTS_DIR-hashloop` and `$RESULTS_DIR-memaccloop`.

Both are taken from the environment. Results of a previous run in RESULTS_DIR
are removed.

[1] https://github.com/usnistgov/SP800-90B_EntropyAssessment
