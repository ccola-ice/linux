# MyBoard F1C200s — 自制主板 bring-up 手册

自制 Allwinner F1C200s（ARM926EJ-S，64 MB 内置 DRAM）主板，板载 MPU-6050
（I2C 接口）。本文档覆盖硬件约定、启动链、内核、rootfs 与联调验证。

配套文件：

- 板级 DTS：`arch/arm/boot/dts/allwinner/suniv-f1c200s-myboard.dts`
- 内核配置片段：`myboard/kernel.config`
- 教学驱动：`mympu6050/`（对照主线 `drivers/iio/imu/inv_mpu6050/`）
- 姿态显示程序：`myboard/attitude/`（LCD 实时显示姿态角 + 3D 线框立方体）

## 1. 硬件引脚约定

写 DTS 前固定的信号分配。若实际 layout 不同，只需修改 DTS，无需改驱动。

| 功能 | 引脚 | 说明 |
|------|------|------|
| 控制台 UART1 | PA2 (TX), PA3 (RX) | 115200n8，dtsi 中 `uart1_pa_pins` |
| I2C0 SCL | PD12 | dtsi 中 `i2c0_pd_pins`，100 kHz |
| I2C0 SDA | PD0 | 同上 |
| MPU-6050 INT | PE2 | GPIO 中断，bank E=4 → `interrupts = <4 2 ...>` |
| LCD SPI1 | PE7 (CS), PE8 (MOSI), PE9 (CLK) | ILI9341 只写，MISO 不接（PE10 留空） |
| LCD DC / RESET | PE4 / PE5 | GPIO |
| LCD 背光 | PE12 | PWM0，25 kHz（pwm-backlight） |
| SPI0 NAND | PC0–PC3 | `spi0_pc_pins` |
| TF 卡 mmc0 | PF0–PF5 | `mmc0_pins`（dtsi 默认） |
| USB OTG | USB-DM/DP | peripheral 模式，兼作 FEL 下载口 |
| 电源 | 3.3 V LDO | MPU-6050 VDD 与 VLOGIC 均接 3.3 V |

MPU-6050 侧：AD0 接地（I2C 地址 0x68）；SCL/SDA 需 4.7 kΩ 上拉到 3.3 V
（F1C200s 内部上拉偏弱，I2C 100 kHz 建议外部上拉）；INT 推挽输出，默认
高有效，对应 DTS 中 `IRQ_TYPE_EDGE_RISING`。

EINT 能力（据 `drivers/pinctrl/sunxi/pinctrl-suniv-f1c100s.c`）：PD0–PD21
为中断 bank 0，PE0–PE11 为 bank 1，PF 为 bank 2。INT 选 PE2 避开
SPI1（PE7–PE10）、LCD 控制线（PE4/PE5）与存储（PC/PF）功能位。

### 为什么 LCD 走 SPI 而不是 RGB 并口

F1C 系列的 RGB LCD 数据/时序线是**整个 PD 组（PD0–PD21）**，一旦用
RGB 屏：I2C0 必须从 PD0/PD12 搬去 PE11/PE12（挤掉 PWM 背光位）；更关键
的是**主线内核没有 suniv 的 TCON/DE 显示驱动**（`drivers/gpu/drm/sun4i/`
无 suniv compatible，dtsi 也无显示节点），RGB 路线只能依赖树外补丁。
SPI 屏（ILI9341，tiny DRM `drivers/gpu/drm/tiny/ili9341.c`）全主线支持、
不动已有引脚，320×240@16bpp 在 32 MHz SPI 下整帧刷新约 15 fps，且 DRM
damage 机制只传变化区域，姿态显示场景实际帧率更高。若最终产品必须上
RGB 大屏，可换 SSD1963 一类自带 framebuffer 的控制器或改用树外 TCON
补丁，硬件上把 I2C0 预留到 PE11/PE12。

## 2. Bootloader（U-Boot，树外）

F1C100s/F1C200s 已在 U-Boot 主线支持（`licheepi_nano_defconfig`）。自制板步骤：

