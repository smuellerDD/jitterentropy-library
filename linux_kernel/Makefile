KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNELRELEASE)/build

# Kbuild.source defines jitter_rng-y, which the clean target below needs: the
# kernel's own external-module clean only covers this directory, while the
# library objects live in ../src and ../arch. Both files only set variables
# (no rules), so including them does not affect the default target.
include Kbuild.config
include Kbuild.source

default: modules
install: modules_install

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean
	@- $(RM) $(jitter_rng-y)
	@- $(RM) $(join $(dir $(jitter_rng-y)), $(addprefix ., $(notdir $(jitter_rng-y:.o=.o.cmd))))
	@- $(RM) $(join $(dir $(jitter_rng-y)), $(addprefix ., $(notdir $(jitter_rng-y:.o=.o.d))))

modules modules_install:
	# Compile the KO files
	$(MAKE) -C $(KDIR) M=$$PWD $@
