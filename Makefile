# Compile Noise Source as user space application

# make's own default, cc, which it defines before this line is read - this only
# takes effect under make -R. A "gcc" here never did, and a CC from the
# environment or the command line wins either way.
CC ?= cc
#Hardening
ENABLE_STACK_PROTECTOR ?= 1
# Only preferences in the default, which a CFLAGS from the environment or the
# make command line replaces. What the build is not correct without is appended
# below whatever CFLAGS says: -O0 (see the #error in src/jitterentropy-base.c),
# -fvisibility=hidden, without which every internal function is exported from
# the shared library - on the ELF linkers version.lds still limits the export
# set, on macOS nothing does - and further down -pthread, the internal timer
# and the include paths.
#
# Hence "override" on every append to CFLAGS and LDFLAGS. A plain += is
# ignored for a variable set on the command line, so "make CFLAGS=-g" used to
# build without any of them. And once a variable has an override, make ignores
# every later assignment to it that lacks one, so all of them carry it.
CFLAGS ?= --param ssp-buffer-size=4 -fPIE -Wextra -Wall -pedantic -Wconversion -Wcast-align -Wmissing-field-initializers -Wshadow -Wswitch-enum
override CFLAGS +=-fPIC -O0 -fwrapv -fvisibility=hidden -std=c11
# The CFLAGS the shared links (and the version script probe) hand the driver:
# all of them, so that -fsanitize=* or --coverage bring their runtimes along,
# less the -fPIE/-pie above. Every object is compiled -fPIC regardless, CMake
# leaves its own position-independent-executable flag off a shared link as
# well, and zig cc refuses -fPIE with -shared outright ("dynamic libraries
# cannot be position independent executables").
JENT_LINK_CFLAGS = $(filter-out -fPIE -fpie -pie,$(CFLAGS))

# -pthread rather than -lpthread: it is the spelling every supported toolchain
# understands, and on FreeBSD it is the only correct one (the library to link
# is libthr, which -pthread selects). It belongs in both the compile and the
# link step.
override CFLAGS +=-pthread
override LDFLAGS +=-pthread

UNAME_S := $(shell uname -s)

# Enable internal timer support
override CFLAGS += -DJENT_CONF_ENABLE_INTERNAL_TIMER

# The counter register the aarch64 time stamp reads, as the CMake option of the
# same name: "make AARCH64_NSTIME_REGISTER=cntpct_el0". EXTERNAL_CRYPTO has no
# counterpart here - it needs the crypto library found and linked, which is
# what the CMake build does for it.
ifneq ($(AARCH64_NSTIME_REGISTER),)
override CFLAGS += -DAARCH64_NSTIME_REGISTER='"$(AARCH64_NSTIME_REGISTER)"'
endif

# Haiku maps the POSIX errno names onto its own B_* error codes, which are
# negative (based at INT_MIN), so the "return -EXXX" convention this library
# reports failure with comes out inverted: -ENOENT is a positive number there
# and every "if (ret < 0)" reads the failure as success. B_USE_POSITIVE_POSIX_ERRORS
# is Haiku's switch for POSIX-convention code and restores the usual positive
# errno values. See the fuller explanation in CMakeLists.txt.
ifeq ($(UNAME_S),Haiku)
override CFLAGS += -DB_USE_POSITIVE_POSIX_ERRORS
endif

GCCVERSIONFORMAT := $(shell echo `$(CC) -dumpversion | tr '.' '\n' | wc -l`)
ifeq "$(GCCVERSIONFORMAT)" "3"
  GCC_GTEQ_490 := $(shell expr `$(CC) -dumpversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' -e 's/\.\([0-9]\)/0\1/g' -e 's/^[0-9]\{3,4\}$$/&00/'` \>= 40900)
else
  GCC_GTEQ_490 := $(shell expr `$(CC) -dumpfullversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' -e 's/\.\([0-9]\)/0\1/g' -e 's/^[0-9]\{3,4\}$$/&00/'` \>= 40900)
endif

