# Jitter RNG SP800-90B Entropy Analysis Tool

This archive contains the SP800-90B analysis tool to be used for the Jitter RNG.
The tool set consists of the following individual tools:

- `recording_kernelspace`: This tool is used to gather the raw entropy of
  the Linux kernel space Jitter RNG.

- `recording_userspace`: This tools is used to gather the raw entropy of
  the user space Jitter RNG implementation.

- `validation-runtime`: This tool is used to calculate the minimum entropy
  values compliant to SP800-90B section 3.1.3.

- `validation-restart`: This tool is used to calculate the minimum entropy
  values for the restart test compliant to SP800-90B section 3.1.4

See the README files in the different subdirectories.

# Author
Stephan Mueller <smueller@chronox.de>
