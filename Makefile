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
ifneq (,$(filter $(UNAME_S),Linux FreeBSD OpenBSD NetBSD DragonFly))
LDFLAGS += -Wl,-z,relro,-z,now
endif
INSTALL_STRIP ?= install -s
STRIP_SHARED := :
endif
SOLINK := lib$(NAME).$(SOEXT)

CFLAGS += $(foreach includedir,$(INCLUDE_DIRS),-I$(includedir))
LDFLAGS += $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir))
LDFLAGS += $(foreach library,$(LIBRARIES),-l$(library))

.PHONY: all scan install clean distclean check $(NAME) $(NAME)-static

all: $(NAME) $(NAME)-static

lib$(NAME).a: $(OBJS)
	$(AR) rcs lib$(NAME).a $(OBJS)

$(SOFILE): $(OBJS)
	$(CC) -shared $(SONAME_FLAGS) -o $(SOFILE) $(OBJS) $(LDFLAGS)

$(NAME)-static: lib$(NAME).a
$(NAME): $(SOFILE)

$(analyze_plists): %.plist: %.c
	@echo "  CCSA  " $@
	clang --analyze $(CFLAGS) $< -o $@

scan: $(analyze_plists)

cppcheck:
	cppcheck --force -q --enable=performance --enable=warning --enable=portability $(shell find * -name \*.h -o -name \*.c)

install: install-man install-shared install-includes

install-man:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/share/man/man3
	install -m 644 doc/$(NAME).3 $(DESTDIR)$(PREFIX)/share/man/man3/
	gzip -n -f -9 $(DESTDIR)$(PREFIX)/share/man/man3/$(NAME).3

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

install-static:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR)
	install -m 0755 lib$(NAME).a $(DESTDIR)$(PREFIX)/$(LIBDIR)/

clean:
	@- $(RM) $(NAME)
	@- $(RM) $(OBJS)
	@- $(RM) $(addprefix $(SRCDIR)/,$(C_OBJS)) $(addprefix $(ARCHDIR)/,$(C_OBJS))
	@- $(RM) lib$(NAME).so* lib$(NAME).*dylib
	@- $(RM) lib$(NAME).a
	@- $(RM) $(analyze_plists)

distclean: clean
