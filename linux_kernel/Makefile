KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNELRELEASE)/build

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
