# Compile Noise Source as user space application

CC ?= gcc
override CFLAGS +=-pedantic -Wall -Wextra -O2

program_NAME := extractlsb
# The extractlsb source is shared between validation-runtime and
# validation-restart and therefore lives in the common parent directory.
program_C_SRCS := ../extractlsb.c

program_LIBRARIES := rt

LDFLAGS += $(foreach library,$(program_LIBRARIES),-l$(library))

.PHONY: all clean distclean

all: $(program_NAME)

$(program_NAME): $(program_C_SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(program_C_SRCS) -o $(program_NAME) $(LDFLAGS)

clean:
	@- $(RM) $(program_NAME)

distclean: clean
