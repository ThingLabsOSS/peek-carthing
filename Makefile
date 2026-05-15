# Build for the minimal MMIO-only peek_mmio.ko variant.
# Designed for stock vendor kernels where the full peek_carthing.ko
# CRC-mismatches too many symbols to be loadable.
#
# Removes -pg to avoid pulling in _mcount (which stock buildroot kernels
# don't export). Tolerates missing attribute consistency from the kernel
# module init macro plumbing on modern GCC.

obj-m := peek_mmio.o

ccflags-y += -Wno-error=missing-attributes
# Strip the kernel-imposed -pg from this file so the build doesn't pull in
# _mcount (which stock buildroot kernels don't export).
CFLAGS_REMOVE_peek_mmio.o := -pg
# Also pass at top level in case kbuild applies CC_FLAGS_FTRACE later.
CC_FLAGS_FTRACE :=

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
