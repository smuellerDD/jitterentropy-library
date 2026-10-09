#!/usr/bin/env bash
# Check that a shared library exports the global: list of its version script
# and nothing else. CMakeLists.txt checks that list against the header at
# configure time; this checks the built library against the list - in CI for
# the Makefile build, and as the exported-symbols* tests of a shared CMake
# build.
#
# Every defined global counts, not only the jent_ ones: on macOS, where the
# linker takes no version script, -fvisibility=hidden alone limits the export
# set, and a global of another name leaks just the same. What the toolchain
# puts into every shared library is left out.
#
# Linux (GNU or LLVM nm) and macOS. Usage: check-exports.sh <library> <script>

set -euxo pipefail

lib=$1
lds=$2

# Captured rather than piped: nm failing must fail the check, not leave awk
# with an empty table that matches nothing.
case "$(uname -s)" in
Darwin)
	table=$(nm -P -g -U "$lib")
	strip_us=1
	;;
*)
	table=$(nm -P -D -g --defined-only "$lib")
	strip_us=0
	;;
esac

# POSIX format: name, type, value, size. Mach-O prefixes every C name with an
# underscore, dropped there and only there.
exported=$(awk -v strip_us="$strip_us" '
	BEGIN {
		n = split("_init _fini __bss_start _edata _end _etext __end__ " \
			  "__bss_end__ __bss_start__ _bss_end__ __data_start " \
			  "_DYNAMIC _GLOBAL_OFFSET_TABLE_ " \
			  "_PROCEDURE_LINKAGE_TABLE_ _mh_dylib_header " \
			  "dyld_stub_binder", t, " ")
		for (i = 1; i <= n; i++)
			toolchain[t[i]] = 1
	}
	NF >= 2 && $2 != "U" && $2 != "w" && $2 != "v" {
		name = $1
		if (strip_us)
			sub(/^_/, "", name)
		if (!(name in toolchain))
			print name
	}' <<<"$table" | LC_ALL=C sort -u)

expected=$(awk '
	/global:/ { g = 1; next }
	/local:/  { g = 0 }
	g && /;/  { gsub(/[ \t;]/, ""); print }' "$lds" | LC_ALL=C sort -u)

if [ -z "$expected" ]; then
	echo "::error::no global: symbols read from $lds"
	exit 1
fi

if [ "$exported" != "$expected" ]; then
	echo "::error::$lib does not export what $lds lists"
	diff <(echo "$expected") <(echo "$exported") || true
	exit 1
fi

echo "$lib: the $(wc -l <<<"$expected" | tr -d ' ') symbols of $lds"