ifeq "$(ENABLE_STACK_PROTECTOR)" "1"
  ifeq "$(GCC_GTEQ_490)" "1"
    SSP_FLAG := -fstack-protector-strong
  else
    SSP_FLAG := -fstack-protector-all
  endif
  # Something has to define the __stack_chk_fail and __stack_chk_guard that
  # flag emits references to. Most C libraries do, so this costs nothing on
  # Linux, the BSDs, macOS, Cygwin and Android; Solaris' libc defines neither
  # and the runtime has to come from GCC's own libssp, without which every link
  # of an instrumented program fails with "ld: fatal: symbol referencing
  # errors"; Haiku has neither and cannot honour the flag at all.
  #
  # Hence the flag goes into LDFLAGS as well as CFLAGS: the driver links its
  # SSP runtime when it sees -fstack-protector* while driving the link, and
  # nothing when it does not, so naming -lssp here would hardcode one
  # platform's spelling of a choice the driver already makes correctly.
  #
  # Probed rather than keyed on UNAME_S, because illumos added both symbols to
  # its libc (illumos issue #5788) while still reporting SunOS, so the name
  # cannot separate the two. The probe compiles and links in one driver call
  # with the flag on it, which is the arrangement used below. It needs a local
  # array: the -strong variant only instruments frames that have one, and an
  # empty main() would link anywhere and settle nothing.
  SSP_USABLE := $(shell printf 'int main(int c,char**v){char b[64];(void)v;b[0]=(char)c;return b[0];}' \
	| $(CC) $(SSP_FLAG) -x c - -o /dev/null > /dev/null 2>&1 && echo yes)
  ifeq "$(SSP_USABLE)" "yes"
    override CFLAGS += $(SSP_FLAG)
    override LDFLAGS += $(SSP_FLAG)
  else
    $(warning Building WITHOUT $(SSP_FLAG): this toolchain resolves neither \
	__stack_chk_fail nor __stack_chk_guard, so the flag would break every link)
  endif
endif

# Change as necessary
PREFIX := /usr/local
# library target directory (either lib or lib64)
LIBDIR := lib

# include target directory
INCDIR := include
# man page directory, relative to PREFIX as LIBDIR and INCDIR are. The BSDs
# other than FreeBSD keep the manual in PREFIX/man, which is what CMake's
# GNUInstallDirs chooses there too.
ifneq (,$(filter $(UNAME_S),OpenBSD NetBSD DragonFly))
MANDIR ?= man
else
MANDIR ?= share/man
endif
SRCDIR := src

NAME := jitterentropy
# sed rather than awk: minimal container images (SLE BCI) ship no awk.
#
# Only spaces and basic regex: no \s (GNU, and OpenBSD reads it as 's') and no
# [[:space:]] (the legacy Solaris /usr/bin/sed does not know the classes).
JENT_VERSION_SED = sed -n 's/^\#define  *$(1)  *\([0-9][0-9]*\).*/\1/p' jitterentropy.h
LIBMAJOR=$(shell $(call JENT_VERSION_SED,JENT_MAJVERSION))
LIBMINOR=$(shell $(call JENT_VERSION_SED,JENT_MINVERSION))
LIBPATCH=$(shell $(call JENT_VERSION_SED,JENT_PATCHLEVEL))
LIBVERSION := $(LIBMAJOR).$(LIBMINOR).$(LIBPATCH)

# A version component that did not parse would otherwise be found only in the
# installed file name. Fail the build instead.
ifeq ($(strip $(LIBMAJOR)),)
$(error could not read JENT_MAJVERSION from jitterentropy.h)
endif
ifeq ($(strip $(LIBMINOR)),)
$(error could not read JENT_MINVERSION from jitterentropy.h)
endif
ifeq ($(strip $(LIBPATCH)),)
$(error could not read JENT_PATCHLEVEL from jitterentropy.h)
endif

