#
# Copyright (C) 2021 - 2026, Stephan Mueller <smueller@chronox.de>
# Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
#
# Induced failure test of the health tests. health.c compiles
# ../../src/jitterentropy-health.c into itself (see the comment at the top of
# that file), so nothing is linked here and the library does not have to be
# built first.
#

CC		?= cc
# "override" on every append, as the top level Makefile has: a plain += is
# ignored for a variable set on the make command line, so "make CFLAGS=-g"
# here used to drop -DJENT_STATIC_LIB, the internal timer define and every
# -I, and the build failed on the first missing header. The root "check"
# target already clears MAKEOVERRIDES before recursing, so this is the same
# guarantee for anyone running make in this directory directly.
#
# -O0, as the CMake build compiles this program: it absorbs
# src/jitterentropy-health.c and src/jitterentropy-noise.c, which refuse to
# compile optimized. No _FORTIFY_SOURCE, which needs optimization.
override CFLAGS	+= -Wextra -Wall -pedantic -fPIC -O0 -std=c11
#Hardening
override CFLAGS	+= -DJENT_CONF_ENABLE_INTERNAL_TIMER -DJENT_STATIC_LIB
override CFLAGS	+= -fstack-protector-strong -fwrapv --param ssp-buffer-size=4 -fvisibility=hidden -fPIE -Wconversion -Wcast-align -Wmissing-field-initializers -Wshadow -Wswitch-enum

# -fPIE at the link as well as the compile, to match the CFLAGS above.
#
# No -flto: this program is a single translation unit (health.c absorbs
# src/jitterentropy-health.c), so there is nothing for link-time optimization
# to do across units, and it needs an LTO plugin the linker of a platform
# outside the Linux/macOS mainstream may well not carry - which is what kept
# "make check" from gating the NetBSD, DragonFly and Cygwin CI jobs
# (.github/scripts/vm-build-and-test.sh).
override LDFLAGS	+= -fPIE

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
override LDFLAGS	+= -Wl,-z,relro,-z,now,--as-needed -pie
endif

NAME		:= health

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
INCLUDE_DIRS	:= ../../ ../../src ../../arch
LIBRARY_DIRS	:=
LIBRARIES	:=

override CFLAGS	+= $(foreach includedir,$(INCLUDE_DIRS),-I$(includedir))
override LDFLAGS	+= $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir))
override LDFLAGS	+= $(foreach library,$(LIBRARIES),-l$(library))

# No version parsing here. This program embeds no version and printed one only
# from show_vars below; the three awk calls that produced it ran on every
# "make check" recursion, and on the minimal container images the CI uses
# (SLE BCI) there is no awk, so each was an "awk: command not found" line and
# an empty result. The library version is parsed where it is used, in the top
# level Makefile and CMakeLists.txt.

###############################################################################
#
# The OSR range the check target sweeps
#
# Read from the header that defines it rather than written out here, as
# CMakeLists.txt reads the version from jitterentropy.h: after a change of
# JENT_MAX_OSR there (with the cutoff tables extended to it) a hardcoded range
# would leave the cutoffs of the added OSRs untested while still reporting
# success. sed, not awk, for the reason above.
#
# Basic regular expressions only, and no character classes: \s is GNU (OpenBSD
# sed reads it as a literal 's') and the legacy Solaris /usr/bin/sed knows
# neither those nor [[:space:]]. The separator here is a tab rather than the
# spaces the top level Makefile's version expression matches, and a literal
# tab in a Makefile is too easy to lose, so the whitespace is spelled as
# "anything that cannot be part of the macro name or of its value".
#
###############################################################################
OSRFILE    := ../../jitterentropy.h
JENT_OSR_SED = sed -n \
	's/^\#define[^A-Za-z_]*$(1)[^A-Za-z0-9_][^0-9]*\([0-9][0-9]*\).*/\1/p' \
	$(OSRFILE)
MIN_OSR    := $(shell $(call JENT_OSR_SED,JENT_MIN_OSR))
MAX_OSR    := $(shell $(call JENT_OSR_SED,JENT_MAX_OSR))

ifeq ($(strip $(MIN_OSR)),)
$(error could not read JENT_MIN_OSR from $(OSRFILE))
endif
ifeq ($(strip $(MAX_OSR)),)
$(error could not read JENT_MAX_OSR from $(OSRFILE))
endif

###############################################################################
#
# Define files to be compiled
#
###############################################################################
C_SRCS := health.c
C_OBJS := ${C_SRCS:.c=.o}
C_GCOV := ${C_SRCS:.c=.gcda}
C_GCOV += ${C_SRCS:.c=.gcno}
C_GCOV += ${C_SRCS:.c=.gcov}
OBJS := $(C_OBJS)

