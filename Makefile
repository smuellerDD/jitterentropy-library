# Compile Noise Source as user space application

CC ?= gcc
#Hardening
ENABLE_STACK_PROTECTOR ?= 1
CFLAGS ?= -fwrapv --param ssp-buffer-size=4 -fvisibility=hidden -fPIE -Wcast-align -Wmissing-field-initializers -Wshadow -Wswitch-enum
CFLAGS +=-Wextra -Wall -pedantic -fPIC -O0 -fwrapv -Wconversion -std=c11

# -pthread rather than -lpthread: it is the spelling every supported toolchain
# understands, and on FreeBSD it is the only correct one (the library to link
# is libthr, which -pthread selects). It belongs in both the compile and the
# link step.
CFLAGS +=-pthread
LDFLAGS +=-pthread

UNAME_S := $(shell uname -s)

# Enable internal timer support
CFLAGS += -DJENT_CONF_ENABLE_INTERNAL_TIMER

# Haiku maps the POSIX errno names onto its own B_* error codes, which are
# negative (based at INT_MIN), so the "return -EXXX" convention this library
# reports failure with comes out inverted: -ENOENT is a positive number there
# and every "if (ret < 0)" reads the failure as success. B_USE_POSITIVE_POSIX_ERRORS
# is Haiku's switch for POSIX-convention code and restores the usual positive
# errno values. See the fuller explanation in CMakeLists.txt.
ifeq ($(UNAME_S),Haiku)
CFLAGS += -DB_USE_POSITIVE_POSIX_ERRORS
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
    CFLAGS += $(SSP_FLAG)
    LDFLAGS += $(SSP_FLAG)
  else
    $(warning Building WITHOUT $(SSP_FLAG): this toolchain resolves neither \
	__stack_chk_fail nor __stack_chk_guard, so the flag would break every link)
  endif
endif

# Clear the call-clobbered registers a function used before it returns, vector
# registers included. GCC 11 and Clang 15 have it, and only for some targets,
# hence the probe. See CMakeLists.txt.
ZCUR_USABLE := $(shell printf 'int main(void){return 0;}' \
	| $(CC) -Werror -fzero-call-used-regs=used -x c - -o /dev/null > /dev/null 2>&1 && echo yes)
ifeq "$(ZCUR_USABLE)" "yes"
  CFLAGS += -fzero-call-used-regs=used
endif

# Change as necessary
PREFIX := /usr/local
# library target directory (either lib or lib64)
LIBDIR := lib

# include target directory
INCDIR := include
SRCDIR := src

NAME := jitterentropy
# grep -E rather than egrep: the latter is deprecated and GNU grep 3.8 (RHEL 10,
# among others) prints an "egrep is obsolescent" warning on every invocation,
# which lands in the middle of the build output three times over.
LIBMAJOR=$(shell grep -E "define\s+JENT_MAJVERSION" jitterentropy.h | awk '{print $$3}')
LIBMINOR=$(shell grep -E "define\s+JENT_MINVERSION" jitterentropy.h | awk '{print $$3}')
LIBPATCH=$(shell grep -E "define\s+JENT_PATCHLEVEL" jitterentropy.h | awk '{print $$3}')
LIBVERSION := $(LIBMAJOR).$(LIBMINOR).$(LIBPATCH)