```sh
git clone https://source.denx.de/u-boot/u-boot.git && cd u-boot
# 1) 派生 defconfig
cp configs/licheepi_nano_defconfig configs/myboard_f1c200s_defconfig
#    修改其中: CONFIG_DEFAULT_DEVICE_TREE="suniv-f1c200s-myboard"
# 2) 拷贝内核 DTS 到 U-Boot（U-Boot 同步了 suniv dtsi）
cp <kernel>/arch/arm/boot/dts/allwinner/suniv-f1c200s-myboard.dts \
   arch/arm/dts/  # 并在 arch/arm/dts/Makefile 登记
# 3) 构建
make CROSS_COMPILE=arm-linux-gnueabi- myboard_f1c200s_defconfig
make CROSS_COMPILE=arm-linux-gnueabi- -j$(nproc)
# 产物: u-boot-sunxi-with-spl.bin
```

调试期使用 **FEL 模式**（免烧写迭代）：

```sh
# 板上不插 TF/清空 SPI NAND 时上电即进 FEL；或按 FEL 键
lsusb | grep 1f3a:efe8              # Allwinner FEL 设备
sunxi-fel uboot u-boot-sunxi-with-spl.bin \
          write 0x80C00000 zImage \
          write 0x80A00000 suniv-f1c200s-myboard.dtb
# U-Boot 内: bootz 0x80C00000 - 0x80A00000
```

稳定后烧写：TF 卡 `dd if=u-boot-sunxi-with-spl.bin of=/dev/sdX bs=1024 seek=8`；
SPI NAND 用 `sunxi-fel spiflash-write 0 u-boot-sunxi-with-spl.bin`。

## 3. 内核构建（本源码树）

主线无 suniv defconfig，用 `multi_v5_defconfig` + 本板片段合成：

```sh
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- O=build-arm multi_v5_defconfig
./scripts/kconfig/merge_config.sh -O build-arm build-arm/.config myboard/kernel.config
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- O=build-arm olddefconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- O=build-arm -j$(nproc) zImage dtbs modules
```

关键配置见 `myboard/kernel.config` 注释。DTS 已在
`arch/arm/boot/dts/allwinner/Makefile` 登记，`dtbs` 目标会生成
`suniv-f1c200s-myboard.dtb`。

注意：本开发机（x86，无 ARM 交叉工具链）只验证了 DTS 的 cpp 预处理与
Makefile 登记；zImage/DTB 需在装有 `gcc-arm-linux-gnueabi`、`bison`、
`flex`、`libssl-dev` 的环境构建。

## 4. 主线驱动验证（先证明硬件）

启动到 shell 后按序执行，每步通过再进行下一步：

```sh
# 1) I2C 控制器就位
ls /sys/bus/i2c/devices/            # 应有 i2c-0
dmesg | grep mv64xxx                # 控制器 probe 成功

# 2) 总线上扫到器件（需 i2c-tools）
i2cdetect -y 0                      # 0x68 处应显示 UU（已被驱动占用）或 68

# 3) IIO 设备注册
cat /sys/bus/iio/devices/iio:device0/name        # → mpu6050

# 4) 原始数据合理性（静置、Z 轴朝上）
cat /sys/bus/iio/devices/iio:device0/in_accel_z_raw   # ≈ +16384 (±2g 量程)
cat /sys/bus/iio/devices/iio:device0/in_temp_raw      # 换算后≈室温
```

故障定位顺序：`i2cdetect` 扫不到 → 查上拉/焊接/电源；扫到但 IIO 不出现 →
`dmesg | grep inv` 查 WHO_AM_I 失败还是 regulator 缺失；数据全 0 →
传感器未唤醒（PWR_MGMT_1 睡眠位）。

### LCD 验证

内核启用了 `CONFIG_FRAMEBUFFER_CONSOLE`，屏幕接线正确时**开机即可看到
启动日志刷屏**（这是 LCD 链路最直观的自检）。手动确认：

```sh
dmesg | grep -i ili9341            # SPI 面板 probe、分辨率 320x240
ls /dev/fb0                        # DRM fbdev 模拟出的 framebuffer
cat /sys/class/backlight/backlight/brightness   # PWM 背光可调 0-7 档
cat /dev/urandom > /dev/fb0        # 花屏 = 显示通路正常（Ctrl-C 停）
```

## 4b. 姿态显示程序（attitude/）

