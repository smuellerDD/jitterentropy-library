# Jitter RNG SP800-90B Entropy Analysis Tool

This archive contains the SP800-90B analysis tool to be used for the Jitter RNG.
In addition, the tool set also supports the AIS 20/31 NTG.1 entropy rate
analysis. The tool set consists of the following individual tools:

- `recording_runtime_kernelspace`: This tool is used to gather the raw entropy
  of the Linux kernel space Jitter RNG for the SP800-90B runtime data.

- `recording_restart_kernelspace`: This tool is used to gather the raw entropy
  of the Linux kernel space Jitter RNG for the SP800-90B restart data.

- `recording_userspace`: This tools is used to gather the raw entropy of
  the user space Jitter RNG implementation for the SP800-90B runtime and
  restart data. Also, this directory contains tools supporting NTG.1 analysis.

- `validation-runtime`: This tool is used to calculate the minimum entropy
  values compliant to SP800-90B section 3.1.3. This tool tool is to be used
  with the user space and kernel space runtime data obtained from the
  aforementioned `recording_*` tools. Also, tools to perform the analysis
  for the NTG.1 compliance are provided.

- `validation-restart`: This tool is used to calculate the minimum entropy
  values for the restart test compliant to SP800-90B section 3.1.4. This tool
  is to be used with user space and kernel space restart data obtained from the
  aforementioned `recording_*` tools.

See the README files in the different subdirectories.

# Interpretation of Results

## Runtime Tests

The result of the data analysis performed with `validation-runtime` contains
in the file `jent-raw-noise-0001.minentropy_FF_8bits.var.txt` at the bottom data
like the following:

```
H_original: 2.387470
H_bitstring: 0.337104

min(H_original, 8 X H_bitstring): 2.387470
```

The last value gives you the entropy estimate per time delta. That means for one
time delta the given number of entropy in bits is collected on average.

Per default, the Jitter RNG heuristic applies 1/3 bit of entropy per
time delta. This implies that the measurement must show that *at least* 1/3 bit
of entropy is present. In the example above, the measurement shows that
2.3 bits of entropy is present which implies that the available amount of
entropy is more than what the Jitter RNG heuristic applies.

## Restart Tests

The results of the restart tests obtained with `validation-restart` contains
in the file `jent-raw-noise-restart-consolidated.minentropy_FF_8bits.var.txt`
at the bottom data like the following:

```
H_r: 0.545707
H_c: 1.363697
```

The `H_r` provides the entropy rate for the row-wise calculation, `H_c` for
the column-wise calculation - Ignore `H_I` in this output. To get to the actual
entropy rate, you have to obtain the heuristic entropy rate `H_I` applying 1/OSR
(common case) and 8/OSR (NTG.1).

Now, to get to the final entropy rate, calculate:

```
min(H_r, H_c, H_I)
```

to obtain the entropy rate for the restart tests.

Per default, the Jitter RNG heuristic applies 1/3 bit of entropy per
time delta (common case). This implies that the measurement must show that 1/3
bit of entropy is present. Unlike with the runtime tests, the restart tests
results compares the data against the Jitter RNG's H_I value of 1/3 bits.
Thus, the value must show 1/3 bits to show that sufficient entropy is
provided. In the example above, the measurement shows that
1/3 bits of entropy is present which implies that the available amount of
entropy is more than what the Jitter RNG heuristic applies.

# NTG.1 Considerations

