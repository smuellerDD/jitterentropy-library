#
# Copyright (C) 2021 - 2026, Stephan Mueller <smueller@chronox.de>
#

CC		?= cc
# "override" on every append, as the top level Makefile has: a plain += is
# ignored for a variable set on the make command line, so "make CFLAGS=-g"
# here used to drop -DJENT_STATIC_LIB, the internal timer define and every
# -I, and the build failed on the first missing header. The root "check"
# target already clears MAKEOVERRIDES before recursing, so this is the same
# guarantee for anyone running make in this directory directly.
override CFLAGS	+= -Wextra -Wall -pedantic -fPIC -O2 -std=c11
#Hardening
override CFLAGS	+= -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fwrapv --param ssp-buffer-size=4 -fvisibility=hidden -fPIE -Wconversion -Wcast-align -Wmissing-field-initializers -Wshadow -Wswitch-enum

# -fPIE at the link as well as the compile, to match the CFLAGS above.
#
# No -flto: this is a handful of translation units built once and run once, so
# cross-TU inlining buys nothing measurable, and it needs an LTO plugin the
# linker of a platform outside the Linux/macOS mainstream may well not carry -
# which is what kept "make check" from gating the NetBSD, DragonFly and Cygwin
# CI jobs (.github/scripts/vm-build-and-test.sh).
override LDFLAGS	+= -fPIE

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
override LDFLAGS	+= -Wl,-z,relro,-z,now,--as-needed -pie
endif

NAME		:= gcd

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
INCLUDE_DIRS	:= ../../ ../../src
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
# Define files to be compiled
#
###############################################################################
C_SRCS := $(wildcard *.c) ../../src/jitterentropy-gcd.c \
	  ../../arch/jitterentropy-arch-memory.c \
	  ../../arch/jitterentropy-arch-atomic.c
# Objects land here, not beside their sources: the root Makefile's VPATH
# would pick up objects left in ../../src and ../../arch as its own.
vpath %.c ../../src ../../arch

C_OBJS := $(notdir ${C_SRCS:.c=.o})
# gcov writes .gcno and .gcda beside the object.
C_GCOV := ${C_OBJS:.o=.gcda}
C_GCOV += ${C_OBJS:.o=.gcno}
C_GCOV += ${C_OBJS:.o=.gcov}
OBJS := $(C_OBJS)

# The headers each object was compiled from, written beside the object as it
# compiles (-MMD). The vpath above lets make find the sources in ../../src and
# ../../arch, so a changed .c was already rebuilt; a changed header was not -
# editing ../../src/jitterentropy-gcd.h left the stale objects and the stale
# binary in place. -MP adds an empty rule per header, which keeps a deleted or
# renamed one from stopping the build with "No rule to make target". Same
# idiom as the top level Makefile, and CPPFLAGS rather than CFLAGS for the
# same reason: the clang --analyze rule below passes CFLAGS and would
# otherwise write .d files for the .plist targets.
override CPPFLAGS += -MMD -MP
DEPS := $(C_OBJS:.o=.d)

analyze_srcs = $(filter %.c, $(sort $(C_SRCS)))
analyze_plists = $(notdir $(analyze_srcs:%.c=%.plist))

.PHONY: all scan install clean cppcheck distclean debug asanaddress asanthread leak gcov FORCE

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

cppcheck:
	cppcheck --force -q --enable=performance --enable=warning --enable=portability *.h *.c ../lib/*.c ../lib/*.h

# Here as well - see the vpath above.
C_ASM := $(notdir ${C_SRCS:.c=.s})

asm:
	$(foreach b, $(C_SRCS), $(CC) $(CFLAGS) -S -fverbose-asm -o $(notdir ${b:.c=.s}) $(b);)

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
	@- $(RM) $(C_ASM)

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
