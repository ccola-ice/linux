# SPDX-License-Identifier: GPL-2.0
obj-m += mympu6050.o

# Build against this source tree by default (module uses IIO/regmap APIs
# of the tree, not necessarily the running kernel):
#   make            # KDIR defaults to the parent tree
#   make KDIR=/lib/modules/$(uname -r)/build
KDIR ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
