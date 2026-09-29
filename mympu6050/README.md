# mympu6050 — 教学用 MPU-6050 I2C/IIO 驱动

配套教程：源码树根目录 `linux-i2c-driver-framework.md`（含阶段 A/B 完整代码与逐段讲解）。
目标硬件：`myboard/README.md` 描述的 F1C200s 自制板（I2C0 = PD12/PD0，INT = PE2）。
主线对照实现：`drivers/iio/imu/inv_mpu6050/`。

## 分阶段演进

| 阶段 | 内容 | 本目录状态 |
|------|------|-----------|
| A | i2c_driver 骨架 + regmap + WHO_AM_I + 复位唤醒 | 代码见教程 |
| B | 量程/采样率配置 + 14 字节 burst 读 + sysfs | 代码见教程 |
| C | IIO channel + read_raw + triggered buffer + data-ready 触发 | `mympu6050.c`（最终版） |

## 构建

```sh
# 针对本源码树（默认，需先在树内完成过一次内核构建以生成 Module.symvers）
make

# 交叉编译到目标板（ARM 内核构建目录）
make KDIR=/path/to/linux/build-arm ARCH=arm CROSS_COMPILE=arm-linux-gnueabi-
```

本模块只依赖 I2C/regmap/IIO 通用 API，无平台相关代码，x86 与 ARM 均可编译。

## 目标板上使用

设备树节点（`suniv-f1c200s-myboard.dts` 已包含）compatible 为
`invensense,mpu6050`，本驱动同样声明了该 compatible，因此**卸载主线驱动后
直接加载本模块即可绑定**，无需改 DTS：

```sh
rmmod inv_mpu6050_i2c inv_mpu6050    # 如为内建则需在 DTS 改用 myvendor,mympu6050
insmod mympu6050.ko
dmesg | tail -2      # → MPU-6050 at 0x68 registered as mympu6050 (irq N)
```

### 单次读（sysfs，IIO 标准接口）

```sh
cd /sys/bus/iio/devices/iio:device0
cat name                    # mympu6050
cat in_accel_z_raw          # 静置 Z 朝上 ≈ 16384
cat in_accel_scale          # 0.000598550  (m/s² per LSB)
cat in_temp_raw in_temp_offset in_temp_scale
# 温度(m°C) = (raw + offset) * scale
```

### 连续采集（triggered buffer + data-ready 中断）

```sh
cd /sys/bus/iio/devices/iio:device0
cat trigger/current_trigger          # mympu6050-dev0（DT 有 interrupts 时自动设置）
for c in accel_x accel_y accel_z anglvel_x anglvel_y anglvel_z timestamp; do
	echo 1 > scan_elements/in_${c}_en 2>/dev/null || echo 1 > scan_elements/in_${c}_index >/dev/null
done
echo 1 > buffer/enable
hexdump -e '7/2 "%6d" 1/8 " %d\n"' /dev/iio:device0   # 100Hz 数据流（大端原始值）
echo 0 > buffer/enable
cat /proc/interrupts | grep mympu6050   # 中断计数随采样增长
```

无 INT 连线时模块同样可用：用软件触发
（`CONFIG_IIO_SYSFS_TRIGGER`，`echo 0 > /sys/bus/iio/devices/iio_sysfs_trigger/add_trigger`）
或 hrtimer 触发替代 data-ready。

## 设计要点（详见教程）

- **burst 读**：0x3B 起一次 I2C 事务读 14 字节，保证 7 个量同采样点，
  且寄存器高/低字节对是锁存一致的。
- **scan_type IIO_BE**：缓冲直接推原始大端字节，字节序转换留给用户空间。
- **claim_direct**：`read_raw` 与 buffer 模式互斥，避免与触发处理竞争总线。
- **devm 化生命周期**：无 `.remove`；睡眠动作用 `devm_add_action_or_reset`
  注册在 IIO register 之前，保证注销后才把器件送回睡眠。
- **中断免清除**：INT_PIN_CFG 默认 50µs 非锁存脉冲 + DTS `IRQ_TYPE_EDGE_RISING`，
  中断处理无需读 INT_STATUS。
