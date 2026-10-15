#!/bin/bash
# 构建用于测试块设备驱动的最小 initramfs。
#
#   ./mkinitramfs.sh [模块.ko ...]
#
# 产物 build/initramfs.cpio.gz 是纯内存根文件系统：不需要磁盘镜像、
# 不需要 mount 宿主目录、不需要 loop 设备，因此对宿主零侵入。
# 传入的 .ko 会被放到 guest 的 / 下，由 /init 自动 insmod。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/build/root"
OUT="$HERE/build/initramfs.cpio.gz"
KDIR="${KDIR:-$HOME/linux-v7.2-rc7}"
# 9p / IIO / i2c-stub 在这份 bzImage 里是模块；打进 initramfs 后才能挂宿主
# 目录、并在没有真 MPU-6050 的 QEMU 里用 i2c-stub 绑定 mympu6050。
NINEP_KOS=(
	"$KDIR/fs/netfs/netfs.ko"
	"$KDIR/net/9p/9pnet.ko"
	"$KDIR/net/9p/9pnet_virtio.ko"
	"$KDIR/fs/9p/9p.ko"
)
IIO_KOS=(
	"$KDIR/drivers/iio/industrialio.ko"
	"$KDIR/drivers/iio/buffer/kfifo_buf.ko"
	"$KDIR/drivers/iio/buffer/industrialio-triggered-buffer.ko"
	"$KDIR/drivers/i2c/i2c-stub.ko"
)

BUSYBOX=/usr/bin/busybox
# 需要但 busybox 不内置的宿主工具，连同其依赖库一起打包
EXTRA_BINS=(/usr/sbin/mkfs.ext4)

[ -x "$BUSYBOX" ] || { echo "缺少 $BUSYBOX，请先 apt install busybox-static" >&2; exit 1; }
file "$BUSYBOX" | grep -q "statically linked" ||
	{ echo "$BUSYBOX 不是静态链接，initramfs 会缺库" >&2; exit 1; }

rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,sbin,dev,proc,sys,lib,lib64,mnt,tmp,root}

cp "$BUSYBOX" "$ROOT/bin/busybox"

# 复制一个动态链接的宿主可执行文件及其全部依赖库
copy_with_libs() {
	local bin="$1"
	[ -x "$bin" ] || { echo "跳过不存在的 $bin" >&2; return 0; }
	install -D "$bin" "$ROOT/sbin/$(basename "$bin")"
	ldd "$bin" 2>/dev/null |
		awk '/=>/ {print $3} /ld-linux/ && $1 ~ /^\// {print $1}' |
		grep '^/' | sort -u |
	while read -r lib; do
		install -D "$lib" "$ROOT$lib"
	done
}
for b in "${EXTRA_BINS[@]}"; do copy_with_libs "$b"; done

# 待测模块（后备：9p 挂不上时仍可从 initramfs 加载）
for ko in "$@"; do
	[ -f "$ko" ] || { echo "找不到模块 $ko" >&2; exit 1; }
	cp "$ko" "$ROOT/"
	echo "已打包模块: $(basename "$ko")"
done

mkdir -p "$ROOT/lib/modules"
pack_kmod() {
	local ko="$1"
	if [ ! -f "$ko" ]; then
		echo "缺少模块 $ko" >&2
		return 0
	fi
	local dest="$ROOT/lib/modules/$(basename "$ko")"
	cp "$ko" "$dest"
	strip --strip-debug "$dest" 2>/dev/null || true
	echo "已打包: $(basename "$ko")"
}
for ko in "${NINEP_KOS[@]}" "${IIO_KOS[@]}"; do
	pack_kmod "$ko"
done

cat > "$ROOT/bin/bind-mpu" <<'BIND'
#!/bin/busybox sh
# 把 mympu6050 绑到 i2c-stub 上的 0x68。insmod 本身不会创建 IIO 节点。
export PATH=/bin:/sbin

mod_loaded() {
	grep -q "^$(echo "$1" | tr '-' '_') " /proc/modules
}

for m in industrialio kfifo_buf industrialio-triggered-buffer; do
	mod_loaded "$m" && continue
	[ -f "/lib/modules/$m.ko" ] && insmod "/lib/modules/$m.ko"
done
if ! mod_loaded i2c-stub; then
	insmod /lib/modules/i2c-stub.ko chip_addr=0x68 || {
		echo "insmod i2c-stub 失败（必须带 chip_addr=0x68）"
		dmesg | tail -10
		exit 1
	}
fi