ARCHDIR := arch
VPATH := $(SRCDIR):$(ARCHDIR)
C_SRCS := $(notdir $(sort $(wildcard $(SRCDIR)/*.c) $(wildcard $(ARCHDIR)/*.c)))
C_OBJS := ${C_SRCS:.c=.o}
OBJS := $(C_OBJS)

analyze_srcs = $(filter %.c, $(sort $(C_SRCS)))
analyze_plists = $(analyze_srcs:%.c=%.plist)

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
	-current_version $(LIBVERSION) -compatibility_version $(LIBMAJOR)
# Apple's strip(1) refuses a full strip of a dylib (the exported symbols
# must remain), so "install -s" aborts the install; install unstripped and
# remove only the local symbols afterwards.
INSTALL_STRIP ?= install
STRIP_SHARED := strip -x
else
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
# the Darwin branch above; the Solaris link editor takes neither in this form,
# so the hardening is applied only where it is known to be understood.
#
# --version-script is the same set of linkers, and it is what limits the
# shared library to the API of jitterentropy.h (see version.lds). Only the
# shared link takes it: an archive has no dynamic symbol table to restrict.
ifneq (,$(filter $(UNAME_S),Linux FreeBSD OpenBSD NetBSD DragonFly))
LDFLAGS += -Wl,-z,relro,-z,now
VERSION_SCRIPT := version.lds
SO_LDFLAGS += -Wl,--version-script=$(VERSION_SCRIPT)
endif
INSTALL_STRIP ?= install -s
STRIP_SHARED := :
endif
SOLINK := lib$(NAME).$(SOEXT)

CFLAGS += $(foreach includedir,$(INCLUDE_DIRS),-I$(includedir))
LDFLAGS += $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir))
LDFLAGS += $(foreach library,$(LIBRARIES),-l$(library))

# libjitterentropy-record, off by default as the CMake option ENABLE_RECORDING
# is: the raw noise recording of RECORD_DIR, for recording where no tool can
# run, under tests/ so that nothing reaches it by accident. It carries a copy
# of the library of its own and exports nothing but the recording, so that it
# links beside libjitterentropy: jitterentropy-record-lib.c compiles the copy
# and the recording as one translation unit in which every function of the
# copy is static, and the one object it yields is both the archive and the
# shared library. On the ELF linkers record.lds limits the shared library's
# exports to the API of jitterentropy-record.h, as version.lds does the
# library's.
ENABLE_RECORDING ?= 0
RECORD_NAME := $(NAME)-record
RECORD_DIR := tests/raw-entropy/recording_library
RECORD_OBJS := jitterentropy-record-lib.o
ifeq ($(UNAME_S),Darwin)
RECORD_SONAME := lib$(RECORD_NAME).$(LIBMAJOR).$(SOEXT)
RECORD_SOFILE := lib$(RECORD_NAME).$(LIBVERSION).$(SOEXT)
RECORD_SONAME_FLAGS = -install_name $(PREFIX)/$(LIBDIR)/$(RECORD_SONAME) \
	-current_version $(LIBVERSION) -compatibility_version $(LIBMAJOR)
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
endif

# jitterentropy-rngd, off by default as the CMake option ENABLE_RNGD is: the
# daemon feeding the Jitter RNG into the Linux /dev/random, with its systemd
# unit. Linked against the archive, so that the daemon runs from wherever it
# is installed without an rpath to the library. The unit goes to
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
endif

.PHONY: all scan install clean distclean check $(NAME) $(NAME)-static \
	$(RECORD_NAME) $(RECORD_NAME)-static install-record install-record-static \
	install-rngd check-rngd

all: $(NAME) $(NAME)-static $(RECORD_ALL) $(RNGD_ALL)

lib$(NAME).a: $(OBJS)
	$(AR) rcs lib$(NAME).a $(OBJS)

$(SOFILE): $(OBJS) $(VERSION_SCRIPT)
	$(CC) -shared $(SONAME_FLAGS) -o $(SOFILE) $(OBJS) $(LDFLAGS) \
		$(SO_LDFLAGS)

$(NAME)-static: lib$(NAME).a
$(NAME): $(SOFILE)

lib$(RECORD_NAME).a: $(RECORD_OBJS)
	$(AR) rcs lib$(RECORD_NAME).a $(RECORD_OBJS)

$(RECORD_SOFILE): $(RECORD_OBJS) $(RECORD_VERSION_SCRIPT)
	$(CC) -shared $(RECORD_SONAME_FLAGS) -o $(RECORD_SOFILE) \
		$(RECORD_OBJS) $(LDFLAGS) $(RECORD_SO_LDFLAGS)

$(RECORD_NAME)-static: lib$(RECORD_NAME).a
$(RECORD_NAME): $(RECORD_SOFILE)

%.o: $(RECORD_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(RNGD_NAME): $(RNGD_OBJS) lib$(NAME).a
	$(CC) $(CFLAGS) -o $@ $(RNGD_OBJS) lib$(NAME).a $(LDFLAGS)

%.o: $(RNGD_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# As CMake's rngd-version test: needs neither root nor the kernel.
check-rngd: $(RNGD_NAME)
	./$(RNGD_NAME) --version 2>&1 | grep -q '$(RNGD_NAME) $(LIBVERSION)'

$(analyze_plists): %.plist: %.c
	@echo "  CCSA  " $@
	clang --analyze $(CFLAGS) $< -o $@

scan: $(analyze_plists)

cppcheck:
	cppcheck --force -q --enable=performance --enable=warning --enable=portability $(shell find * -name \*.h -o -name \*.c)

install: install-man install-shared install-includes $(RECORD_INSTALL) \
	$(RNGD_INSTALL)

# The pages install-man installs: the library's, the kernel module's where
# there is one to load, the recording library's and the daemon's where they
# are built. Each goes to the man<N> directory of its section.
MANPAGES := doc/$(NAME).3 $(wildcard doc/jent_*.3)
ifeq ($(UNAME_S),Linux)
MANPAGES += doc/jitter_rng.4
endif
ifeq ($(ENABLE_RECORDING),1)
MANPAGES += doc/$(RECORD_NAME).3
endif
ifeq ($(ENABLE_RNGD),1)
MANPAGES += doc/$(RNGD_NAME).1
endif

install-man:
	for page in $(MANPAGES); do \
		man=$(DESTDIR)$(PREFIX)/share/man/man$${page##*.}; \
		install -d -m 0755 $$man && \
		install -m 644 $$page $$man/ && \
		gzip -n -f -9 $$man/$${page##*/} || exit 1; \
	done

install-shared:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR)
	$(INSTALL_STRIP) -m 0755 $(SOFILE) $(DESTDIR)$(PREFIX)/$(LIBDIR)/
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

install-static: $(if $(RECORD_INSTALL),install-record-static)
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR)
	install -m 0755 lib$(NAME).a $(DESTDIR)$(PREFIX)/$(LIBDIR)/

