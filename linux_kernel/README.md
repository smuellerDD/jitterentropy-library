# Linux Kernel Support

The Jitter RNG is intended to be used in any kind of execution environment
provided that it has a high-resolution timer. This applies also to the Linux
kernel.

The vanilla Linux kernel already contains a copy of the Jitter RNG. But it is
an older version since the update of the Linux kernel is a very lengthy and
tedious effort, including politics.

That said, the Jitter RNG code distribution offers the means to compile the
Linux kernel support. This compilation uses the unchanged current Jitter RNG
code base and compiles it for the Linux kernel. There are several ways how
to compile it as outlined in the following sections. Select the compile option
that fits your purpose.

All of the following discussions assume you are in the directory `linux_kernel`.

For each compilation, the file `Kbuild.config` contains configuration options
that can be set accordingly.

## Supported Kernel Versions

The kernel support builds against Linux 5.10 and every newer release. 5.10 is a
conservative baseline; all kernel APIs used by these interfaces (including
`kvfree_sensitive()`, available since 5.7, used by the character-device and test
interfaces) predate it. Building against an older kernel is rejected at compile
time with an explicit error (see `jitterentropy_compat.h`).

## Build as Standalone Kernel Module

This option compiles a standalone kernel module that can be insmod'ed and
used out of the box. It registers with the kernel crypto API under the name
`jitter_rng`.

Pro:

* No changes to the vanilla kernel.

* Can be compiled separately from the kernel.

Con:

* The used kernel crypto API name is unknown to vanilla kernel users (e.g. the
DRBG). Therefore, it would not be immediately used.

Compilation:

1. Check `Kbuild.config` that `CONFIG_EXTERNAL_JITTERENTROPY` is set to `m` and
   `CONFIG_BUILTIN_JITTERENTROPY` is commented out.
   
2. Call `make`

3. Insert the compiled Linux kernel `jitter_rng.ko`

### Module Parameters

The module offers the following load-time parameters:

* `osr`: OSR applied to all Jitter RNG instances (0 selects the default).

* `flags`: numeric flags value applied to all Jitter RNG instances, using the
  `JENT_*` flag bits from `jitterentropy.h`.

* `ntg1`: boolean shortcut enabling AIS 20/31 NTG.1 compliant operation
  without knowing the numeric value of the `JENT_NTG1` flag bit. Equivalent to
  setting that bit in `flags`.

* `force_fips`: boolean shortcut forcing FIPS compliant operation without
  knowing the numeric value of the `JENT_FORCE_FIPS` flag bit. Equivalent to
  setting that bit in `flags`.

* `cache_all`: boolean shortcut deriving the memory access region from the
  size of all caches instead of only the L1 cache, without knowing the numeric
  value of the `JENT_CACHE_ALL` flag bit. Equivalent to setting that bit in
  `flags`.