echo "I2C 适配器:"
STUB_BUS=
for d in /sys/bus/i2c/devices/i2c-*; do
	[ -f "$d/name" ] || continue
	n=$(cat "$d/name")
	b=${d##*/}
	b=${b#i2c-}
	echo "  i2c-$b  $n"
	case "$n" in
	*stub*) STUB_BUS=$b ;;
	esac
done
[ -n "$STUB_BUS" ] || { echo "没有 i2c-stub 适配器"; dmesg | tail -15; exit 1; }
echo "使用 i2c-$STUB_BUS"

# 内核节点是 /dev/i2c-N；BusyBox i2cset 打开 /dev/i2c/N
mkdir -p /dev/i2c
if [ -e /dev/i2c-"$STUB_BUS" ]; then
	ln -sf /dev/i2c-"$STUB_BUS" /dev/i2c/"$STUB_BUS"
else
	echo "没有 /dev/i2c-$STUB_BUS，i2c-dev 节点未出现"
	exit 1
fi

# -f：即使已有内核驱动占用该地址也强制写（reload 时需要）
i2cset -fy "$STUB_BUS" 0x68 0x75 0x68 b
i2cset -fy "$STUB_BUS" 0x68 0x3f 0x40 b
i2cset -fy "$STUB_BUS" 0x68 0x40 0x00 b
who=$(i2cget -fy "$STUB_BUS" 0x68 0x75 b)
echo "WHO_AM_I=$who (应为 0x68)"
[ "$who" = "0x68" ] || { echo "i2c-stub 写寄存器失败"; exit 1; }

MPU=
[ -f /mnt/mpu/mympu6050.ko ] && MPU=/mnt/mpu/mympu6050.ko
[ -z "$MPU" ] && [ -f /mympu6050.ko ] && MPU=/mympu6050.ko
[ -n "$MPU" ] || { echo "找不到 mympu6050.ko"; exit 1; }
if ! mod_loaded mympu6050; then
	echo "insmod $MPU"
	insmod "$MPU"
fi

# 已有 0-0068 则 insmod 时已经 probe；没有就 new_device
cli=
for c in /sys/bus/i2c/devices/*-0068; do
	[ -d "$c" ] && cli=$c
done
if [ -z "$cli" ]; then
	echo "echo mympu6050 0x68 > /sys/bus/i2c/devices/i2c-$STUB_BUS/new_device"
	echo mympu6050 0x68 > "/sys/bus/i2c/devices/i2c-$STUB_BUS/new_device" || true
	sleep 1
else
	echo "已有 I2C 客户: $cli"
	if [ ! -e "$cli/driver" ]; then
		echo "驱动未绑定，尝试 bind"
		echo "${cli##*/}" > /sys/bus/i2c/drivers/mympu6050/bind || true
		sleep 1
	fi
fi

dmesg | grep -i mympu6050 | tail -8
echo "iio devices:"
ls -l /sys/bus/iio/devices/ 2>/dev/null || echo "(无)"
for n in /sys/bus/iio/devices/iio:device*/name; do
	[ -f "$n" ] || continue
	dev=$(dirname "$n")
	echo "  $(cat "$n")  $dev"
	[ -f "$dev/in_accel_z_raw" ] && echo "  in_accel_z_raw=$(cat "$dev/in_accel_z_raw")"
done
BIND
chmod +x "$ROOT/bin/bind-mpu"

cat > "$ROOT/init" <<'INIT'
#!/bin/busybox sh
# initramfs 的 PID 1。注意 CONFIG_DEVTMPFS_MOUNT 只作用于真实根，
# initramfs 下必须自己挂 /dev。
/bin/busybox --install -s /bin
mount -t proc     none /proc
mount -t sysfs    none /sys
mount -t devtmpfs none /dev
mkdir -p /dev/pts && mount -t devpts none /dev/pts
# -nographic 只看得到 ttyS0；/dev/console 往往是 VGA，输出会丢。
exec </dev/ttyS0 >/dev/ttyS0 2>&1
export PATH=/bin:/sbin
export PS1='guest# '

echo
echo "================ guest 已启动 ================"
echo "内核: $(uname -r)   内存: $(awk '/MemTotal/{print $2/1024 " MiB"}' /proc/meminfo)"
echo "根目录: $(ls -l /)"

# virtio-9p 不是网卡。9pnet 依赖 netfs，必须先加载。
for m in netfs 9pnet 9pnet_virtio 9p; do
	[ -f "/lib/modules/$m.ko" ] || continue
	insmod "/lib/modules/$m.ko" || echo "insmod $m.ko 失败"
done
mount_9p() {
	local tag="$1" dir="$2" label="$3"
	mkdir -p "$dir"
	if mount -t 9p -o trans=virtio,version=9p2000.L,cache=none,ro "$tag" "$dir"; then
		echo "宿主 $label 已只读挂到 $dir"
		ls -l "$dir"/*.ko "$dir"/*.c 2>/dev/null
		return 0
	fi
	echo "9p $tag 未挂上"
	return 1
}
mount_9p hostshare /mnt/host myram/ || true
mount_9p mpushare /mnt/mpu mympu6050/ || true

load_ko() {
	local ko="$1"
	[ -f "$ko" ] || return 1
	echo
	echo "--- insmod $ko ---"
	if insmod "$ko"; then
		echo "加载成功"
		return 0
	fi
	echo "加载失败, busybox 退出码=$?"
	dmesg | tail -20
	return 1
}

KO=
[ -f /mnt/host/myram.ko ] && KO=/mnt/host/myram.ko
[ -z "$KO" ] && [ -f /myram.ko ] && KO=/myram.ko
if [ -n "$KO" ]; then
	load_ko "$KO" && {
		echo "/proc/partitions:"
		cat /proc/partitions
		ls -l /dev/myram0 2>/dev/null || echo "/dev/myram0 尚未出现"
	}
fi

echo
bind-mpu || echo "bind-mpu 失败，可在 guest 里再跑一次 bind-mpu"

echo
echo "改 myram：  宿主 myram/ 下 make，然后  rmmod myram && insmod /mnt/host/myram.ko"
echo "改 mpu：    宿主 mympu6050/ 下 make，然后"
echo "  rmmod mympu6050 && insmod /mnt/mpu/mympu6050.ko && bind-mpu"
echo "退出虚拟机：Ctrl-A 然后 X"
echo "============================================="
echo
exec setsid sh -c 'exec sh </dev/ttyS0 >/dev/ttyS0 2>&1'
INIT
chmod +x "$ROOT/init"

mkdir -p "$(dirname "$OUT")"
( cd "$ROOT" && find . -print0 | cpio --null -ov --format=newc 2>/dev/null ) |
	gzip -9 > "$OUT"

echo
echo "initramfs: $OUT  ($(du -h "$OUT" | cut -f1))"
