#!/bin/bash
# 启动 guest 测试块设备驱动。
#
# 对宿主零侵入的关键三点：
#   -net none        不创建任何网络接口、不加防火墙规则
#   不传任何 -drive  guest 看不到宿主的任何块设备
#   initramfs 根     不需要磁盘镜像、不需要 mount、不需要 loop
#
# -virtfs 把本目录以只读 9p 挂进 guest 的 /mnt/host，用来热加载新编的
# myram.ko。这不是块设备直通，guest 写不了宿主文件。
#
# 退出虚拟机：Ctrl-A 然后按 X
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/.." && pwd)"
KERNEL="$TREE/arch/x86/boot/bzImage"
INITRD="$HERE/build/initramfs.cpio.gz"

command -v qemu-system-x86_64 >/dev/null ||
	{ echo "未安装 QEMU，请先 sudo apt install qemu-system-x86" >&2; exit 1; }
[ -f "$KERNEL" ] || { echo "找不到 $KERNEL，请先在源码树 make bzImage" >&2; exit 1; }
[ -f "$INITRD" ] || { echo "找不到 $INITRD，请先运行 ./mkinitramfs.sh" >&2; exit 1; }

# 能读写 /dev/kvm 就用硬件加速，否则退化为 TCG 纯软件模拟（慢但可用）
ACCEL=()
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
	ACCEL=(-enable-kvm -cpu host)
	echo "使用 KVM 硬件加速"
else
	echo "无 /dev/kvm 读写权限，使用 TCG 软件模拟（较慢）"
	echo "如需加速: sudo usermod -aG kvm \$USER 后重新登录"
fi

exec qemu-system-x86_64 \
	"${ACCEL[@]}" \
	-kernel "$KERNEL" \
	-initrd "$INITRD" \
	-append "console=ttyS0 panic=-1 loglevel=6" \
	-m 2G -smp 1 \
	-net none \
	-virtfs local,path="$HERE",mount_tag=hostshare,security_model=none,readonly=on \
	-nographic \
	-no-reboot
