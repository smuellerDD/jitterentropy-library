# Validation of Raw Entropy Data

This tool is used to calculate the minimum entropy values
compliant to SP800-90B for the gathered data.

The validation operation of the raw entropy data is invoked with the
processdata.sh script.

The sampling data from the recording operation must be post-processed. This is
because you need to determine which part of the sample elements contain the
high-resolution part of the time delta provided by the Jitter RNG. Usually,
this occurs in the 4 to 8 least significant bits of the sample element, but it
may vary depending on the processor.

The first step is performed by the extractlsb program, which reads the input
data, extracts the significant bits from each sample item (using a mask that
you provide) and writes them as one byte per sample into a binary data stream
file, one file for each extraction method.

In the second step, the binary data streams are processed with the SP800-90B
entropy assessment tool, which calculates the min entropy.

The resulting minimum entropy for each data stream is provided in the
`*.minentropy_<mask>_<bits>bits.txt` file. The log file contains a summary of the steps performed and
the output of the extractlsb program.


## Prerequisites

To execute the testing, you need:

	* NIST SP800-90B tool from:
		https://github.com/usnistgov/SP800-90B_EntropyAssessment

	* Obtain the sample data recorded on the target platforms

	* Configure processdata.sh with proper parameter values


### Parameters of processdata.sh

ENTROPYDATA_DIR: Location of the sample data files (with .data extension)

RESULTS_DIR: Location for the interim data bit streams and results.

Both are passed as the first and second argument, e.g.
`./processdata.sh "" ../results-analysis-runtime-mine` for the default data
location. Results of a previous run in RESULTS_DIR are removed.

LOGFILE: Name of the log file. The default is $RESULTS_DIR/processdata.log.

EATOOL_NONIID: Path of the non-IID program of the Entropy Assessment tool
(usually, ea_non_iid).

BUILD_EXTRACT: Indicates whether the script will rebuild the extractlsb program
from scratch. The default is "yes", and "no" when RESULTS_DIR is given.

MASK_LIST: Indicates the extraction method from each sample item. You can
indicate one or more methods; the script will generate one bit stream data
file for each extraction method. See below for a more
detailed explanation.

MAX_EVENTS: the size of the sample that will be extracted from the sample data.
It is taken from the environment, else from `NUM_EVENTS` as used for the
recording (see `recording_userspace/README.md`), else it is 1000000, the
minimum suggested by SP800-90B. A data file holding fewer samples fails the
extraction.


### Extraction method

Although the samples elements collected from the JitterRNG (time deltas) have
a size 64 bits, the entropy will likely be provided on the high resolution
part, which is usually the least significant bits.

Also, the SP800-90B entropy assessment tool has two limitations:

	* although the SP800-90B standard allows alphabets of any size, the
	  script only processes alphabets up to 8 bits.

	* the tool expects each sample to be contained in one byte.
  	  Therefore, if you want to use only the 4 LSB of each sample item,
	  you'll need to generate one byte per each 4-bit symbol.

You need to analyze the sample and determine which bit positions provide
the high-resolution time deltas that will feed the entropy analysis.

Usually, you can assume that the high-resolution time delta is represented
in the least significant 4 or 8 bits. So you specify the MASK_LIST parameter
as follows:

	MASK_LIST="0F:4 FF:8"

This parameter value means the following:

	* There will be two extractions methods for each data set.

	* The first method takes the 4 LSB from each sample item, generates
	  one byte per sample item, and uses a 4-bit alphabet for analyzing
	  entropy.

	* The second method takes the 8 LSB from each sample item, generates
	  one byte per item, and uses an 8-bit alphabet for analyzing entropy.

The parameter is a list of extraction methods. Each extraction method is
represented by two fields separated by a colon:

	* A mask in hexadecimal format, that indicates which bits are
	  significant. If the bit is on in a given position, this bit will be
	  considered to form the final byte. Otherwise, it'll be discarded.
	  For instance, a mask of "F8" means that the 3 LSB will be discarded,
	  and the bits 4 to 8 will be used; a mask of "7F8", means that the
	  3 LSB will be discarded, and bits 4 to 10 will be used. The bit
	  values are shifted so all bit positions in the final byte are used.

	* The second value is the alphabet size in bits. This is redundant but
	  required to avoid complex calculations in bash.

You can start with the default values, and then refine your extraction methods
if necessary. You may find that the LSB of the samples do not provide entropy,
then use a different mask to discard them.

The output of the extractlsb program can be used to detect this scenarios. The
program shows a summary of the extraction, indicating the bit positions that
have not changed in any of the samples. For example, this is the output of
the extraction program.

Processed 1000000 items from ../results-measurements/jent-raw-noise-0001.data samples with mask [0x00000000000000ff] significant bits [8]
Constant 0s in sample: 
00000000 00000000 00000000 00000000 --000-00 0---0--- -------- -----000 
Constant 1s in sample: 
-------- -------- -------- -------- -------- -------- -------- -------- 

You can see here that the 3 LSB of the samples always show zeroes (the minus
means that this position is variable). There you can easily determine which
positions you should consider. In this particular case, you should use a mask
of "78:4" and "7F8:8" to analyze the samples with 4-bit and 8-bit alphabets,
discarding the 3 LSB.


## Conclusion

The conclusion you have to draw is the following: To generate a 256 bit block,
the Jitter RNG obtains 256 * OSR time deltas ((256 + 65) * OSR in FIPS mode -
`JENT_FORCE_FIPS` or a system in FIPS mode - and with `JENT_NTG1`, both rounded
up to a multiple of three), i.e.
it heuristically credits each time delta with 1/OSR bits of entropy. So, if you
obtain a result that the minimum entropy per time delta is more than 1/OSR
bits, the 256 bit output block is believed to have (close to) full entropy.
Otherwise it will have relatively less entropy. See `../README.md` for how to
read the results and for the requirements of NTG.1.

Please note that the minimum collision entropy value for 8 bits may be smaller
than the 4 bit values due to the inclusion of leading zeros. This, however is
a data processing problem that should be considered when drawing conclusions.
One can see the effect of these leading zeros by compressing the 4 bit and
8 bit data streams. Whereas the 4 bit data stream may not be compressable,
the 8 bit data stream may be compressed

This may also occur when the least significant bits in the time delta do not
change.  You need to refine the extraction method to reach to the right
calculation.


# Example Assessment of Results

The Jitter RNG measures the execution time of its hash loop and memory access
loop, and each time delta is inserted into its SHAKE256 based entropy pool
together with other intermediary data; only the time delta is considered to
contain entropy. `processdata.sh` analyzes exactly these time deltas, and its
result ends with, e.g.:

```
H_original: 2.387470
H_bitstring: 0.337104

min(H_original, 8 X H_bitstring): 2.387470
```

The last line is the key: with the default mask `FF:8` it is the minimum
entropy of one time delta, assuming that the bits above the 8 LSB contribute
no entropy. With the default OSR of 3, the Jitter RNG credits each time delta
with 1/3 bit, so this example provides about 2.39 / (1/3) = 7 times the
entropy the Jitter RNG assumes. As the 256 bit output block is derived from
256 * 3 of these time deltas, it is believed to have full entropy.

Note, applying the Shannon-Entropy formula to the data, we will get much
higher entropy values.