# libjitterentropy-record and its header, with ENABLE_RECORDING=1, as
# CMakeLists.txt installs them.
install-record: $(RECORD_SOFILE)
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR) $(DESTDIR)$(PREFIX)/$(INCDIR)
	$(INSTALL_STRIP) -m 0755 $(RECORD_SOFILE) $(DESTDIR)$(PREFIX)/$(LIBDIR)/
	$(STRIP_SHARED) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(RECORD_SOFILE)
	$(RM) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(RECORD_SONAME)
	ln -sf $(RECORD_SOFILE) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(RECORD_SONAME)
	ln -sf $(RECORD_SONAME) $(DESTDIR)$(PREFIX)/$(LIBDIR)/$(RECORD_SOLINK)
	install -m 0644 $(RECORD_DIR)/jitterentropy-record.h $(DESTDIR)$(PREFIX)/$(INCDIR)/

install-record-static: lib$(RECORD_NAME).a
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR) $(DESTDIR)$(PREFIX)/$(INCDIR)
	install -m 0644 lib$(RECORD_NAME).a $(DESTDIR)$(PREFIX)/$(LIBDIR)/
	install -m 0644 $(RECORD_DIR)/jitterentropy-record.h $(DESTDIR)$(PREFIX)/$(INCDIR)/

# jitterentropy-rngd and its unit, with ENABLE_RNGD=1. The unit names the
# daemon where this install puts it.
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
	@- $(RM) $(OBJS)
	@- $(RM) $(addprefix $(SRCDIR)/,$(C_OBJS)) $(addprefix $(ARCHDIR)/,$(C_OBJS))
	@- $(RM) lib$(NAME).so* lib$(NAME).*dylib
	@- $(RM) lib$(NAME).a
	@- $(RM) $(RECORD_OBJS)
	@- $(RM) lib$(RECORD_NAME).so* lib$(RECORD_NAME).*dylib lib$(RECORD_NAME).a
	@- $(RM) $(RNGD_NAME) $(RNGD_OBJS)
	@- $(RM) $(analyze_plists)

distclean: clean
