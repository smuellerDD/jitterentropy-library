# The Jitter RNG with no operating system

The README of this project says the Jitter RNG "could even run on baremetal
without any operating system". Nothing tested that. Every other program in this
tree runs on Linux, Windows, a BSD, macOS or inside the Linux kernel, and each
of those hands the `arch/` backends an allocator, a clock call, a CPU count, a
cache geometry and a random pool to draw an identifier from.

`jitterentropy-efi.c` is that claim, executed. It is an EFI application: it
boots on firmware, before an operating system exists, with one processor, no
scheduler, no threads, no libc, no `/proc` and no CSPRNG. It initializes the
library and then puts three configurations - the default, `JENT_FORCE_FIPS` and
`JENT_NTG1` - through the same sequence: allocate a collector, generate 32
bytes, print them, print the `jent_status()` document. Then it powers the
machine off.

## Why an application and not a compile test

`flake.nix` already cross-compiles the library for a dozen targets, which
proves the backends select and the sources translate. It cannot prove that the
counter the freestanding path reads actually moves, that the startup health
tests pass on what that counter measures, or that a collector can be built
where the only allocator is the firmware's `AllocatePool()`. Those are
properties of running.

The failure this guards against is not a build error. It is a Jitter RNG that
comes up on such a target, reports success, and hands out something it never
measured.

## What selects the freestanding path

`-ffreestanding`, and nothing else. Both GCC and Clang set `__STDC_HOSTED__` to
zero for it, which is what the C standard defines that macro to mean, and
`jitterentropy.h` defines `JENT_BAREMETAL` from it. Every `arch/` backend then
takes its OS-less branch.

That indirection is needed because a freestanding target is normally built by
the host's own compiler, which still announces the host: this application is
compiled on Linux and `__linux__` and `__unix__` are defined throughout it.
Without `JENT_BAREMETAL` the backends would reach for `mmap()`, `mlock()`,
`sysconf()`, `sched_getaffinity()` and `getrandom()` on a machine that has none
of them. `JENT_BAREMETAL` can also be defined directly, for a toolchain that
does not report freestanding or to force the port on a hosted one.

## The porting interface

Six functions, and that is the whole of it - the ones a freestanding C
implementation does not provide and the compiler may emit calls to regardless:

| Function | Supplied by |
| --- | --- |
| `memcpy`, `memset` | gnu-efi |
| `malloc`, `free` | this application, over `AllocatePool()` / `FreePool()` |
| `strlen` | this application |
| `snprintf` | this application, for `jent_status()` and nothing else |

The `snprintf()` here implements only the conversions `jent_status()` uses -
`%s`, `%d`, `%u`, `%ld`, `%llu` - and copies anything else through verbatim, so
a conversion added to that source shows up in the output as itself rather than
disappearing.

## What the output means

```
jitterentropy-efi: start, library version 3070100
jitterentropy-efi: startup passed
jitterentropy-efi: default collector allocated
jitterentropy-efi: default entropy c7d00293f4319cdb936828498734704393f6b4389666c0081c6dc42e4c222cbb
jitterentropy-efi: default status
{ ... }
jitterentropy-efi: FIPS collector allocated
jitterentropy-efi: FIPS entropy 8ec51b6ceeee25de963f7ac43a0683fc7a8439ab12229e0542abb82a5bc5f950
jitterentropy-efi: FIPS status
{ ... }
jitterentropy-efi: NTG.1 collector allocated
jitterentropy-efi: NTG.1 entropy 8015d24c8ac096df9a0361b3a30f345d175880b86828eec2b46f984c70a0cc65
jitterentropy-efi: NTG.1 status
{ ... }
jitterentropy-efi: internal timer refused, the startup reporting 1
jitterentropy-efi: done
```

`startup passed` is the substantive line: it means the platform counter moved,
that a common divisor for the deltas was established, that the conditioning
known answer tests passed, and that the startup health tests did not fire on
what was measured. `done` means every configuration completed and every
collector was released.

The three status documents differ only in what the library made of the flags -
`"fipsMode"` and `"ntg1Mode"` are set in the ones that asked for them - which
is what makes them worth printing all three: it is the same code path reached
three ways with no operating system under it.

Two things in the status document are properties of having no operating system
rather than defects, and the VM check asserts both so that they stay stated:

* `"uuid": "00000000-..."` - the instance identifier is drawn from the
  platform CSPRNG, and there is none. The library says so rather than inventing
  one.
* `"internalTimer": false` - the counting thread needs a thread. Asking for it
  anyway, with `JENT_FORCE_INTERNAL_TIMER`, has to be *refused*, and the last
  line of the run checks that it is on both entry points that accept the flag.
  An allocation that took it would hand back a collector whose clock is a
  counter nothing increments, and the first measurement would spin rather than
  return an error - which on a target with no scheduler is a hang with nothing
  left to break it.

`"cpuCores": 1` is the same kind of thing: an EFI application runs on the boot
processor with the others parked, and there is nothing to ask how many there
are.

The cache geometry is not. On x86_64 it comes out of `CPUID`, which needs no
operating system, so the sizes there are real and the VM check asserts they are
non-zero - a zero would mean the one platform query this build still makes had
stopped working. That is also why the check runs QEMU with `-cpu max`: the
default model for the board reports no cache descriptors at all, and a check
against it would be asserting a property of the emulator rather than of the
library. On aarch64 there is no `CPUID` and the cache size registers are read
through the kernel's own accessors in the one backend that reads them, so this
build makes no cache query and takes the library's default memory size instead.

## The compliance modes

