#!/bin/sh
# Build and test on a platform with no hosted GitHub runner. Driven by the
# freebsd/openbsd/netbsd/dragonflybsd/solaris/haiku jobs in
# .github/workflows/ci.yml, which reach their platform through a VM, and by the
# cygwin and linux-distro jobs, which are not VMs - a Cygwin installation and
# RHEL/SLES/Arch containers - but want exactly the same build matrix.
#
# Booting the VM dominates the runtime of those jobs, so everything the Linux
# and macOS jobs spread across a matrix is done here in a single boot: both
# CMake link configurations plus the Makefile build.
#
# Strictly POSIX sh. /bin/sh is the Almquist shell on FreeBSD, NetBSD and
# DragonFly, pdksh on OpenBSD, ksh93 on Solaris and bash on Haiku and Cygwin;
# not all of them are bash, and the workflow-level "shell: bash" default in
# ci.yml applies to the runner, not to the guest.
#
# MAKE is overridden by the jobs whose GNU make is not called gmake - Haiku and
# Cygwin ship it as plain make - and CC by those where the default choice would
# be wrong.

set -e

os=$(uname -s)

# The Makefile is GNU make syntax (ifeq, $(filter ...), $(shell ...)). No guest
# here has that as /usr/bin/make - the BSDs ship BSD make and Solaris ships the
# SunOS one - so every job installs GNU make and it is invoked by its own name.
: "${MAKE:=gmake}"

# hw.ncpu is a BSD sysctl and does not exist on Solaris; _NPROCESSORS_ONLN is
# the POSIX spelling and is what answers there. Neither is universal, so both
# are tried and the result is sanity-checked rather than trusted: an empty or
# non-numeric answer would silently become "make -j" with no argument, which is
# unlimited parallelism and enough to wedge a small guest.
ncpu=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
case "$ncpu" in
''|*[!0-9]*) ncpu=$(sysctl -n hw.ncpu 2>/dev/null || true) ;;
esac
case "$ncpu" in
''|*[!0-9]*) ncpu=2 ;;
esac

# cc(1) is not a given, and where it exists it is not always the one wanted: on
# Solaris /usr/bin/cc can be the Oracle Studio compiler, which the GCC-specific
# Makefile cannot be driven by, so that job passes CC in rather than relying on
# this search. Exported so that CMake, the Makefile and the smoke-test link
# below all agree on one compiler - the Makefile's "CC ?=" does not override
# make's built-in default, but an environment value does.
if [ -z "${CC:-}" ]; then
	for c in cc gcc clang; do
		if command -v "$c" > /dev/null 2>&1; then CC=$c; break; fi
	done
fi
if [ -z "${CC:-}" ]; then
	echo "no C compiler found (looked for cc, gcc, clang)" >&2
	exit 1
fi
export CC

echo "==> $(uname -a)"
echo "==> cc: $($CC --version 2>&1 | head -n 1)"
echo "==> using CC=$CC MAKE=$MAKE ncpu=$ncpu"

# ---------------------------------------------------------------------------
# CMake, static and shared
# ---------------------------------------------------------------------------
for shared in OFF ON; do
	build="build-shared-$shared"
	echo "==> CMake build (BUILD_SHARED_LIBS=$shared)"

	rm -rf "$build"
	cmake -S . -B "$build" -DBUILD_SHARED_LIBS="$shared"
	cmake --build "$build" -j "$ncpu"

	# A shared build leaves the library in the top-level build directory,
	# which is not on the guests' default search path.
	LD_LIBRARY_PATH="$PWD/$build:$LD_LIBRARY_PATH"
	export LD_LIBRARY_PATH

	# Cygwin builds a DLL rather than a shared object and resolves it the
	# way Windows does, through PATH; LD_LIBRARY_PATH means nothing there
	# and the shared run would fail to start for want of the library. The
	# directory holds only the library, so prepending it shadows nothing.
	PATH="$PWD/$build:$PATH"
	export PATH

	"$build/tests/gcd/gcd"

	# The CPU information tool. These guests are what compiles its generic
	# backend at all - the BSDs its sysctl half, Solaris, Haiku and Cygwin
	# the half without. It describes the machine rather than testing it, so
	# the output is shown and a failure here is a defect in that backend.
	echo "==> jitterentropy-cpuinfo"
	cpuinfo="$build/tests/raw-entropy/recording_userspace/jitterentropy-cpuinfo"
	"$cpuinfo"
	# The JSON form as well: these guests carry the null values and the
	# backend note the writer has to escape. Only that it runs is checked -
	# no guest is guaranteed a JSON parser, so Linux validates the output.
	"$cpuinfo" --json > /dev/null

	rng="$build/tests/raw-entropy/recording_userspace/jitterentropy-rng"
	for opt in "" --all-caches --force-internal-timer; do
		echo "==> jitterentropy-rng 256 $opt"
		# Unquoted on purpose: the empty case must expand to no argument.
		"$rng" 256 $opt > /dev/null
	done

	# --force-fips and --ntg1 are the two modes that require the collector
	# memory to be locked into RAM (they imply JENT_FORCE_SECURE_MEM), so
	# they are what reaches the mlock path in arch/jitterentropy-arch-memory.c and the
	# limit raising the tool does for them beforehand
	# (tests/raw-entropy/recording_userspace/jitterentropy-memlock.h).
	#
	# They are not gated on. Both can fail on properties of the guest rather
	# than on a defect: a memory lock limit smaller than the collector needs,
	# or a startup whose health tests do not converge on the block size
	# derived from this CPU's caches - which is a real possibility on the
	# emulated CPUs these guests run on. The failure is printed rather than
	# swallowed; note that "set -e" does not apply to a command whose status
	# is consumed by ||.
	for opt in --force-fips --ntg1; do
		echo "==> jitterentropy-rng 256 $opt"
		"$rng" 256 $opt > /dev/null ||
			echo "::warning::jitterentropy-rng $opt failed in $os"
	done