`myboard/attitude/attitude_cube.c`：读 IIO sysfs 原始值 + scale，互补滤波
解算 roll/pitch（yaw 无磁力计参考，纯陀螺积分会缓慢漂移），在 LCD 上以
25 Hz 绘制三个姿态角数值和一个跟随板子转动的线框立方体（红/绿/蓝线为
机体 X/Y/Z 轴）。主线 inv_mpu6050 与教学驱动 mympu6050 都能配套使用
（按 IIO 设备名自动发现）。

```sh
# 交叉编译并放入 rootfs
cd myboard/attitude && make CROSS_COMPILE=arm-linux-gnueabi-
cp attitude_cube <rootfs>/usr/bin/

# 板上运行（先把 fbcon 从 fb0 上解绑，避免控制台刷新覆盖画面）
echo 0 > /sys/class/vtconsole/vtcon1/bind
attitude_cube
```

无硬件时可在主机上渲染单帧自测：
`./attitude_cube --test out.ppm 15 -25 40`（roll/pitch/yaw，单位度）。

带宽说明：sysfs 单次读走 100 kHz I2C，25 Hz × 6 通道约占总线 24%；
若需更高采样率，改用 `/dev/iio:device0` 缓冲流（配合 data-ready 触发，
一次中断取整帧 14 字节）。

## 5. rootfs（Buildroot，树外）

```sh
git clone https://gitlab.com/buildroot.org/buildroot.git && cd buildroot
make licheepi_nano_defconfig       # 同为 suniv，工具链/内核头适用
make menuconfig
#   Target packages → Hardware handling → i2c-tools     [*]
#   Target packages → Hardware handling → libiio (可选) [*]
#   Kernel → 不由 Buildroot 编内核（我们用本树产物）
make -j$(nproc)
```

TF 卡分区：

```text
p1  16MB  FAT32   zImage + suniv-f1c200s-myboard.dtb + boot.scr
p2  剩余  ext4    Buildroot output/images/rootfs.tar 解压
```

boot.scr 源（`mkimage -C none -A arm -T script -d boot.cmd boot.scr`）：

```text
load mmc 0:1 0x80C00000 zImage
load mmc 0:1 0x80A00000 suniv-f1c200s-myboard.dtb
setenv bootargs console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait
bootz 0x80C00000 - 0x80A00000
```

（`ttyS0` 对应 DTS alias `serial0 = &uart1`。）

## 6. 联调与验证清单

按顺序执行，全部通过即完成 bring-up：

1. **FEL 链路**：`sunxi-fel version` 能读到 SoC ID（0x1663）
2. **U-Boot 控制台**：UART1 出现提示符，`mmc list`/`sf probe` 正常
3. **内核启动**：到 shell，无 panic；`cat /proc/device-tree/model` 显示 MyBoard F1C200s
4. **I2C/传感器**：第 4 节全部通过；示波器确认 SCL 100 kHz
5. **双驱动对照**：
   ```sh
   rmmod inv_mpu6050_i2c inv_mpu6050      # 卸载主线驱动
   insmod mympu6050.ko                    # 教学驱动（见 mympu6050/README.md）
   # 两者读数一致性对比（同为静置状态）
   ```
6. **中断验证**：`cat /proc/interrupts | grep mpu` 计数随数据就绪增长
7. **LCD**：开机 fbcon 出日志；`dmesg | grep ili9341` 无错误；背光可调
8. **姿态显示**：`attitude_cube` 运行，转动板子立方体实时跟随、
   静置后 roll/pitch 回到稳定值（互补滤波收敛），yaw 缓慢漂移属预期
9. **稳定性**：
   ```sh
   while :; do cat /sys/bus/iio/devices/iio:device0/in_accel_*_raw; done
   # 长跑 30 分钟，dmesg 无 i2c timeout/arbitration lost
   ```

## 7. 已知限制

- `myvendor,myboard-f1c200s` 未在 `Documentation/devicetree/bindings/vendor-prefixes.yaml`
  注册；若要上游化需换正式 vendor 前缀并补 board binding。
- F1C200s 的 64 MB DRAM 由 SPL 初始化（licheepi_nano 的 DRAM 参数即适用
  F1C200s；若换 DRAM 型号需调 SPL `CONFIG_DRAM_CLK/ZQ`）。
- mmc0 无热插拔检测（`broken-cd`），按需接 CD 引脚并改 DTS。