ARCHDIR := arch
# No VPATH: it would also find the objects the kernel build leaves beside the
# sources in src/ and arch/ and take them for this build's. The explicit rules
# below look only at the sources.
C_SRCS := $(notdir $(sort $(wildcard $(SRCDIR)/*.c) $(wildcard $(ARCHDIR)/*.c)))
C_OBJS := ${C_SRCS:.c=.o}
OBJS := $(C_OBJS)

analyze_src_plists = $(patsubst $(SRCDIR)/%.c,%.plist,$(wildcard $(SRCDIR)/*.c))
analyze_arch_plists = $(patsubst $(ARCHDIR)/%.c,%.plist,$(wildcard $(ARCHDIR)/*.c))
analyze_plists = $(analyze_src_plists) $(analyze_arch_plists)

INCLUDE_DIRS := . $(SRCDIR)
LIBRARY_DIRS :=

# Shared-library naming and hardening flags are toolchain specific. Apple's
# ld64 understands neither -z relro/now nor -soname, macOS has no librt (the
# POSIX timer/clock functions live in libSystem), and shared libraries are
# .dylib carrying an install name rather than .so carrying an soname.
ifeq ($(UNAME_S),Darwin)
LIBRARIES :=
SOEXT := dylib
SONAME := lib$(NAME).$(LIBMAJOR).$(SOEXT)
SOFILE := lib$(NAME).$(LIBVERSION).$(SOEXT)
SONAME_FLAGS = -install_name $(PREFIX)/$(LIBDIR)/$(SONAME) \
	-current_version $(LIBVERSION) -compatibility_version $(LIBMAJOR) \
	-headerpad_max_install_names
# Installed unstripped by default, as on the other systems below and as
# CMakeLists.txt installs it. INSTALL_STRIP="install -s" opts in here too, but
# Apple's strip(1) refuses a full strip of a dylib (the exported symbols must
# remain), so "install -s" would abort the install: the -s is taken back out
# and only the local symbols are removed afterwards, with "strip -x".
INSTALL_STRIP ?= install
ifneq (,$(filter -s --strip,$(INSTALL_STRIP)))
override INSTALL_STRIP := $(filter-out -s --strip,$(INSTALL_STRIP))
STRIP_SHARED := strip -x
else
STRIP_SHARED := :
endif
# The install name is fixed when the dylib is linked, from the PREFIX and
# LIBDIR of that make run; "make" followed by "make install PREFIX=..." would
# install a library naming the default directory, which then fails to load.
# The installed copy is given the name of where it is installed instead;
# -headerpad_max_install_names above leaves the room a longer name needs.
SET_INSTALL_NAME := install_name_tool -id
else
SET_INSTALL_NAME := :
# librt is a separate library only on Linux (glibc before 2.17) and Solaris.
# On the BSDs the POSIX clock and timer functions live in libc, and OpenBSD
# ships no librt at all - naming it unconditionally made the link fail there
# with "cannot find -lrt".
ifneq (,$(filter $(UNAME_S),Linux SunOS))
LIBRARIES := rt
else
LIBRARIES :=
endif
SOEXT := so
SONAME := lib$(NAME).$(SOEXT).$(LIBMAJOR)
SOFILE := lib$(NAME).$(SOEXT).$(LIBVERSION)
SONAME_FLAGS = -Wl,-soname,$(SONAME)
# -z relro / -z now are GNU ld and lld spellings. Apple's ld64 is handled by
# the Darwin branch above; the Solaris link editor rejects -z relro but takes
# -z now, so it gets the immediate-binding half only, as in CMakeLists.txt.
#
# --version-script limits the shared library to the API of jitterentropy.h
# (see version.lds). Taken where the linker accepts it, as CMakeLists.txt
# probes for it, rather than for a list of systems: GNU ld and lld beyond the
# BSDs (Haiku, GNU/Hurd) take it too, ld64 and the Solaris link editor do not.
# Only the shared link takes it: an archive has no dynamic symbol table to
# restrict.
ifneq (,$(filter $(UNAME_S),Linux FreeBSD OpenBSD NetBSD DragonFly))
override LDFLAGS += -Wl,-z,relro,-z,now
endif
ifneq ($(UNAME_S),Darwin)
ifneq ($(MAKECMDGOALS),clean)
# A script of its own for the probe: version.lds names symbols the probe does
# not define, which lld refuses (--no-undefined-version). It has a C comment as
# version.lds does, which the Solaris link editor refuses where it takes the
# option as a mapfile, so the probe fails wherever version.lds would. With the
# CFLAGS and LDFLAGS of the real link, which the shared library rules below pass
# to the driver as CMake passes the C flags to its link, so it probes the same
# target and the same linker with the same runtimes (-fsanitize=*, --coverage).
# Less the -fPIE/-pie of the default CFLAGS, as there (see JENT_LINK_CFLAGS).
# With warnings off, so that a -Werror of the caller's cannot fail it, and in a
# directory of its own under TMPDIR (/tmp when TMPDIR names no writable
# directory, and the compiler is handed that fallback too, as a driver that
# puts its own temporaries under TMPDIR fails otherwise) rather than in a
# source tree that may be read-only. The compiler runs inside it, so its
# source, script and output are there, and so is whatever file an option writes beside the output
# or into the working directory (-ftest-coverage, -fdump-*, -save-temps=cwd),
# which against -o /dev/null failed and took the version script away.
VERSION_SCRIPT_USABLE := $(shell t="$${TMPDIR:-/tmp}"; \
	{ [ -d "$$t" ] && [ -w "$$t" ]; } || t=/tmp; \
	d="$$t/jent-probe.$$$$" && mkdir -m 0700 "$$d" && { \
	printf '/* probe */\n{ global: p; local: *; };\n' > "$$d/p.lds" && \
	printf 'int p(void);\nint p(void) { return 0; }\n' > "$$d/p.c" && \
	(cd "$$d" && TMPDIR="$$t" $(CC) $(JENT_LINK_CFLAGS) $(LDFLAGS) -w -shared -fPIC p.c \
	-Wl,--version-script=p.lds -o p.so) >/dev/null 2>&1 && echo 1; \
	rm -rf "$$d"; })
ifneq ($(VERSION_SCRIPT_USABLE),1)
ifneq (,$(filter $(UNAME_S),Linux FreeBSD OpenBSD NetBSD DragonFly))
$(warning the linker takes no version script: the shared library exports what visibility leaves exported)
endif
endif
endif
ifeq ($(VERSION_SCRIPT_USABLE),1)
VERSION_SCRIPT := version.lds
SO_LDFLAGS += -Wl,--version-script=$(VERSION_SCRIPT)
endif
endif
ifeq ($(UNAME_S),SunOS)
override LDFLAGS += -Wl,-z,now
endif
# Installed unstripped, as CMakeLists.txt installs it: "install -s" runs the
# build host's strip, which fails on a cross-compiled library, and it drops the
# debug information distribution packaging splits off itself. Pass
# INSTALL_STRIP="install -s" to strip anyway (on Darwin that means "strip -x",
# see above).
INSTALL_STRIP ?= install
STRIP_SHARED := :
endif
SOLINK := lib$(NAME).$(SOEXT)

# libjitterentropy-record, off by default as the CMake option ENABLE_RECORDING
# is: the raw noise recording of RECORD_DIR, for recording in an app, under
# tests/ so that nothing reaches it by accident. It carries a copy of the
# library of its own and exports nothing but the recording, so that it links
# beside libjitterentropy: jitterentropy-record-lib.c compiles the copy and the
# recording as one translation unit in which every function of the copy is
# static, and the one object it yields is both the archive and the shared
# library. On the ELF linkers record.lds limits the shared library's exports
# to the API of jitterentropy-record.h, as version.lds does the library's.
ENABLE_RECORDING ?= 0
RECORD_NAME := $(NAME)-record
RECORD_DIR := tests/raw-entropy/recording_library
RECORD_OBJS := jitterentropy-record-lib.o
ifeq ($(UNAME_S),Darwin)
RECORD_SONAME := lib$(RECORD_NAME).$(LIBMAJOR).$(SOEXT)
RECORD_SOFILE := lib$(RECORD_NAME).$(LIBVERSION).$(SOEXT)
RECORD_SONAME_FLAGS = -install_name $(PREFIX)/$(LIBDIR)/$(RECORD_SONAME) \
	-current_version $(LIBVERSION) -compatibility_version $(LIBMAJOR) \
	-headerpad_max_install_names
else
RECORD_SONAME := lib$(RECORD_NAME).$(SOEXT).$(LIBMAJOR)
RECORD_SOFILE := lib$(RECORD_NAME).$(SOEXT).$(LIBVERSION)
RECORD_SONAME_FLAGS = -Wl,-soname,$(RECORD_SONAME)
endif
ifneq (,$(VERSION_SCRIPT))
RECORD_VERSION_SCRIPT := $(RECORD_DIR)/record.lds
RECORD_SO_LDFLAGS := -Wl,--version-script=$(RECORD_VERSION_SCRIPT)
endif
RECORD_SOLINK := lib$(RECORD_NAME).$(SOEXT)
ifeq ($(ENABLE_RECORDING),1)
RECORD_ALL := $(RECORD_NAME) $(RECORD_NAME)-static
RECORD_INSTALL := install-record
RECORD_INSTALL_STATIC := install-record-static
endif

# jitterentropy-rngd, off by default as the CMake option ENABLE_RNGD is: the
# daemon feeding the Jitter RNG into the Linux /dev/random, with its systemd
# unit and man page. Linked against the archive, so that the daemon runs from
# wherever it is installed without an rpath to the library. The unit goes to
# JENT_SYSTEMD_UNITDIR, relative to PREFIX unless absolute, as in CMake.
ENABLE_RNGD ?= 0
RNGD_NAME := $(NAME)-rngd
RNGD_DIR := rngd
RNGD_OBJS := jitterentropy-rngd.o
SBINDIR := sbin
JENT_SYSTEMD_UNITDIR ?= lib/systemd/system
RNGD_UNITDIR := $(if $(filter /%,$(JENT_SYSTEMD_UNITDIR)),,$(PREFIX)/)$(JENT_SYSTEMD_UNITDIR)
ifeq ($(ENABLE_RNGD),1)
ifneq ($(UNAME_S),Linux)
$(error ENABLE_RNGD needs Linux: the daemon feeds the Linux /dev/random)
endif
RNGD_ALL := $(RNGD_NAME)
RNGD_INSTALL := install-rngd
RNGD_CHECK := check-rngd
endif

override CFLAGS += $(foreach includedir,$(INCLUDE_DIRS),-I$(includedir))
override LDFLAGS += $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir))
override LDFLAGS += $(foreach library,$(LIBRARIES),-l$(library))

# The headers each object was compiled from, written by the compiler beside the
# object as it compiles (-MMD), so that editing a header rebuilds what includes
# it. -MP adds an empty rule per header, which keeps a deleted or renamed
# header from stopping the build with "No rule to make target". Only CPPFLAGS
# gets these, not CFLAGS: the scan and clang --analyze rules below pass CFLAGS
# and would otherwise write .d files for the .plist targets.
override CPPFLAGS += -MMD -MP
DEPS := $(C_OBJS:.o=.d) $(RECORD_OBJS:.o=.d) $(RNGD_OBJS:.o=.d)

.PHONY: all scan install clean distclean check $(NAME) $(NAME)-static \
	$(RECORD_NAME) $(RECORD_NAME)-static install-record install-record-static \
	install-rngd check-rngd

all: $(NAME) $(NAME)-static $(RECORD_ALL) $(RNGD_ALL)

# The suites whose Makefiles can run them. Each absorbs the sources it tests,
# so none needs the library built first. The CTest suite is the complete one
# (ctest -LE unreliable in a CMake build tree); this is the fallback for a tree
# without CMake, as the Makefiles under tests/ are. The flags this Makefile
# built up are not handed down - they name include paths relative to this
# directory - and each of those Makefiles sets its own. Variables from the make
# command line would reach them all the same, through MAKEFLAGS rather than the
# environment, and a CFLAGS there makes them drop their own appends; hence the
# empty MAKEOVERRIDES, with CC handed on by name as the one worth keeping.
check: MAKEOVERRIDES :=
check: $(RNGD_CHECK)
	unset CFLAGS LDFLAGS; $(MAKE) -C tests/unit check CC="$(CC)"
	unset CFLAGS LDFLAGS; $(MAKE) -C tests/gcd CC="$(CC)" && tests/gcd/gcd
	unset CFLAGS LDFLAGS; $(MAKE) -C tests/health check CC="$(CC)"

# As CMake's rngd-version test: needs neither root nor the kernel.
check-rngd: $(RNGD_NAME)
	./$(RNGD_NAME) --version 2>&1 | grep -q '$(RNGD_NAME) $(LIBVERSION)'

lib$(NAME).a: $(OBJS)
	$(AR) rcs lib$(NAME).a $(OBJS)

$(SOFILE): $(OBJS) $(VERSION_SCRIPT)
	$(CC) $(JENT_LINK_CFLAGS) -shared $(SONAME_FLAGS) -o $(SOFILE) $(OBJS) \
		$(LDFLAGS) $(SO_LDFLAGS)

$(NAME)-static: lib$(NAME).a
$(NAME): $(SOFILE)

lib$(RECORD_NAME).a: $(RECORD_OBJS)
	$(AR) rcs lib$(RECORD_NAME).a $(RECORD_OBJS)

$(RECORD_SOFILE): $(RECORD_OBJS) $(RECORD_VERSION_SCRIPT)
	$(CC) $(JENT_LINK_CFLAGS) -shared $(RECORD_SONAME_FLAGS) -o $(RECORD_SOFILE) \
		$(RECORD_OBJS) $(LDFLAGS) $(RECORD_SO_LDFLAGS)

$(RECORD_NAME)-static: lib$(RECORD_NAME).a
$(RECORD_NAME): $(RECORD_SOFILE)

$(RNGD_NAME): $(RNGD_OBJS) lib$(NAME).a
	$(CC) $(CFLAGS) -o $@ $(RNGD_OBJS) lib$(NAME).a $(LDFLAGS)

# Compile rules naming the source directory; see the note at C_SRCS.
%.o: $(SRCDIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.o: $(ARCHDIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.o: $(RECORD_DIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.o: $(RNGD_DIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Absent before the first build, hence the "-".
-include $(DEPS)

$(analyze_src_plists): %.plist: $(SRCDIR)/%.c
	@echo "  CCSA  " $@
	clang --analyze $(CFLAGS) $< -o $@

$(analyze_arch_plists): %.plist: $(ARCHDIR)/%.c
	@echo "  CCSA  " $@
	clang --analyze $(CFLAGS) $< -o $@

scan: $(analyze_plists)

cppcheck:
	cppcheck --force -q --enable=performance --enable=warning --enable=portability $(shell find * -name \*.h -o -name \*.c)

install: install-man install-shared install-includes install-pkgconfig \
	$(RECORD_INSTALL) $(RNGD_INSTALL)

# The pages install-man installs: the library's, the kernel module's where
# there is one to load, the recording library's where it is built.
MANPAGES := doc/$(NAME).3 $(wildcard doc/jent_*.3)
ifeq ($(UNAME_S),Linux)
MANPAGES += doc/jitter_rng.4
endif
ifeq ($(ENABLE_RECORDING),1)
MANPAGES += doc/$(NAME)-record.3
endif
ifeq ($(ENABLE_RNGD),1)
MANPAGES += doc/$(RNGD_NAME).1
endif

# $(MANPAGES); the pages of the tools and of the kernel library
# go with those, which the CMake build installs. The titles name the version
# as x.y.z and the overview's synopsis as x, y and z; the installed pages
# carry the values of this tree, as the CMake install does, and every further
# function a NAME section lists gets an alias, a symlink: man opens a link
# given as a path too, which a .so page it resolves against the current
# directory then. A placeholder that is gone fails, as the CMake configure
# does, rather than installing a page without the version. A page is removed
# before it is written: an installation from before the split has its name as
# an alias.
install-man:
	for p in '#define JENT_MAJVERSION x"' '#define JENT_MINVERSION y"' \
		 '#define JENT_PATCHLEVEL z"'; do \
		grep -F -q "$$p" doc/$(NAME).3 || \
			{ echo "doc/$(NAME).3 lacks '$$p'" >&2; exit 1; }; \
	done
	for page in $(MANPAGES); do \
		sec=$${page##*.}; \
		n=$$(basename $$page .$$sec); \
		man=$(DESTDIR)$(PREFIX)/$(MANDIR)/man$$sec; \
		install -d -m 0755 $$man || exit 1; \
		grep -F -q '"jitterentropy x.y.z"' $$page || \
			{ echo "$$page lacks '\"jitterentropy x.y.z\"'" >&2; exit 1; }; \
		rm -f $$man/$$n.$$sec $$man/$$n.$$sec.gz; \
		sed -e 's/"jitterentropy x\.y\.z"/"jitterentropy $(LIBVERSION)"/' \
		    -e 's/\(#define JENT_MAJVERSION \)x"/\1$(LIBMAJOR)"/' \
		    -e 's/\(#define JENT_MINVERSION \)y"/\1$(LIBMINOR)"/' \
		    -e 's/\(#define JENT_PATCHLEVEL \)z"/\1$(LIBPATCH)"/' \
		    $$page > $$man/$$n.$$sec || exit 1; \
		chmod 0644 $$man/$$n.$$sec; \
		gzip -n -f -9 $$man/$$n.$$sec || exit 1; \
		for f in $$(sed -n '/^\.SH NAME/,/^\.SH LIBRARY/p' $$page | \
			  sed -n 2p | sed 's/ \\- .*//' | tr -d ','); do \
			[ "$$f" = "$$n" ] && continue; \
			ln -sf $$n.$$sec.gz $$man/$$f.$$sec.gz; \
		done; \
	done

# The installed files depend on what they install, so "make install" in a
# clean tree builds first instead of failing on a missing $(SOFILE).
install-shared: $(SOFILE)
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR)
	$(INSTALL_STRIP) -m 0755 $(SOFILE) $(DESTDIR)$(PREFIX)/$(LIBDIR)/
	$(SET_INSTALL_NAME) $(PREFIX)/$(LIBDIR)/$(SONAME) \
		$(DESTDIR)$(PREFIX)/$(LIBDIR)/$(SOFILE)
	$(STRIP_SHARED) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(SOFILE)
	$(RM) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(SONAME)
	ln -sf $(SOFILE) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(SOLINK)

# jitterentropy.h is the whole installed interface. The arch/ headers are
# internal to the build: nothing the public header declares needs them - struct
# jent_notime_ctx is defined in jitterentropy.h itself, so it compiles on its
# own - and they declare functions that are not exported from the library.
# CMakeLists.txt installs the same set.
install-includes:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(INCDIR)
	install -m 0644 jitterentropy.h $(DESTDIR)$(PREFIX)/$(INCDIR)/

# The pkg-config file the CMake build installs, from the same template.
# Libs.private is what a link against the archive of install-static needs on
# top: -pthread, the stack protector flag and -lrt where they are used above.
# libdir and includedir are relative to prefix, as LIBDIR and INCDIR are to
# PREFIX. There is no CMake package from this build - its targets file is
# what CMake's install(EXPORT) writes, which has no Makefile counterpart - so
# find_package() needs the CMake build. A placeholder that is left over fails
# the install rather than being installed.
PC_LIBS_PRIVATE := -pthread $(if $(filter yes,$(SSP_USABLE)),$(SSP_FLAG)) \
	$(foreach library,$(LIBRARIES),-l$(library))
install-pkgconfig:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR)/pkgconfig
	sed -e 's|@JENT_PC_PREFIX@|$(PREFIX)|' \
	    -e 's|@JENT_PC_LIBDIR@|$${prefix}/$(LIBDIR)|' \
	    -e 's|@JENT_PC_INCLUDEDIR@|$${prefix}/$(INCDIR)|' \
	    -e 's|@PROJECT_NAME@|$(NAME)|' \
	    -e 's|@PROJECT_VERSION@|$(LIBVERSION)|' \
	    -e 's|@JITTER_PC_LIBS@|-l$(NAME)|' \
	    -e 's|@JITTER_PC_LIBS_PRIVATE@|$(strip $(PC_LIBS_PRIVATE))|' \
	    -e 's|@JITTER_PC_REQUIRES_PRIVATE@||' \
	    -e 's|@JITTER_PC_CFLAGS@||' \
	    cmake/$(NAME).pc.in > $(DESTDIR)$(PREFIX)/$(LIBDIR)/pkgconfig/$(NAME).pc
	if grep -q '@[A-Z_]*@' $(DESTDIR)$(PREFIX)/$(LIBDIR)/pkgconfig/$(NAME).pc; then \
		echo "cmake/$(NAME).pc.in has a placeholder the Makefile does not fill" >&2; \
		$(RM) $(DESTDIR)$(PREFIX)/$(LIBDIR)/pkgconfig/$(NAME).pc; exit 1; \
	fi
	chmod 0644 $(DESTDIR)$(PREFIX)/$(LIBDIR)/pkgconfig/$(NAME).pc

# 0644, as CMake's install(TARGETS) gives an archive: it is data for the
# linker, not something that is executed, and 0755 on it is what rpmlint
# reports as spurious-executable-perm and Lintian as executable-not-elf-or-script.
# The shared library above keeps 0755 - that one is mapped executable, and the
# distributions expect the mode there.
install-static: lib$(NAME).a $(RECORD_INSTALL_STATIC)
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR)
	install -m 0644 lib$(NAME).a $(DESTDIR)$(PREFIX)/$(LIBDIR)/

# libjitterentropy-record and its header, with ENABLE_RECORDING=1, as
# install-shared, install-includes and install-static install the library.
install-record: $(RECORD_SOFILE)
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR) $(DESTDIR)$(PREFIX)/$(INCDIR)
	$(INSTALL_STRIP) -m 0755 $(RECORD_SOFILE) $(DESTDIR)$(PREFIX)/$(LIBDIR)/
	$(SET_INSTALL_NAME) $(PREFIX)/$(LIBDIR)/$(RECORD_SONAME) \
		$(DESTDIR)$(PREFIX)/$(LIBDIR)/$(RECORD_SOFILE)
	$(STRIP_SHARED) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(RECORD_SOFILE)
	$(RM) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(RECORD_SONAME)
	ln -sf $(RECORD_SOFILE) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(RECORD_SONAME)
	ln -sf $(RECORD_SONAME) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(RECORD_SOLINK)
	install -m 0644 $(RECORD_DIR)/jitterentropy-record.h $(DESTDIR)$(PREFIX)/$(INCDIR)/

install-record-static: lib$(RECORD_NAME).a
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR) $(DESTDIR)$(PREFIX)/$(INCDIR)
	install -m 0644 lib$(RECORD_NAME).a $(DESTDIR)$(PREFIX)/$(LIBDIR)/
	install -m 0644 $(RECORD_DIR)/jitterentropy-record.h $(DESTDIR)$(PREFIX)/$(INCDIR)/

# jitterentropy-rngd and its unit, with ENABLE_RNGD=1; its man page goes with
# install-man. The unit names the daemon where this install puts it.
install-rngd: $(RNGD_NAME)
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(SBINDIR) $(DESTDIR)$(RNGD_UNITDIR)
	$(INSTALL_STRIP) -m 0755 $(RNGD_NAME) $(DESTDIR)$(PREFIX)/$(SBINDIR)/
	sed -e 's|@PATH@|$(PREFIX)/$(SBINDIR)|' $(RNGD_DIR)/jitterentropy.service.in \
		> $(DESTDIR)$(RNGD_UNITDIR)/jitterentropy.service
	if grep -q '@[A-Z_]*@' $(DESTDIR)$(RNGD_UNITDIR)/jitterentropy.service; then \
		echo "$(RNGD_DIR)/jitterentropy.service.in has a placeholder the Makefile does not fill" >&2; \
		$(RM) $(DESTDIR)$(RNGD_UNITDIR)/jitterentropy.service; exit 1; \
	fi
	chmod 0644 $(DESTDIR)$(RNGD_UNITDIR)/jitterentropy.service

clean:
	@- $(RM) $(NAME)
	@- $(RM) $(OBJS) $(DEPS)
	@- $(RM) $(addprefix $(SRCDIR)/,$(C_OBJS)) $(addprefix $(ARCHDIR)/,$(C_OBJS))
	@- $(RM) lib$(NAME).so* lib$(NAME).*dylib
	@- $(RM) lib$(NAME).a
	@- $(RM) $(RECORD_OBJS) $(RECORD_OBJS:.o=.d)
	@- $(RM) lib$(RECORD_NAME).so* lib$(RECORD_NAME).*dylib lib$(RECORD_NAME).a
	@- $(RM) $(RNGD_NAME) $(RNGD_OBJS) $(RNGD_OBJS:.o=.d)
	@- $(RM) $(analyze_plists)

distclean: clean
