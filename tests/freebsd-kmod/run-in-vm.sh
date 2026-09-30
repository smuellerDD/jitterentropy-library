#!/usr/bin/env bash
# Build tests/freebsd-kmod against the running kernel's sources, load it and
# check that it passed. What the freebsd-kmod job in .github/workflows/bsd.yml
# runs, as root, in a FreeBSD VM; see README.md in this directory.

set -euxo pipefail

cd "$(dirname "$0")"

echo "==> $(uname -a)"

# The VM images do not all ship the kernel sources. Those of the running
# release are in its src.txz; a patch level (-pN) is not part of the path.
# The path names the machine and the machine architecture (arm64/aarch64,
# amd64/amd64), which uname -m and uname -p report.
SYSDIR=/usr/src/sys
if [ ! -f "$SYSDIR/conf/kmod.mk" ]; then
	rel=$(uname -r | sed 's/-p[0-9]*$//')
	url="https://download.freebsd.org/releases/$(uname -m)/$(uname -p)/$rel/src.txz"
	echo "==> $SYSDIR missing, fetching $url"
	fetch -o /tmp/src.txz "$url"
	tar -C / -xf /tmp/src.txz usr/src/sys
	rm -f /tmp/src.txz
fi

# BSD make, the system's own - not gmake. Built in an object directory of its
# own: in the source directory bsd.obj.mk warns "Object directory not changed
# from original", and the checkout stays free of build products. MAKEOBJDIR
# is only honoured from the environment and only if the directory exists.
MAKEOBJDIR=$(mktemp -d /tmp/jitterentropy-kmod.XXXXXX)
export MAKEOBJDIR
make SYSDIR="$SYSDIR"

# A failure in MOD_LOAD fails kldload; either way the module's own report is
# what explains it.
#
# Only this load's report counts: the message buffer keeps those of earlier
# loads in the same boot, whose PASS said nothing about this one. Every report
# starts with the library version line, so this one is from the last such line
# on - provided there is one more of them than before the load. Should the
# buffer drop old lines meanwhile, that fails rather than passes.
header='^jitterentropy_test: library version'
before=$(dmesg | grep -c "$header" || true)
rc=0
kldload "$MAKEOBJDIR/jitterentropy_test.ko" || rc=$?
after=$(dmesg | grep -c "$header" || true)
report=""
if [ "$after" -gt "$before" ]; then
	report=$(dmesg | awk -v h="$header" '$0 ~ h { n = 0 } { l[++n] = $0 }
		END { for (i = 1; i <= n; i++) print l[i] }')
fi
[ -z "$report" ] || printf '%s\n' "$report"
if [ "$rc" -ne 0 ]; then
	echo "kldload failed with $rc" >&2
	exit 1
fi

if [ -z "$report" ]; then
	echo "no report from this load of the module" >&2
	exit 1
fi
# No pipe: grep -q leaving at the match would fail the writer under pipefail.
if ! grep -q '^jitterentropy_test: PASS$' <<< "$report"; then
	echo "no PASS line from the module" >&2
	exit 1
fi

kldunload jitterentropy_test
echo "==> jitterentropy_test loaded, passed and unloaded"
