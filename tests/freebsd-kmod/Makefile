#
# Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
# Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
#
# The Jitter RNG as a FreeBSD kernel module: the library, the arch/ backends
# and one module that runs them on load. See README.md in this directory.
#
# BSD make, as every kernel module is built - not the GNU make the top level
# Makefile is written for.
#
#   make SYSDIR=/usr/src/sys
#   kldload ./jitterentropy_test.ko
#

ROOT=	${.CURDIR}/../..

.PATH:	${ROOT}/src ${ROOT}/arch

KMOD=	jitterentropy_test

# Everything but arch/jitterentropy-arch-thread.c, which is empty without
# JENT_CONF_ENABLE_INTERNAL_TIMER - the kernel has no builtin timer thread to
# offer it (see jitterentropy.h), as linux_kernel/Kbuild.source leaves it out.
SRCS=	jitterentropy-kmod-test.c

SRCS+=	jitterentropy-base.c jitterentropy-gcd.c jitterentropy-health.c \
	jitterentropy-noise.c jitterentropy-sha3.c jitterentropy-status.c \
	jitterentropy-timer.c jitterentropy-uuid.c

SRCS+=	jitterentropy-arch-atomic.c jitterentropy-arch-cache.c \
	jitterentropy-arch-fips.c jitterentropy-arch-memory.c \
	jitterentropy-arch-ncpu.c jitterentropy-arch-random.c \
	jitterentropy-arch-sched.c jitterentropy-arch-timer.c

CFLAGS+=	-I${ROOT} -I${ROOT}/src

# The kernel's warning set is wider than the one the library is developed
# against (-Wcast-qual, -Wmissing-prototypes, -Wredundant-decls, ...). This
# test is about building and running in the kernel, so the warnings are shown
# but do not fail the build.
WERROR=
NO_WERROR=

# The entropy core must be compiled without optimization - the loops it times
# are what it measures - and src/jitterentropy-base.c refuses to compile
# otherwise. The same objects as linux_kernel/Kbuild.source; the status and
# UUID formatting and the arch/ backends are built as the kernel builds.
JENT_O0_SRCS=	jitterentropy-base.c jitterentropy-gcd.c \
		jitterentropy-health.c jitterentropy-noise.c \
		jitterentropy-sha3.c jitterentropy-timer.c

.include <bsd.kmod.mk>

# Appended after the include, so that -O0 comes after the -O2 the kernel
# build puts into CFLAGS and wins. CFLAGS is expanded per object, while
# .IMPSRC names the source being compiled.
.for _src in ${JENT_O0_SRCS}
JENT_CFLAGS.${_src}=	-O0
.endfor
CFLAGS+=	${JENT_CFLAGS.${.IMPSRC:T}}
