# Compile Noise Source as user space application

CC ?= gcc
CFLAGS +=-Wextra -Wall -pedantic -fPIC -O0
#Hardening
CFLAGS +=-fstack-protector-all -fwrapv --param ssp-buffer-size=4
LDFLAGS +=-Wl,-z,relro,-z,now

# Change as necessary
PREFIX := /usr/local
# library target directory (either lib or lib64)
LIBDIR := lib

# include target directory
INCDIR := include

INSTALL_STRIP ?= install -s

NAME := jitterentropy
LIBMAJOR=$(shell cat jitterentropy-base.c | grep define | grep MAJVERSION | awk '{print $$3}')
LIBMINOR=$(shell cat jitterentropy-base.c | grep define | grep MINVERSION | awk '{print $$3}')
LIBPATCH=$(shell cat jitterentropy-base.c | grep define | grep PATCHLEVEL | awk '{print $$3}')
LIBVERSION := $(LIBMAJOR).$(LIBMINOR).$(LIBPATCH)

#C_SRCS := $(wildcard *.c) 
C_SRCS := jitterentropy-base.c
C_OBJS := ${C_SRCS:.c=.o}
OBJS := $(C_OBJS)

analyze_srcs = $(filter %.c, $(sort $(C_SRCS)))
analyze_plists = $(analyze_srcs:%.c=%.plist)

INCLUDE_DIRS :=
LIBRARY_DIRS :=
LIBRARIES := rt

CFLAGS += $(foreach includedir,$(INCLUDE_DIRS),-I$(includedir))
LDFLAGS += $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir))
LDFLAGS += $(foreach library,$(LIBRARIES),-l$(library))

.PHONY: all scan install clean distclean check

all: $(NAME) $(NAME)-static

$(NAME)-static: $(OBJS)
	$(AR) rcs lib$(NAME).a $(OBJS)

$(NAME): $(OBJS)
	$(CC) -shared -Wl,-soname,lib$(NAME).so.$(LIBMAJOR) -o lib$(NAME).so.$(LIBVERSION) $(OBJS) $(LDFLAGS)

$(analyze_plists): %.plist: %.c
	@echo "  CCSA  " $@
	clang --analyze $(CFLAGS) $< -o $@

scan: $(analyze_plists)

cppcheck:
	cppcheck --force -q --enable=performance --enable=warning --enable=portability *.h *.c

install: install-man install-shared install-includes

install-man:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/share/man/man3
	install -m 644 doc/$(NAME).3 $(DESTDIR)$(PREFIX)/share/man/man3/
	gzip -f -9 $(DESTDIR)$(PREFIX)/share/man/man3/$(NAME).3

install-shared:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR)
	$(INSTALL_STRIP) -m 0755 lib$(NAME).so.$(LIBVERSION) $(DESTDIR)$(PREFIX)/$(LIBDIR)/
	$(RM) $(DESTDIR)$(PREFIX)/$(LIBDIR)/lib$(NAME).so.$(LIBMAJOR)
	ln -sf lib$(NAME).so.$(LIBVERSION) $(DESTDIR)$(PREFIX)/$(LIBDIR)/lib$(NAME).so.$(LIBMAJOR)
	ln -sf lib$(NAME).so.$(LIBMAJOR) $(DESTDIR)$(PREFIX)/$(LIBDIR)/lib$(NAME).so

install-includes:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(INCDIR)
	install -m 0644 jitterentropy.h $(DESTDIR)$(PREFIX)/$(INCDIR)/
	install -m 0644 jitterentropy-base-user.h $(DESTDIR)$(PREFIX)/$(INCDIR)/

install-static:
	install -d -m 0755 $(DESTDIR)$(PREFIX)/$(LIBDIR)
	install -m 0755 lib$(NAME).a $(DESTDIR)$(PREFIX)/$(LIBDIR)/

clean:
	@- $(RM) $(NAME)
	@- $(RM) $(OBJS)
	@- $(RM) lib$(NAME).so*
	@- $(RM) lib$(NAME).a
	@- $(RM) $(analyze_plists)

distclean: clean