* `selftest_interval`: seconds between two runs of the cryptographic self test
  of each Jitter RNG instance (default `0`, no periodic runs; maximum
  2592000). See
  [Periodic Cryptographic Self Test](#linux-kernel-jitter-rng-periodic-cryptographic-self-test)
  below.

* `verbose`: enable verbose logging.

The shortcut parameters are folded into `flags` during module initialization,
so reading the `flags` sysfs file reports the effective configuration. Example:

	insmod jitter_rng.ko ntg1=1 osr=3

## Build with DKMS

A module built as above is bound to the kernel it was compiled against and has
to be rebuilt by hand after every kernel update. DKMS removes that step: the
source is registered once and rebuilt automatically for each kernel installed
afterwards.

The DKMS source is the whole Jitter RNG tree, not just this directory —
`Kbuild.source` compiles the entropy core from `../src` and the architecture
backends from `../arch`. `dkms.conf` therefore sits in the project root, and
the commands below are run from there:

	VERSION=$(awk '/define[ \t]+JENT_(MAJVERSION|MINVERSION|PATCHLEVEL)/ \
		       { printf "%s%s", sep, $3; sep="." }' jitterentropy.h)
	sudo cp -a . /usr/src/jitterentropy-$VERSION
	sudo dkms install -m jitterentropy -v $VERSION

`dkms status` then reports the module as installed, and `modprobe jitter_rng`
loads it. Removal, including the copy installed for every kernel:

	sudo dkms remove -m jitterentropy -v $VERSION --all
	sudo rm -rf /usr/src/jitterentropy-$VERSION

The build requires the kernel headers of the target kernel
(`linux-headers-$(uname -r)` on Debian and Ubuntu, `kernel-devel` on Fedora and
openSUSE). Where DKMS puts the built module is the distribution's choice —
`/lib/modules/<kernel>/updates/dkms` on Debian and Ubuntu, `/lib/modules/<kernel>/extra`
on Fedora — and `depmod` is run by DKMS either way, so `modprobe` finds it.

### Configuration

The build is configured through `Kbuild.config` as in every other build, but
the file to edit is the copy in the DKMS source tree:

	/usr/src/jitterentropy-<version>/linux_kernel/Kbuild.config

Configuration passed on a `make` command line would only reach a single manual
build; the automatic rebuilds DKMS performs on a kernel update read that file.
After changing it, rebuild the currently installed kernels:

	sudo dkms remove -m jitterentropy -v $VERSION --all
	sudo dkms install -m jitterentropy -v $VERSION

`dkms.conf` carries the version as a literal `PACKAGE_VERSION`, since DKMS names
both the `/usr/src` directory and its own build tree after it. When the library
version in `jitterentropy.h` changes, that literal has to follow; CI fails the
build if the two disagree.

## Build in Tree

When the use of the Jitter RNG as a kernel module is insufficient, e.g. when its
services is required during boot time such as for early boot-time entropy, it
can be compiled statically into the kernel binary. To do that, the following
steps have to be taken:

1. copy the entire the Jitter RNG tree into the Linux kernel source tree:

	`cp -av jitterentropy-library-<version> linux-<version>/crypto/jitterentropy-library`
	
2. tell the Linux kernel build system to build the just copied code instead of
   the version that ships with the kernel by editing
   the file `crypto/Makefile` and replace the following:
   
	`obj-$(CONFIG_CRYPTO_JITTERENTROPY) += jitterentropy_rng.o`

   with
   
	`obj-$(CONFIG_CRYPTO_JITTERENTROPY) += jitterentropy-library/linux_kernel/`
	
3. Configure the Linux kernel as usual and ensure that the Jitter RNG is
   selected as needed.
	
4. tell the Jitter RNG to be compiled statically into the kernel by editing the 
   file `crypto/jitterentropy-library/linux_kernel/Kbuild.config` and modify the
   option `CONFIG_EXTERNAL_JITTERENTROPY` to `y` as well as enable the
   option `CONFIG_BUILTIN_JITTERENTROPY`.
   
At this point, the Jitter RNG will now be built statically into the Linux kernel
when compiling it. Naturally, all Linux kernel options can be set as
the Jitter RNG does not depend on specific kernel options.

# Linux Kernel Jitter RNG Character Device

In addition to registering with the kernel crypto API, the module can expose a
character device that provides direct access to the Jitter RNG entropy.

This interface is controlled by the `CONFIG_EXTERNAL_JITTERENTROPY_CHARDEV`
option in `Kbuild.config`. It is enabled by default and can be disabled at
compile time by commenting the option out.

When enabled, the module registers a misc character device `/dev/jitterentropy`.
Its semantics are:

* `open`: a dedicated Jitter RNG instance (entropy collector) is allocated using
  the `osr` and `flags` module parameters. Each open obtains its own independent
  instance.

* `read`: delivers Jitter RNG entropy bytes generated by the instance associated
  with the open file description. Arbitrary read sizes are supported. With
  `O_NONBLOCK`, a read returns `-EAGAIN` instead of waiting for a concurrent
  reader of the same file description, and at most 256 bytes are delivered per
  call (a short read); callers are expected to retry for the remainder.

* `close`: the Jitter RNG instance allocated on open is destroyed.

* `ioctl(JENT_IOCSTATUS)`: retrieves the JSON status string of the Jitter RNG
  instance bound to the open file description (the same information as the
  user space `jent_status()` API: version, health-test state, runtime
  environment and configuration).

Example usage:

	dd if=/dev/jitterentropy bs=32 count=1 | xxd

The `JENT_IOCSTATUS` ioctl and its argument structure are defined in
`jitterentropy_uapi.h`. The caller passes a buffer via `struct
jent_status_ioctl` and receives the NUL-terminated status string:

	#include "jitterentropy_uapi.h"

	char status[JENT_STATUS_MAX_LEN];
	struct jent_status_ioctl arg = {
		.buf	= (unsigned long)status,
		.length	= sizeof(status),
	};
	int fd = open("/dev/jitterentropy", O_RDONLY);

	if (ioctl(fd, JENT_IOCSTATUS, &arg) == 0)
		fputs(status, stdout);

If the supplied buffer is too small the ioctl fails with `-EOVERFLOW` and
`arg.length` is set to the number of bytes required (including the terminating
NUL); on success `arg.length` holds the number of bytes written.

### Single-field ioctls

For callers that want one value and no JSON parser, the fields below are also
available individually. Each reports exactly what the correspondingly named
part of the status document reports - a second spelling of one state, not a
second source - and each is implemented by the character device and by the
debugfs test interface alike:

| ioctl | argument | status field |
| --- | --- | --- |
| `JENT_IOCUUID` | `struct jent_uuid_ioctl` | `uuid` |
| `JENT_IOCVERSION` | `__u32` | `version`, as `major * 1000000 + minor * 10000 + patchlevel * 100` |
| `JENT_IOCOSR` | `__u32` | `configuration.osr` |
| `JENT_IOCFLAGS` | `__u32` | `configuration.flags`, as the raw `JENT_*` bit mask |
| `JENT_IOCHEALTH` | `__u32` | `healthFailure`, as the raw `JENT_*_FAILURE` bit mask |
| `JENT_IOCOUTPUT` | `struct jent_output_ioctl` | `output.invocations` and `output.bytes` |
| `JENT_IOCREINIT` | `__u32` | `reinitializations` |

`JENT_IOCVERSION` describes the library and always answers. The rest describe
an instance and fail with `-ENODATA` when there is none, as does
`JENT_IOCUUID` on a raw instance of the test interface, which skips the
startup that assigns the UUID.

Example:

	#include "jitterentropy_uapi.h"

	struct jent_uuid_ioctl uuid;
	int fd = open("/dev/jitterentropy", O_RDONLY);

	if (ioctl(fd, JENT_IOCUUID, &uuid) == 0)
		printf("%s\n", uuid.uuid);

`jitterentropy-chardev-fields` in `tests/chardev` calls every one of them and
checks each answer against the status document; it takes the file to query as
its argument, so it serves both interfaces.

### Self test ioctl

`JENT_IOCSELFTEST` runs the cryptographic self test - the SHA3-256 and
XDRBG-256 known answer tests of the conditioning component - and reports the
verdict. It is an action rather than a query and therefore requires
`CAP_SYS_ADMIN`, failing with `-EPERM` without it. Both the character device
and the debugfs test interface implement it, and it takes no argument:

	#include "jitterentropy_uapi.h"

	int fd = open("/dev/jitterentropy", O_RDONLY);

	if (ioctl(fd, JENT_IOCSELFTEST) == 0)
		puts("conditioning component verified");

On the character device the run is that of the instance the ioctl arrives on,
handled exactly as a failing periodic run of that instance is, panic under
`fips=1` included: the call returns 0 when the tests pass, and a failure gives
`-EFAULT` and permanently stops the output of this instance - reads on this
open file description fail from then on, while every other instance keeps
delivering (see
[Periodic Cryptographic Self Test](#linux-kernel-jitter-rng-periodic-cryptographic-self-test)
below). A call made after a failed run of this instance gives `-EFAULT`
without running anything.

On the debugfs test interface the run is unbound: its instances record raw,
unconditioned noise, so there is no output to stop and only the verdict of
this run is returned.

Each instance runs the same tests on a timer by itself when
`selftest_interval` is set; this ioctl is for a caller that wants them run at
a moment of its own choosing - after resume, say, or on the schedule of an
audit regime that is not the module's.

# Linux Kernel Jitter RNG Hardware RNG (hwrng)

The module can also register the Jitter RNG with the kernel `hw_random`
framework.

This interface is controlled by the `CONFIG_EXTERNAL_JITTERENTROPY_HWRNG`
option in `Kbuild.config`. It is enabled by default and can be disabled at
compile time by commenting the option out. It requires the running kernel to
provide the `hw_random` core (`CONFIG_HW_RANDOM`).

When enabled, the Jitter RNG appears under the name `jitterentropy` in
`/sys/class/misc/hw_random/rng_available`. Once selected as the current hwrng
(by writing `jitterentropy` to `/sys/class/misc/hw_random/rng_current`), its
output can be read from `/dev/hwrng`. A single Jitter RNG instance is allocated
when the module is loaded and freed when it is unloaded; reads are served from
that instance and serialized.

The amount of entropy the Jitter RNG declares to the kernel is controlled by
the `hwrng_quality` module parameter (bits of estimated entropy per 1024 bits
of output, range 0..1024):

* `0` (default): the device is registered but does not automatically feed the
  kernel random pool. The output remains available via `/dev/hwrng`.

* non-zero (e.g. `1024`): the in-kernel hwrng thread uses the Jitter RNG to
  feed the kernel random pool. As the Jitter RNG is designed to deliver
  conditioned, full-entropy output, `1024` is a reasonable choice.

Example usage:

	echo jitterentropy > /sys/class/misc/hw_random/rng_current
	dd if=/dev/hwrng bs=32 count=1 | xxd

The status of the single hwrng Jitter RNG instance is exported read-only as
`/proc/jitterentropy/hwrng_status` (when the kernel provides `CONFIG_PROC_FS`).
Reading it returns the same JSON status string as the user space
`jent_status()` API (version, health-test state, runtime environment and
configuration):

	cat /proc/jitterentropy/hwrng_status | jq .

Note on the `output` counters in this status: they count the bytes *generated*
by the Jitter RNG instance, which is more than the bytes read from
`/dev/hwrng`. The hw_random core always pulls a full kernel buffer
(`rng_buffer_size()`, typically 64 bytes) from the driver and serves user
reads from that cache, so e.g. a single 32-byte read shows up as 64 generated
bytes. The divergence is bounded by one buffer size; additionally, the
in-kernel hwrng entropy thread consumes bytes when the effective quality is
non-zero. The character-device instance counters
(`/proc/jitterentropy/instances/`) have no such intermediary and match the bytes
delivered to the reader exactly.

# Linux Kernel Jitter RNG procfs Interface

When the kernel provides `CONFIG_PROC_FS`, the module creates the directory
`/proc/jitterentropy/` that collects its read-only status and statistics files:

* `version`: the Jitter RNG library version (e.g. `3.7.1`).

* `config/`: a subdirectory grouping the effective runtime configuration
  shared by the kernel interfaces:

    * `config/flags`: human-readable breakdown of the effective flags value
      (raw value, the state of every used `JENT_*` flag bit and the decoded
      memory-size and hash-loop fields):

			flags:                       0x00000060
			JENT_DISABLE_MEMORY_ACCESS:  off
			JENT_FORCE_INTERNAL_TIMER:   off
			JENT_DISABLE_INTERNAL_TIMER: off
			JENT_FORCE_FIPS:             on
			JENT_NTG1:                   on
			JENT_CACHE_ALL:              off
			max memory size:             auto (derived from cache size)
			hash loop count:             default

    * `config/flags_raw`: the effective flags value as a plain hexadecimal
      number (e.g. `0x00000060`), directly reusable as the `flags=` module
      parameter.

    * `config/osr`: the effective OSR as a plain number, directly reusable as
      the `osr=` module parameter. Reports the value the collectors actually
      run with: an `osr=` request below the library minimum (including the
      default `0`) is raised to that minimum.

    * `config/ntg1`: plain `0`/`1` indicating whether AIS 20/31 NTG.1
      compliant operation is in effect.

    * `config/fips`: plain `0`/`1` indicating whether FIPS compliant operation
      is in effect - when the `JENT_FORCE_FIPS` flag is set, when the kernel
      itself runs in FIPS mode (`fips=1` kernel command line), or when NTG.1
      mode is enabled (NTG.1 implies FIPS operation).

* `interfaces/`: a subdirectory grouping the compile-time interface
  availability of this module build. `interfaces/kcapi`, `interfaces/hwrng`,
  `interfaces/chardev` and `interfaces/testing` each report a plain `0`/`1`
  indicating whether the respective optional interface (crypto API, hwrng,
  character device, debugfs test interface) was compiled in (the
  `CONFIG_EXTERNAL_JITTERENTROPY_*` options in `Kbuild.config`).

* `statistics`: module-wide statistics in JSON format. It reports the
  character-device instances (one Jitter RNG entropy collector is allocated per
  `open()` of `/dev/jitterentropy`) and the periodic cryptographic self test:

		{
			"charDevice": {
				"openInstances": 3,
				"cumulativeOpens": 42
			},
			"selfTest": {
				"intervalSeconds": 3600,
				"runs": 12,
				"secondsSinceLastRun": 87,
				"failures": 0
			}
		}

  `openInstances` is the number of instances open right now; `cumulativeOpens`
  is the total number of opens since the module was loaded.

  `intervalSeconds` is the effective `selftest_interval` module parameter
  (`0` when the periodic runs are disabled), `runs` the number of self test
  runs completed since the module was loaded - across all instances, the
  on-demand ones included - and `secondsSinceLastRun` the age of the last one,
  `null` before the first run. Neither counts the known answer tests
  `jent_entropy_init_ex()` ran at module load, which belong to the library's
  initialization. `failures` is the number of failed runs; each failed run has
  permanently stopped the instance it was bound to, as described below.

* `hwrng_status`: the JSON status string of the single hwrng instance (only
  present when the hwrng interface is enabled, see above).

* `instances/`: a subdirectory holding one file per currently open character
  device instance, named by the instance UUID (see below). Reading
  `instances/<uuid>` returns the JSON status string of that specific instance.
  Files appear on `open()` of `/dev/jitterentropy` and disappear on `close()`
  (only present when the character device interface is enabled).

Every Jitter RNG instance is assigned a stable RFC 4122 version 4 UUID at
allocation. It is reported in the JSON status output as the `uuid` field and,
for the character device, used as the `instances/<uuid>` file name. The UUID is
preserved when an instance's collector is reallocated (e.g. on health-test
recovery), so it identifies the instance for its whole lifetime.

Example usage:

	cat /proc/jitterentropy/statistics | jq .
	ls /proc/jitterentropy/instances/
	cat /proc/jitterentropy/instances/$(ls /proc/jitterentropy/instances/ | head -1) | jq .

# Linux Kernel Jitter RNG Periodic Cryptographic Self Test

The known answer tests of the conditioning component - SHA3-256 and XDRBG-256 -
run once at module load inside `jent_entropy_init_ex()`, the library's
initialization; a failure makes the module refuse to load with `EFAULT`. The
module schedules no additional run of its own at load.

A module that is loaded once and then keeps running for months can repeat them,
which bounds the window in which a conditioning component that broke after the
load keeps delivering data. Every Jitter RNG instance repeats them for itself:
each open character device instance, the hwrng instance and each crypto API
instance queues its own work item, which runs the tests bound to that
instance's collector (`jent_selftest()`, see the library documentation). There
is no module-wide self test machinery behind the instances and no instance
waits for another: a run takes only the lock of its own instance, so that one
instance hands out no data while its own test is running - a blocking reader
waits the two Keccak computations out, a non-blocking one sees "no data" - and
every other instance stays untouched.

The `selftest_interval` module parameter sets the period in seconds; it is `0`
by default, leaving the library's run at load and no timers, as the repetition
is a regime to opt into rather than something every system needs. Where it is
wanted, one hour is a sensible value: the tests are two Keccak computations
over a handful of bytes per instance, so their cost is not measurable at any
sensible interval. They run on the power-efficient workqueue, so on a kernel
running in power-efficient mode (`CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y` or the
`workqueue.power_efficient` kernel parameter) they do not by themselves wake an
idle CPU. That workqueue always exists; without that mode it is the ordinary
per-CPU system workqueue.

Should a run of an instance ever fail:

* When the kernel was booted with `fips=1`, the whole kernel acts as a FIPS 140
  module and the failure is a `panic()`, exactly as a permanent health test
  failure is.

* Otherwise the failure is logged as `cryptographic self test failed`, naming
  the instance by its UUID where it has one, and that instance permanently
  stops delivering: the library marks it and its reads fail with
  `JENT_ERR_SELFTEST`, surfacing as `EFAULT`. Every other instance - and every
  instance created later - keeps delivering, each vouched for by its own runs.
  The debugfs test interface is not covered: it delivers raw, unconditioned
  noise, which does not involve the conditioning component.

The failure is sticky for the instance and its timer stops; a character device
reader recovers by closing and re-opening (a fresh instance), the hwrng and
crypto API instances by reloading the module or reinstantiating the tfm. The
count of failed runs is visible as the `selfTest.failures` field of
`/proc/jitterentropy/statistics`, along with the number of runs completed
across all instances and the age of the last one.

The same tests can be run on demand through the `JENT_IOCSELFTEST` ioctl (see
[Self test ioctl](#self-test-ioctl) above), for a caller that wants them at a
moment of its own choosing. Those runs are counted in the statistics with the
periodic ones, so what is reported there is every run that happened.

# Linux Kernel Jitter RNG Testing

The test code discussed in the following is not intended for production
systems. Its functionality does not support production environment as it offers
additional interfaces to the kernel not needed in production mode.

The Jitter RNG instance discussed before can be tested similar as the user space
instance. For doing that, the option
`CONFIG_EXTERNAL_JITTERENTROPY_TESTINTERFACE` found in the `Kbuild.config` must
be enabled. This enables the test code and the interfaces to utilize the
testing. Instead of editing `Kbuild.config`, the option can also be set on the
make command line:

	make CONFIG_EXTERNAL_JITTERENTROPY_TESTINTERFACE=y

The same applies to the other interface options
(`CONFIG_EXTERNAL_JITTERENTROPY_CHARDEV`,
`CONFIG_EXTERNAL_JITTERENTROPY_HWRNG`), which can be disabled with `=n`.

The debugfs file `jent_raw_hires` provided by the test interface also
implements the `ioctl(JENT_IOCSTATUS)` described in the character device
section above, returning the JSON status string of the raw-noise Jitter RNG
instance bound to the open file description (`getrawentropy --status` prints
it).

In addition, the test interface (and only the test interface) implements
`ioctl(JENT_IOCLOOPCNT)`, which sets the loop count applied to the raw noise
measurements of the open instance: 0 (the default) selects the loop count the
instance was configured with, any other value overrides the hash and memory
access loop counts of every subsequent measurement (`getrawentropy --loopcnt
<NUM>` uses it). See `jitterentropy_uapi.h` for the ABI of both ioctls.

## Test Execution

See `tests/raw-entropy/recording_runtime_kernelspace/README.md`.
