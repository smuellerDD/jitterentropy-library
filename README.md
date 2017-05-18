Hardware RNG based on CPU timing jitter
=======================================

The design of the RNG is given in the documentation found in doc/. The text
below references the section in the design spec where the functionality
implemented in the respective file is documented

API
---

jitterentropy.h -- The API functions that are intended to be used by normal
callers


Common files
------------
jitterentropy-base.c -- Jitter entropy collection -- file contains the heart
			of the CPU Jitter random number generator
			(design given in chapter 3)

jitterentropy-stat.c -- Implementation of the statistic validation of the data
			collected by the jitter entropy part (Note, that code
			is only needed for performing statistical tests
			when setting CONFIG_CRYPTO_CPU_JITTERENTROPY_STAT. Otherwise
			the entire code	is not compiled - the Makefile may
			even skip compiling this file when
			CONFIG_CRYPTO_CPU_JITTERENTROPY_STAT is not set).
			(functionality needed for graphs in chapter 4 and 5)


Linux Kernel files
-------------------

see Linux-kernel/ directory.


User space files
----------------

see openssl/ directory for OpenSSL related implementations

see libgcrypt/ directory for libgcrypt related implementations

jitterentropy-base-user.h -- User space helper functions needed for entropy
			     collection

jitterentropy-main-user.c -- main() function used to test user space code. It is
			     intended as a debug support and does not need to be
			     compiled if the entropy collection code is compiled
			     into a shared library. However, it is a working
			     application showing how to use the CPU Jitter RNG.

Makefile.user -- user space make file to generate an executable binary from
jitterentropy-main-user.c.

Makefile.shared -- generation of a shared library for the CPU Jitter RNG

Android
-------

android/Android.mk	-- NDK make file template that can be used to directly
			   compile the CPU Jitter RNG code into Android binaries

Direct CPU instructions
-----------------------

If the function in jent_get_nstime is not available, you can replace the
jitterentropy-base-user.h with examples from the arch/ directory.


Test
----

The test directories contain shell files that drive the testing.
