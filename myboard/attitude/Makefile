# SPDX-License-Identifier: GPL-2.0
# 主机自测:  make
# 交叉编译:  make CROSS_COMPILE=arm-linux-gnueabi-
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra

attitude_cube: attitude_cube.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f attitude_cube *.ppm

.PHONY: clean