# What each object was really compiled from, written beside it as it compiles
# (-MMD). health.c #includes ../../src/jitterentropy-health.c and the arch
# atomics rather than linking them, and make sees none of that: with only
# "health.o: health.c" to go on it reported "nothing to be done" after an edit
# to the health tests and left the previous binary in place, so the suite went
# on passing against code that no longer existed. The binary is gitignored, so
# it survived branch switches too, and CI never noticed because it checks out
# fresh every run.
#
# -MP adds an empty rule per dependency, which keeps a deleted or renamed file
# from stopping the build with "No rule to make target". Same idiom as the top
# level Makefile, CPPFLAGS rather than CFLAGS for the same reason: the clang
# --analyze rule below passes CFLAGS and would otherwise write .d files for
# the .plist targets.
override CPPFLAGS += -MMD -MP
DEPS := $(C_OBJS:.o=.d)

analyze_srcs = $(filter %.c, $(sort $(C_SRCS)))
analyze_plists = $(analyze_srcs:%.c=%.plist)

.PHONY: all check scan install clean cppcheck distclean debug asanaddress asanthread leak gcov FORCE

all: $(NAME)

debug: override CFLAGS += -g -DDEBUG
debug: DBG-$(NAME)

asanaddress: override CFLAGS += -g -DDEBUG -fsanitize=address -fno-omit-frame-pointer
asanaddress: override LDFLAGS += -fsanitize=address
asanaddress: DBG-$(NAME)

asanthread: override CFLAGS += -g -DDEBUG -fsanitize=thread -fno-omit-frame-pointer
asanthread: override LDFLAGS += -fsanitize=thread
asanthread: DBG-$(NAME)

leak: override CFLAGS += -g -DDEBUG -fsanitize=leak -fno-omit-frame-pointer
leak: override LDFLAGS += -fsanitize=leak
leak: DBG-$(NAME)

# Compile for the use of GCOV
# Usage after compilation: gcov <file>.c
gcov: override CFLAGS += -g -DDEBUG -fprofile-arcs -ftest-coverage
gcov: override LDFLAGS += -fprofile-arcs
gcov: DBG-$(NAME)

###############################################################################
#
# Build the library
#
##############################################################################

$(NAME): $(OBJS)
	$(CC) -o $(NAME) $(OBJS) $(LDFLAGS)
	$(STRIP) $(NAME)

DBG-$(NAME): $(OBJS)
	$(CC) -g -DDEBUG -o $(NAME) $(OBJS) $(LDFLAGS)

# The command the objects were last built with, rewritten only when it
# changes, so that they are rebuilt when the flags are. asanaddress,
# asanthread, leak, gcov and debug add flags to objects that make otherwise
# found up to date, and linked them uninstrumented; a plain build after them
# kept the instrumented ones.
$(OBJS): build.flags
build.flags: FORCE
	@printf '%s\n' '$(subst ','\'',$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS))' > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

FORCE:

# Absent before the first build, hence the "-".
-include $(DEPS)

$(analyze_plists): %.plist: %.c
	@echo "  CCSA  " $@
	clang --analyze $(CFLAGS) $< -o $@

scan: $(analyze_plists)

# Every OSR, and each replay vector by its exit status - 1 when a test fired, 0
# for the one that must fire none - and by which test fired, as CTest does:
# the exit status is 1 for every firing vector alike, and a vector tripping the
# wrong test passed on it alone. The lines between "health tests that fired:"
# and the first note are the verdict, compared with blanks squeezed. Every
# firing vector must fire its test and nothing else. The lag vectors need the
# lag predictor built in.
JENT_REPLAY_VECTORS := rct-intermittent rct-permanent lag-intermittent \
		       lag-permanent apt-intermittent apt-permanent rct-mem \
		       no-failure

# The usage errors exit 2, not 1, the status of a health test that fired - with
# the reason on stderr - as the health-usage-* CTest cases check: a missing
# replay file, an osr below the range, and a sign behind 0x, which is no time
# stamp (written here, testdata/ holds recordings).
#
# The cutoff values themselves are checked by cutoffs.py, an independent
# derivation from the SP800-90B formulas, as the CTest case
# health-cutoff-tables does. It needs python3 with mpmath; without it the check
# says it was skipped rather than failing, as CTest reports a skip.
PYTHON3 ?= python3