When enabling the NTG.1 operational behavior, it is possible to incur
insufficient entropy. It is **strongly** advisable to perform the
[assessment for insufficient entropy](#approach-to-analyze-insufficient-entropy).
This is due to the fact that the health test is significantly more strict:

- 8/OSR bits of entropy are heuristically expected to be available.
This value must be achieved by the hash loop and memory access loop
**independently** of each other. For details on the independent operation of
the hash loop and memory access loop, see the design and architecture
specification of the Jitter RNG.

Note, when applying the analysis in the following, you have to compare the 
data with the heuristic entropy value of 8/OSR. 

# Approach to Analyze Insufficient Entropy

The Jitter RNG does not need any specific configurations or settings. However,
in case your entropy assessment shows that insufficient entropy is
present (e.g. by showing that the measured entropy rate is less than 1/3) or the
health test flags an error too often, you can perform a search whether different
configuration values gives better entropy.

## Basic Requirement For Entropy Rate

To be precise, the following requirements must be met - every time the following
refers to the "OSR" value, please consider [Oversampling Rate](#oversampling-rate): 

- In all cases: The runtime and restart entropy rates given by `processdata.sh`
  must show an entropy value that is larger than 1/OSR.
  
- In case of NTG.1 configuration: The runtime and restart entropy rates given by
  `processdata_ntg1.sh` for the common case, the hash loop and the memory access
  must show an entropy value that is larger than 8/OSR.

## Background - Jitter RNG Configuration Possibilities

The Jitter RNG has the following configuration options that allow altering the
entropy rate for the hash loop and memory access loop, as well as alter the
heuristically applied entropy rate.

### Oversampling Rate

The oversampling rate defines the heuristically applied entropy rate by the
Jitter RNG. Its value defines how much raw entropy data is collected to obtain
an output block of 256 bits that is expected to have full entropy.

In addition, Its value implicitly selects the health test cutoff values and thus
directly affects the probability that health errors are incurred.

The global heuristic entropy rate is defined as 1/OSR bits of entropy per
time delta obtained.

Runtime configuration: Use the `osr` parameter during initialization

Compile time configuration: Apply `-DJENT_MIN_OSR=<VALUE>` during compilation.

Default value: 3

Note: Runtime configuration value takes precedence over compile-time value.

### Memory Access Buffer Size

The buffer size defines the amount of memory allocated by the Jitter RNG to
measure the execution timing variation over memory accesses.

Rule of thumb: The larger the buffer, the higher the entropy rate.

Runtime configuration: Use the `JENT_MAX_MEMSIZE_*` flags during initialization.

Compile time configuration: Apply `-DJENT_DEFAULT_MEMORY_BITS=<VALUE>` during
compilation. Note, the memory size value is 2^JENT_DEFAULT_MEMORY_BITS.

Default value: 18 (resulting in 2^18 bytes)

Note: Runtime configuration value takes precedence over compile-time value.

Note: Compile-time value applied only if no L1-cache size is detected.

### Hash Loop Iteration Count

The iteration count defines the number of hash loop iterations executed to
measure CPU instruction timing variations.

Rule of thumb: The larger the iteration count, the higher the entropy rate.

Runtime configuration: Us the `JENT_HASHLOOP_*` flags during initialization.

Compile time configuration: Apply `-DJENT_HASH_LOOP_DEFAULT=<VALUE>` during
compilation.

Default value: 1

Note: Runtime configuration value takes precedence over compile-time value.

## Tool for Searching for More Entropy

Prerequisites:

- R-Project must be installed

- [SP800-90B Statistical Tool](https://github.com/usnistgov/SP800-90B_EntropyAssessment)
  must be installed and available pointed to by `EATOOL_NONIID` in
  `validation-runtime/processdata_helper.sh`.

It is possible that the the default setting of the Jitter RNG does not deliver
sufficient entropy. It is possible to adjust the memory access part of the
Jitter RNG which may deliver more entropy.

To support analysis of insufficient entropy, the following tools are provided.
The goal of those test tools is to detect the proper memory setting that is
appropriate for your environment. One memory setting consists of two values,
one for the number of memory blocks and one for the memory block size.

- `recording_userspace/invoke_testing_hashloop.sh`: This tool generates a large
  number of different test results for different settings for the hash loop
  operation. Simply execute the tool without any options. The tool creates
  test result data for each hash loop configuration option possible with the
  Jitter RNG.
  
- `recording_userspace/invoke_testing_memloop.sh`: This tool generates a large
  number of different test results for different settings for the memory access
  operation. Simply execute the tool without any options. The tool creates
  test result data for each memory buffer size configuration option possible
  with the Jitter RNG.

- `validation-runtime/processdata_hashloop.sh`: This tool analyzes all test
  results created by the `recording_userspace/invoke_testing_hashloop.sh` for
  the runtime data. It generates an overview file with all test results in
  `minentropy_collected_hashloop.txt` as well as it provides a graphical
  display of the entropy rates in `minentropy_collected_hashloop.pdf`. Analyze
  it and extract the hash loop iteration count that gives you the intended
  entropy rate.

- `validation-runtime/processdata_memloop.sh`: This tool analyzes all test
  results created by the `recording_userspace/invoke_testing_memloop.sh` for
  the runtime data. It generates an overview file with all test results in
  `minentropy_collected_memloop.txt` as well as it provides a graphical
  display of the entropy rates in `minentropy_collected_memloop.pdf`. Analyze
  it and extract the memory buffer size that gives you the intended entropy
  rate.

After you concluded the testing you have a memory buffer size and a hash loop
iteration count that should be appropriate for you. Note, it is perfectly fine
if only one, i.e. the hash loop count or the memory buffer size needs adjustment
to give you the intended entropy rate as the respective other already offer
sufficient entropy.

Once you found the suitable memory setting, invoke the Jitter RNG with the
respective option `JENT_MAX_MEMSIZE_*` as flag in `jent_entropy_init_ex` and
`jent_entropy_collector_alloc`.

Once you found the suitable hash loop setting, invoke the Jitter RNG with the
respective option `JENT_HASHLOOP_*` as flag in `jent_entropy_init_ex` and
`jent_entropy_collector_alloc`.

# Author
Stephan Mueller <smueller@chronox.de>
