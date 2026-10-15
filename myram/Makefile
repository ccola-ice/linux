# SPDX-License-Identifier: GPL-2.0
obj-m += myram.o

# QEMU guest runs arch/x86/boot/bzImage, built 2026-08-13 at v7.2-rc7.
# This git tree has moved on (7.3-era headers + stale generated files), so
# `make -C /home/zhangb/linux M=$PWD` produces vermagic 7.2.0-rc7 but a
# different sizeof(struct module). insmod then fails with:
#   .gnu.linkonce.this_module section size must match the kernel's built
#   struct module size at run time
#
# Matching prepared tree (git worktree + copied build artifacts):
#   git worktree add ~/linux-v7.2-rc7 v7.2-rc7
KDIR ?= $(HOME)/linux-v7.2-rc7
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