done

# ---------------------------------------------------------------------------
# Makefile
#
# A second, independent build system with its own UNAME_S-keyed flag and
# link-library selection. Those branches are reached from nowhere else, and the
# guests here are what reach them: the BSD case drops -lrt (the POSIX clocks
# live in libc there, and OpenBSD ships no librt at all) while keeping
# -Wl,-z,relro,-z,now, and SunOS does the opposite - it is one of the two
# platforms that still needs -lrt, and its link editor takes neither -z form.
# ---------------------------------------------------------------------------
echo "==> Makefile build ($MAKE)"
"$MAKE" -j "$ncpu"

cat > smoke.c <<'EOF'
#include <stdio.h>
#include "jitterentropy.h"

static int collect(unsigned int flags, const char *what)
{
	char buf[64];
	struct rand_data *ec = jent_entropy_collector_alloc(0, flags);

	if (!ec) { printf("alloc failed (%s)\n", what); return 1; }
	if (jent_read_entropy(ec, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
		printf("short read (%s)\n", what);
		jent_entropy_collector_free(ec);
		return 1;
	}
	jent_entropy_collector_free(ec);
	printf("ok (%s)\n", what);
	return 0;
}

int main(void) {
	int rc = jent_entropy_init();
	if (rc) { printf("jent_entropy_init: %d\n", rc); return 1; }
	if (collect(0, "default")) return 1;
	/*
	 * The internal timer has to be forced: where the counter read works,
	 * which is everywhere this runs, the automatic selection never falls
	 * back to it, so the thread back-end would be linked in and never
	 * started. That back-end is what the Makefile's own -pthread selection
	 * exists for, and this is the only thing the Makefile build runs.
	 */
	if (collect(JENT_FORCE_INTERNAL_TIMER, "internal timer")) return 1;
	return 0;
}
EOF

# Mirrors the Makefile's own LIBRARIES selection, because this links the same
# static archive by hand. -pthread rather than -lpthread: on FreeBSD it is the
# only spelling that selects libthr, and every compiler here accepts it.
case "$os" in
SunOS) smoke_libs="-pthread -lrt" ;;
*)     smoke_libs="-pthread" ;;
esac

# The archive's objects are stack-protector instrumented even though smoke.c
# itself is not compiled with the flag, so their __stack_chk_fail and
# __stack_chk_guard have to resolve here too - and on Solaris the driver only
# links the runtime that defines them when the flag is on the link line. Same
# probe as the Makefile's SSP_USABLE, for the same reason it is a probe and not
# a name check: Solaris needs this and illumos does not, and both say SunOS.
#
# Where the probe fails the build dropped the flag as well, so the archive
# carries no instrumented frames and there is nothing here left to resolve.
if printf 'int main(int c,char**v){char b[64];(void)v;b[0]=(char)c;return b[0];}' \
	| $CC -fstack-protector-strong -x c - -o /dev/null > /dev/null 2>&1; then
	smoke_libs="$smoke_libs -fstack-protector-strong"
fi

# Unquoted on purpose: smoke_libs must split into separate arguments.
# shellcheck disable=SC2086
$CC -std=c11 -I. smoke.c libjitterentropy.a $smoke_libs -o smoke
./smoke
