# The Jitter RNG in the FreeBSD kernel

`jitterentropy.h` and every `arch/` backend treat the FreeBSD kernel
(`_KERNEL` with `__FreeBSD__`) as a target of its own: memory from
`malloc(9)` and released with `zfree(9)`, `mp_ncpus` for the CPU count,
`kern_yield(9)` to yield, `arc4random_buf()` from libkern for the instance
identifier, and the counter instruction, or `get_cyclecount()` where there is
none, for the time stamps. This module is the build that keeps that true.

It compiles the library sources with the kernel's own flags into
`jitterentropy_test.ko`. Loading it runs the library once from the `MOD_LOAD`
handler: `jent_entropy_init()`, a collector from
`jent_entropy_collector_alloc()`, 64 bytes from `jent_read_entropy_safe()`,
the `jent_status()` document, and the collector freed again. Every step is
printed to the console with a `jitterentropy_test:` prefix, and the last line
is `jitterentropy_test: PASS`. Any failure is returned from `MOD_LOAD`, which
makes `kldload(8)` itself fail.

## Building and loading

The kernel sources for the running release have to be installed; the module
is built with BSD make, as every kernel module is:

    make SYSDIR=/usr/src/sys
    kldload ./jitterentropy_test.ko
    dmesg | grep jitterentropy_test
    kldunload jitterentropy_test

Where `/usr/src/sys` is missing, `src.txz` of the release provides it:

    fetch https://download.freebsd.org/releases/$(uname -m)/$(uname -p)/$(uname -r | sed 's/-p[0-9]*$//')/src.txz
    tar -C / -xf src.txz

`run-in-vm.sh` does all of that and is what the `freebsd-kmod` job in
`.github/workflows/bsd.yml` runs.

## Compiler flags

The entropy core - the same objects `linux_kernel/Kbuild.source` names - is
compiled at `-O0`, which `src/jitterentropy-base.c` insists on; the Makefile
appends it per object after `<bsd.kmod.mk>`, so it comes after the kernel's
`-O2`. The status and UUID formatting, the `arch/` backends and the module
are built optimized. The kernel's warning set is wider than the one the
library is developed against, so warnings are shown but not made errors.