# The sweep is a shell loop rather than a make-expanded list: neither seq(1)
# nor jot(1) is everywhere, and $((...)) is POSIX shell arithmetic every
# /bin/sh in play has.
check: $(NAME)
	osr=$(MIN_OSR); while [ $$osr -le $(MAX_OSR) ]; do \
		./$(NAME) $$osr || exit 1; \
		osr=$$((osr + 1)); \
	done
	for v in $(JENT_REPLAY_VECTORS); do \
		case $$v in lag-*) \
			./$(NAME) --lag-predictor; rc=$$?; \
			test $$rc -eq 1 && continue; \
			test $$rc -eq 0 || { echo "--lag-predictor: $$rc"; exit 1; };; \
		esac; \
		want_rc=1; \
		case $$v in \
		rct-intermittent) want='repetition count test (RCT) intermittent';; \
		rct-permanent) want='repetition count test (RCT) intermittent and permanent';; \
		lag-intermittent) want='lag predictor test intermittent';; \
		lag-permanent) want='lag predictor test intermittent and permanent';; \
		apt-intermittent) want='adaptive proportion test (APT) intermittent';; \
		apt-permanent) want='adaptive proportion test (APT) intermittent and permanent';; \
		rct-mem) want='repetition count test with memory cutoff reached 1 time(s), recovery not replayed';; \
		*) want='no health test fired'; want_rc=0;; \
		esac; \
		out=`./$(NAME) --replay testdata/$$v.txt`; rc=$$?; \
		test $$rc -eq $$want_rc || { echo "replay $$v: exit status $$rc, expected $$want_rc"; exit 1; }; \
		if test $$want_rc -eq 0; then \
			fired=`printf '%s\n' "$$out" | grep 'no health test fired' | sed -e 's/^ *//'`; \
		else \
			fired=`printf '%s\n' "$$out" | \
				sed -n '/health tests that fired:/,/note:/p' | \
				sed -e '1d' -e '$$d' -e 's/^ *//' -e 's/  */ /g'`; \
		fi; \
		test "$$fired" = "$$want" || { printf 'replay %s: FAILED, fired:\n%s\n' "$$v" "$$fired"; exit 1; }; \
	done
	printf '0x10\n0x-5\n' > hex-sign.txt
	for u in no-file bad-osr hex-sign; do \
		case $$u in \
		no-file) set -- --replay; want='--replay needs a file';; \
		bad-osr) set -- --replay testdata/no-failure.txt 0; want='Oversampling rate must be in the range';; \
		hex-sign) set -- --replay hex-sign.txt; want=':2: not a time stamp: 0x-5';; \
		esac; \
		out=`./$(NAME) "$$@" 2>&1`; rc=$$?; \
		test $$rc -eq 2 || { echo "usage $$u: exit status $$rc, expected 2"; rm -f hex-sign.txt; exit 1; }; \
		case "$$out" in *"$$want"*) ;; \
		*) printf 'usage %s: FAILED, said:\n%s\n' "$$u" "$$out"; rm -f hex-sign.txt; exit 1;; \
		esac; \
	done
	rm -f hex-sign.txt
	@if $(PYTHON3) -c 'import mpmath' >/dev/null 2>&1; then \
		echo "$(PYTHON3) cutoffs.py --check ../../src/jitterentropy-health.c"; \
		$(PYTHON3) cutoffs.py --check ../../src/jitterentropy-health.c; \
	else \
		echo "cutoffs.py skipped: the cutoff derivation needs $(PYTHON3) with mpmath"; \
	fi

cppcheck:
	cppcheck --force -q --enable=performance --enable=warning --enable=portability *.c

asm:
	$(foreach b, $(C_SRCS), $(CC) $(CFLAGS) -S -fverbose-asm -o ${b:.c=.s} $(b);)

install:
	install -m 0755 $(NAME) -D -t $(DESTDIR)$(BINDIR)/

###############################################################################
#
# Clean
#
###############################################################################

clean:
	@- $(RM) $(OBJS) $(DEPS) build.flags build.flags.tmp
	@- $(RM) $(NAME)
	@- $(RM) $(C_GCOV)
	@- $(RM) *.gcov
	@- $(RM) $(analyze_plists)
	@- $(RM) hex-sign.txt

distclean: clean

###############################################################################
#
# Show variables
#
###############################################################################
show_vars:
	@echo DESTDIR=$(DESTDIR)
	@echo BINDIR=$(BINDIR)
	@echo NAME=$(NAME)
	@echo LDFLAGS=$(LDFLAGS)
	@echo CFLAGS=$(CFLAGS)
