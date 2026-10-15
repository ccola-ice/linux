# myram — teaching RAM block device

Stage D: sparse xarray-backed storage with a configurable block size (512 or
4096). Backing pages are allocated on first write, holes read as zero, and
DISCARD/WRITE_ZEROES release fully covered pages.

## Build

QEMU loads `arch/x86/boot/bzImage` from 2026-08-13 (`7.2.0-rc7`). Compile
against that same kernel, not this live git tree:

```sh
make
./mkinitramfs.sh "$PWD/myram.ko" "$PWD/../mympu6050/mympu6050.ko"
./run-qemu.sh
```

QEMU 把本目录只读 9p 挂到 guest 的 `/mnt/host`。之后改 `myram.c` 不必重打包、不必重启：

```sh
make          # 宿主
# guest:
rmmod myram
insmod /mnt/host/myram.ko
```

`~/linux-v7.2-rc7` is a `v7.2-rc7` git worktree plus the original build
artifacts (`Module.symvers`, `include/generated`, `scripts/mod/modpost`).
Building with `make -C /home/zhangb/linux M=$PWD` picks newer
`include/linux/module.h` while `utsrelease.h` is still `7.2.0-rc7`, so
`insmod` fails with a `struct module` size mismatch.

Against the running host kernel (needs matching headers; do not load a
7.2 module into the 6.8 host):

```sh
make KDIR=/lib/modules/$(uname -r)/build
```

## Load / unload

```sh
sudo insmod myram.ko
sudo insmod myram.ko myram_size_mb=128 myram_blocksize=4096

lsblk
cat /sys/block/myram0/queue/logical_block_size
cat /sys/block/myram0/size

# Fresh device reads as zero:
sudo dd if=/dev/myram0 bs=4096 count=1 status=none | od -An -tx1

# Write and read back:
printf 'myram-test\n' | sudo dd of=/dev/myram0 bs=512 conv=sync status=none
sudo dd if=/dev/myram0 bs=512 count=1 status=none | od -An -tc

# DISCARD releases fully covered backing pages and zeros partial pages:
printf 'garbage' | sudo dd of=/dev/myram0 bs=512 count=1 conv=sync status=none
sudo blkdiscard /dev/myram0 -o 0 -l 4096
sudo dd if=/dev/myram0 bs=512 count=1 status=none | od -An -tc

sudo rmmod myram
```

The unload message reports how many sparse backing pages were freed:

```sh
dmesg | tail
```

## Filesystem smoke test

```sh
sudo insmod myram.ko myram_size_mb=64 myram_blocksize=4096
sudo mkfs.ext4 -F /dev/myram0
sudo mkdir -p /mnt/myram
sudo mount /dev/myram0 /mnt/myram
echo hello | sudo tee /mnt/myram/test
sync
cat /mnt/myram/test
sudo umount /mnt/myram
sudo rmmod myram
```

Load only into the kernel that produced the headers you compiled against.
The 6.8 host cannot load this module. Test in QEMU (`./run-qemu.sh`).
`vermagic` matching is not enough: `sizeof(struct module)` must match too.

## Files

- `myram.c` — driver (tag set, gendisk, `.queue_rq`)
- `Makefile` — out-of-tree kbuild
- `mkinitramfs.sh` / `run-qemu.sh` — guest image and virtfs 9p share

See also [../linux-block-driver-framework.md](../linux-block-driver-framework.md).