Only the default configuration must succeed. `JENT_FORCE_FIPS` and `JENT_NTG1`
are attempted and whatever comes back is reported:

```
jitterentropy-efi: NTG.1 refused, which its health test cutoffs allow
```

Both carry tighter health test cutoffs than the common configuration - NTG.1
markedly so - and a startup firing on what a particular firmware and processor
produce is a legitimate outcome rather than a defect. So the VM check asserts
that each was attempted and answered, not which way it went; where one does
come up, it holds it to the same 32 bytes and status document as the default.

Both imply `JENT_FORCE_SECURE_MEM`, and here that is satisfied rather than
waived: `"secureMemory": true` above is not a locked page but the absence of
anything that could page it out - no swap device, no second process, no core
dump - which is the same ground the Linux kernel backend claims it on.

**Neither line is a compliance claim**: FIPS 140-3 and AIS 20/31 are about a
validated module on assessed hardware, and what this shows is that the code
paths those modes take are reachable with no operating system under them.

**This is a demonstration that the library runs, not an entropy assessment.**
Nothing here says how much entropy a given firmware and processor produce. That
question is what `tests/raw-entropy` and an SP800-90B analysis are for, and it
has to be answered on the hardware in question.

## Building and running it

Through Nix:

```
nix build .#efi                              # the EFI system partition
nix build .#checks.x86_64-linux.efi-vm       # build it and boot it under OVMF
```

The check writes the console transcript to `result/console.txt`. CI runs that
one on every push; see below for why it does not run the aarch64 one.

By hand, with gnu-efi installed:

```
make -C tests/efi GNUEFI=/usr esp
qemu-system-x86_64 -machine q35 -m 512 -nographic -no-reboot \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/path/to/OVMF_CODE.fd \
  -drive if=pflash,format=raw,unit=1,file=OVMF_VARS.fd \
  -drive format=raw,file=fat:rw:tests/efi/esp \
  -serial mon:stdio
```

`esp/EFI/BOOT/BOOTX64.EFI` is the removable-media default path, so the firmware
boots it with no boot entry configured - and it is also what to copy onto a USB
stick to run this on real hardware, which is the more interesting machine to
run it on.

## Both architectures

x86_64 and aarch64, which is where the firmware in the field is. The
application is the same source; what differs is the toolchain and how the image
is put together, and the Makefile says so at each point:

| | x86_64 | aarch64 |
| --- | --- | --- |
| Image | `BOOTX64.EFI` | `BOOTAA64.EFI` |
| Calling convention | Microsoft, so `-DGNU_EFI_USE_MS_ABI` and (on GCC) `-maccumulate-outgoing-args` | the compiler's own |
| Red zone | disabled: the firmware takes interrupts on the same stack | none to disable |
| PE conversion | `objcopy --output-target pei-x86-64` | `objcopy --output-target pei-aarch64-little` |
| Page size | the linker default | `-z common-page-size=4096 -z max-page-size=4096`, the granularity the firmware maps at |
| Atomics | inline | `-mno-outline-atomics`, see below |

gnu-efi's own build rules fall back to a flat `objcopy -O binary` for aarch64,
relying on their startup code carrying a hand-written PE header, wherever
`objcopy` has no PE target for the architecture. binutils does have one -
`pei-aarch64-little` - and the fallback does not produce an image this firmware
will load at all, so the real thing is written instead.

`-mno-outline-atomics` is the other one that is not optional, and it is a fact
about porting this library to aarch64 rather than about EFI. GCC 10 and later
default to `-moutline-atomics` there, which turns an atomic access into a call
to a libgcc helper - `__aarch64_swp4_acq_rel` for the one read-modify-write in
`arch/jitterentropy-arch-atomic.c` - that selects the LSE or the LL/SC
implementation at run time through a libgcc ifunc. A `-nostdlib` link has no
libgcc, so the symbol stays undefined and the first
`jent_atomic_exchange_int()` of the startup jumps into nothing.

What that looks like is worth recording, because the console says almost
nothing: one line, `Synchronous Exception at 0x0000000000013780`, and no
register dump - ArmPkg prints those through `DEBUG()`, which a release build of
AAVMF does not put on this console. `qemu-system-aarch64 -d int` supplies what
is missing:

```
Taking exception 1 [Undefined Instruction] on CPU 0
...from EL1 to EL1
...with ESR 0x0/0x2000000
...with ELR 0x13780
```

ESR exception class 0 is "unknown reason", which is what executing a word of
zeroes gives, and the ELR is a *link-time* address rather than a loaded one.
`readelf -r` then names it outright: a single `R_AARCH64_JUMP_SLOT` against an
undefined `__aarch64_swp4_acq_rel`. With the flag the compiler emits the
instructions inline instead, as it does for the kernel, which uses it for the
same reason.

Both are built and booted by `nix flake check`. Only x86_64 runs in CI: the
aarch64 image is cross-built, and a runner with no aarch64 toolchain in its
cache spends about an hour building one before it reaches the seconds of
booting that are the point.

By hand, cross-building aarch64 needs gnu-efi for that architecture and a cross
toolchain:

```
make -C tests/efi EFI_ARCH=aarch64 GNUEFI=/path/to/aarch64/gnu-efi \
  CROSS_COMPILE=aarch64-linux-gnu- esp
qemu-system-aarch64 -machine virt -cpu max -m 512 -nographic -no-reboot \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=AAVMF_CODE.fd \
  -drive if=pflash,format=raw,unit=1,file=AAVMF_VARS.fd \
  -drive format=raw,file=fat:rw:tests/efi/esp
```

The two images have different default names, so one EFI system partition can
hold both and boot on either machine.
