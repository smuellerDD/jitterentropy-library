#
# Copyright (C) 2021 - 2026, Stephan Mueller <smueller@chronox.de>
# Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
#
# Unit tests for src/ and arch/. Each program absorbs the sources it exercises
# (see the comment at the top of CMakeLists.txt in this directory), so nothing
# is linked here and the library does not have to be built first.
#
# CMake/CTest is the primary way to run these - `ctest -LE unreliable` in a
# configured build tree. This Makefile is the same fallback tests/gcd and
# tests/health carry, for a tree with no CMake.
#

CC		?= gcc
# -O0 is mandatory, not a preference: the entropy core absorbed by the
# unit-base programs refuses to compile at any other level (see the #error in
# src/jitterentropy-base.c). -flto is dropped for the same reason.
CFLAGS		+= -Wextra -Wall -pedantic -fPIC -O0 -std=c11
CFLAGS		+= -DJENT_CONF_ENABLE_INTERNAL_TIMER -DJENT_STATIC_LIB -pthread
#Hardening
CFLAGS		+= -fstack-protector-strong -fwrapv --param ssp-buffer-size=4 -fvisibility=hidden -fPIE -Wconversion -Wcast-align -Wmissing-field-initializers -Wshadow -Wswitch-enum

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
LDFLAGS		+= -Wl,-z,relro,-z,now,--as-needed -pie
endif

# One program per unit-*.c, which is what CMakeLists.txt in this directory
# declares as well. Derived from the sources rather than written out: the list
# that used to stand here still named unit-arch and unit-base long after those
# two were split into one program per subject, so make stopped at the missing
# unit-arch.c and never built the ten programs that had replaced them.
NAMES		:= $(sort $(patsubst %.c,%,$(wildcard unit-*.c)))

DESTDIR		:=
ETCDIR		:= /etc
BINDIR		:= /bin
SBINDIR		:= /sbin
SHAREDIR	:= /usr/share/keyutils
MANDIR		:= /usr/share/man
MAN1		:= $(MANDIR)/man1
MAN3		:= $(MANDIR)/man3
MAN5		:= $(MANDIR)/man5
MAN7		:= $(MANDIR)/man7
MAN8		:= $(MANDIR)/man8
INCLUDEDIR	:= /usr/include
LN		:= ln
LNS		:= $(LN) -sf
# Apple's strip(1) reads -s as "strip the symbols named in this file" and
# then takes the binary as that file, aborting with "no files specified".
# Plain strip removes the same symbols from an executable there.
ifeq ($(UNAME_S),Darwin)
STRIP		?= strip
else
STRIP		?= strip -s
endif

###############################################################################
#
# Define compilation options
#
###############################################################################
# .. is tests/, for jitterentropy-memlock.h - shared with the recording tools
# and included by unit.h.
INCLUDE_DIRS	:= ../../ ../../src ../../arch ..
LIBRARY_DIRS	:=
# librt is a separate library only on Linux (glibc before 2.17) and Solaris.
# Elsewhere - macOS, the BSDs - the POSIX clock functions live in libc and
# naming it makes the link fail with "library 'rt' not found". Same rule as
# the top level Makefile.
ifneq (,$(filter $(UNAME_S),Linux SunOS))
LIBRARIES	:= rt
else
LIBRARIES	:=
endif

CFLAGS		+= $(foreach includedir,$(INCLUDE_DIRS),-I$(includedir))
LDFLAGS		+= $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir))
LDFLAGS		+= $(foreach library,$(LIBRARIES),-l$(library))

###############################################################################
#
# Get version name and cross check
#
###############################################################################
###############################################################################
#
# Define files to be compiled
#
###############################################################################
C_SRCS := $(NAMES:=.c)
C_OBJS := ${C_SRCS:.c=.o}
C_GCOV := ${C_SRCS:.c=.gcda}
C_GCOV += ${C_SRCS:.c=.gcno}
C_GCOV += ${C_SRCS:.c=.gcov}
OBJS := $(C_OBJS)

analyze_srcs = $(filter %.c, $(sort $(C_SRCS)))
analyze_plists = $(analyze_srcs:%.c=%.plist)

.PHONY: all check scan install clean cppcheck distclean debug asanaddress asanthread leak gcov DBG-all

all: $(NAMES)

debug: CFLAGS += -g -DDEBUG
debug: DBG-all

asanaddress: CFLAGS += -g -DDEBUG -fsanitize=address -fno-omit-frame-pointer
asanaddress: LDFLAGS += -fsanitize=address
asanaddress: DBG-all

asanthread: CFLAGS += -g -DDEBUG -fsanitize=thread -fno-omit-frame-pointer
asanthread: LDFLAGS += -fsanitize=thread
asanthread: DBG-all

leak: CFLAGS += -g -DDEBUG -fsanitize=leak -fno-omit-frame-pointer
leak: LDFLAGS += -fsanitize=leak
leak: DBG-all

# Compile for the use of GCOV
# Usage after compilation: gcov <file>.c
gcov: CFLAGS += -g -DDEBUG -fprofile-arcs -ftest-coverage
gcov: LDFLAGS += -fprofile-arcs
gcov: DBG-all

###############################################################################
#
# Build the test programs
#
##############################################################################

$(NAMES): %: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# The mocked time source is compiled into this one program only.
unit-mock: CFLAGS += -DJENT_CONF_ENABLE_MOCK_TIMER

DBG-all: CFLAGS += -g -DDEBUG
DBG-all: $(NAMES)

check: $(NAMES)
	@rc=0; for t in $(NAMES); do ./$$t || rc=1; done; exit $$rc

$(analyze_plists): %.plist: %.c
	@echo "  CCSA  " $@
	clang --analyze $(CFLAGS) $< -o $@

scan: $(analyze_plists)

cppcheck:
	cppcheck --force -q --enable=performance --enable=warning --enable=portability *.c

install:
	install -m 0755 $(NAMES) -D -t $(DESTDIR)$(BINDIR)/

###############################################################################
#
# Clean
#
###############################################################################

clean:
	@- $(RM) $(NAMES)
	@- $(RM) $(C_GCOV)
	@- $(RM) *.gcov
	@- $(RM) $(analyze_plists)

distclean: clean
