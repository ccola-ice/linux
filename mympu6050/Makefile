# SPDX-License-Identifier: GPL-2.0
obj-m += mympu6050.o

# QEMU guest bzImage is v7.2-rc7 (2026-08-13). Do not use the live parent
# tree: same vermagic / different sizeof(struct module). ARM 板级编译覆盖 KDIR。
KDIR ?= $(HOME)/linux-v7.2-rc7
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
