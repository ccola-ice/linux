# SPDX-License-Identifier: GPL-2.0
obj-m += myram.o

# Default: headers for the running kernel. Override for this tree, e.g.:
#   make KDIR=/home/zhangb/linux
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_clean

.PHONY: all clean
