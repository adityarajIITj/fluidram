# SPDX-License-Identifier: GPL-2.0-only
#
# FluidRAM: Hydrodynamic Memory Compression Driver
# Supports both in-tree kernel builds and out-of-tree kmod builds.
#

ifneq ($(KERNELRELEASE),)
    # In-tree / kbuild invocation
    obj-$(CONFIG_FLUIDRAM) += fluidram.o
    obj-m += fluidram.o
    fluidram-objs := fluidram_drv.o fluid_galois.o fluid_slab.o
else
    # Out-of-tree invocation
    KDIR ?= /lib/modules/$(shell uname -r)/build
    PWD  := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

endif
