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
# 9p 在这份 bzImage 里是模块；打进 initramfs 后才能挂宿主目录。
NINEP_KOS=(
	"$KDIR/fs/netfs/netfs.ko"
	"$KDIR/net/9p/9pnet.ko"
	"$KDIR/net/9p/9pnet_virtio.ko"
	"$KDIR/fs/9p/9p.ko"
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
for ko in "${NINEP_KOS[@]}"; do
	if [ ! -f "$ko" ]; then
		echo "缺少 9p 模块 $ko，guest 将无法挂载宿主目录" >&2
		echo "请: make -C $KDIR M=fs/netfs netfs.ko && make -C $KDIR M=net/9p 9pnet.ko 9pnet_virtio.ko && make -C $KDIR M=fs/9p 9p.ko" >&2
		continue
	fi
	dest="$ROOT/lib/modules/$(basename "$ko")"
	cp "$ko" "$dest"
	strip --strip-debug "$dest" 2>/dev/null || true
	echo "已打包 9p: $(basename "$ko")"
done

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
mkdir -p /mnt/host
if mount -t 9p -o trans=virtio,version=9p2000.L,cache=none,ro hostshare /mnt/host; then
	echo "宿主 myram/ 已只读挂到 /mnt/host :"
	ls -l /mnt/host/*.ko /mnt/host/*.c 2>/dev/null
else
	echo "9p 未挂上，将使用 initramfs 里的 .ko"
	dmesg | tail -20
fi

KO=
[ -f /mnt/host/myram.ko ] && KO=/mnt/host/myram.ko
[ -z "$KO" ] && [ -f /myram.ko ] && KO=/myram.ko
if [ -n "$KO" ]; then
	echo
	echo "--- insmod $KO ---"
	if insmod "$KO"; then
		echo "加载成功"
		lsmod
		echo "/proc/partitions:"
		cat /proc/partitions
		ls -l /dev/myram0 2>/dev/null || echo "/dev/myram0 尚未出现"
	else
		echo "加载失败, busybox 退出码=$?"
		dmesg | tail -30
	fi
fi

echo
echo "改驱动后：宿主 make，然后在本 guest 里"
echo "  rmmod myram && insmod /mnt/host/myram.ko"
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
