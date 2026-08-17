#!/bin/sh
# Build and test where there is no hosted GitHub runner: the VM and container
# jobs in ci.yml. Both CMake linkages and the Makefile build run in one boot.
#
# Strictly POSIX sh - ash, pdksh, ksh93 and bash are all in play.

set -e

os=$(uname -s)

# The Makefile is GNU make syntax and no guest here has that as /usr/bin/make.
: "${MAKE:=gmake}"

# Neither spelling is universal, and an empty answer would leave a bare
# "make -j" - unlimited parallelism, enough to wedge a small guest.
ncpu=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
case "$ncpu" in
''|*[!0-9]*) ncpu=$(sysctl -n hw.ncpu 2>/dev/null || true) ;;
esac
case "$ncpu" in
''|*[!0-9]*) ncpu=2 ;;
esac

# Exported so all three builds agree: the Makefile's "CC ?=" loses to make's
# built-in default but not to the environment.
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

	# Not on the guests' default search path.
	LD_LIBRARY_PATH="$PWD/$build:$LD_LIBRARY_PATH"
	export LD_LIBRARY_PATH

	# Cygwin resolves its DLL through PATH, not LD_LIBRARY_PATH.
	PATH="$PWD/$build:$PATH"
	export PATH

	# The deterministic half of the suite, so gated on.
	ctest --test-dir "$build" --output-on-failure -LE unreliable

	# These guests are what compiles the generic CPU information backend at
	# all, so a failure here is a defect in it.
	echo "==> jitterentropy-cpuinfo"
	cpuinfo="$build/tests/raw-entropy/recording_userspace/jitterentropy-cpuinfo"
	"$cpuinfo"
	# Only that the JSON form runs: no guest is guaranteed a parser, so
	# Linux is where the output is validated.
	"$cpuinfo" --json > /dev/null

	rng="$build/tests/raw-entropy/recording_userspace/jitterentropy-rng"
	for opt in "" --all-caches --force-internal-timer; do
		echo "==> jitterentropy-rng 256 $opt"
		# Unquoted on purpose: the empty case must expand to no argument.
		"$rng" 256 $opt > /dev/null
	done

	# The compliance modes, not gated on: on these emulated CPUs they can
	# fail on the memory lock limit or on health tests that do not converge.
	for opt in --force-fips --ntg1; do
		echo "==> jitterentropy-rng 256 $opt"
		"$rng" 256 $opt > /dev/null ||
			echo "::warning::jitterentropy-rng $opt failed in $os"
	done
done

# ---------------------------------------------------------------------------
# Makefile - a second build system, whose BSD and SunOS branches nothing else
# reaches.
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
	 * Forced: where the counter read works, which is everywhere this
	 * runs, the automatic selection never reaches the thread back-end -
	 * the one the Makefile's -pthread exists for.
	 */
	if (collect(JENT_FORCE_INTERNAL_TIMER, "internal timer")) return 1;
	return 0;
}
EOF

# Mirrors the Makefile's LIBRARIES selection, this linking the same archive by
# hand. -pthread, not -lpthread: on FreeBSD only that selects libthr.
case "$os" in
SunOS) smoke_libs="-pthread -lrt" ;;
*)     smoke_libs="-pthread" ;;
esac

# On Solaris the driver links the runtime defining __stack_chk_fail only when
# the flag is on the link line. A probe, as the Makefile's SSP_USABLE is:
# Solaris needs it, illumos does not, and both say SunOS.
if printf 'int main(int c,char**v){char b[64];(void)v;b[0]=(char)c;return b[0];}' \
	| $CC -fstack-protector-strong -x c - -o /dev/null > /dev/null 2>&1; then
	smoke_libs="$smoke_libs -fstack-protector-strong"
fi

# Unquoted on purpose: smoke_libs must split into separate arguments.
# shellcheck disable=SC2086
$CC -std=c11 -I. smoke.c libjitterentropy.a $smoke_libs -o smoke
./smoke
