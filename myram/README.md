# myram — teaching RAM block device

Stage D: sparse xarray-backed storage with a configurable block size (512 or
4096). Backing pages are allocated on first write, holes read as zero, and
DISCARD/WRITE_ZEROES release fully covered pages.

## Build

Against this kernel tree:

```sh
make -C /home/zhangb/linux M="$PWD" modules
```

Against the running kernel (needs matching headers):

```sh
make
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

Only load into a kernel built from the same source tree (or ABI-compatible
headers). Loading a 7.2-built module into another running kernel will fail
with `Invalid module format` unless versions match.

On this development host the running kernel is currently 6.8, while the module
is built for 7.2, so it can be compiled here but must be runtime-tested after
booting the matching kernel.

## Files

- `myram.c` — driver (tag set, gendisk, `.queue_rq`)
- `Makefile` — out-of-tree kbuild

See also [../linux-block-driver-framework.md](../linux-block-driver-framework.md).
