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
from scratch (and remove it again on exit); with "no" it is not built at all
and has to be built with `make` beforehand. The default is "yes";
processdata.sh sets "no" when its caller gives RESULTS_DIR, which is then
expected to have built extractlsb itself, and processdata_ntg1.sh builds it
for its first set only.

MASK_LIST: Indicates the extraction method from each sample item. You can
indicate one or more methods; the script will generate one bit stream data
file for each extraction method. See `../validation-runtime/README.md` for a
more detailed explanation.

MAX_EVENTS: the number of samples taken from each restart file. The script
concatenates the first MAX_EVENTS samples of every restart file into one file
and extracts MAX_EVENTS times the number of restarts from it - with the
default of 1000 and 1000 restarts the 1000 x 1000 matrix ea_restart expects.
Without any restart file the script fails.

### Parameters of processdata.sh

ENTROPYDATA_DIR: Location of the sample data files (with .data extension)

RESULTS_DIR: Location for the interim data bit streams and results.
processdata_ntg1.sh stores its three sets in `../results-analysis-restart`,
`../results-analysis-hashloop-restart` and
`../results-analysis-memaccloop-restart`.

processdata.sh takes both from the environment. A result file already present
in RESULTS_DIR is kept rather than computed again, so remove it to repeat an
analysis.

[1] https://github.com/usnistgov/SP800-90B_EntropyAssessment
