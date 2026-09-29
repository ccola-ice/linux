# Linux I2C 设备驱动框架与实践：从 WHO_AM_I 到 IIO

以 MPU-6050（六轴 IMU，I2C 接口）为例，在 Allwinner F1C200s 自制板上
分三个阶段实现一个完整的现代 I2C client 驱动。配套代码：

- 最终驱动：`mympu6050/mympu6050.c`（阶段 C）
- 阶段 A/B 完整代码：本文第 4、5 节
- 板级 DTS：`arch/arm/boot/dts/allwinner/suniv-f1c200s-myboard.dts`
- 主线对照：`drivers/iio/imu/inv_mpu6050/`

---

## 1. I2C 子系统分层

```text
   用户空间          /sys/bus/iio/...   /dev/iio:deviceN   /dev/i2c-N
                              │                │               │
   ┌──────────────────────────┼────────────────┼───────────────┼──────┐
   │ client 驱动层     IIO 框架│                │        i2c-dev│      │
   │                  ┌───────┴────────────────┴───┐   ┌───────┴────┐ │
   │                  │  mympu6050 / inv_mpu6050   │   │ 用户态直通 │ │
   │                  └──────────┬─────────────────┘   └───────┬────┘ │
   │                        regmap-i2c                         │      │
   ├───────────────────────────┬─┴─────────────────────────────┴──────┤
   │ i2c-core        i2c_transfer() / smbus 模拟 / 设备枚举(DT/ACPI)  │
   ├───────────────────────────┴──────────────────────────────────────┤
   │ adapter 驱动     i2c-mv64xxx（F1C200s/sun6i）、i2c-imx（NXP）…   │
   └──────────────────────────────────────────────────────────────────┘
```

两类驱动职责完全不同：

- **adapter（总线控制器）驱动**：操作 SoC 的 I2C 控制器寄存器，实现
  `i2c_algorithm.xfer`，把 `struct i2c_msg` 序列变成总线上的
  START/地址/数据/STOP 波形。F1C200s 的 I2C0 复用 `i2c-mv64xxx.c`
  （dtsi 中 compatible 回落到 `allwinner,sun6i-a31-i2c`）。
- **client（外设）驱动**：只关心"这个器件有哪些寄存器、什么语义"，
  通过 i2c-core 发事务，完全不接触控制器细节。本文写的就是 client 驱动。

### 设备是怎么"出现"的

现代内核中 client 设备不由驱动创建，而由**固件描述**枚举：设备树的
i2c 控制器节点下写一个子节点，i2c-core 在 adapter 注册时自动为其创建
`struct i2c_client`，再按 `compatible` 匹配驱动并调用 probe：

```text
&i2c0 {                                   ← adapter（PD12=SCL, PD0=SDA）
	status = "okay";
	imu@68 {                          ← client，地址 0x68
		compatible = "invensense,mpu6050";
		reg = <0x68>;
		interrupt-parent = <&pio>;
		interrupts = <4 2 IRQ_TYPE_EDGE_RISING>;  /* PE2 */
		vdd-supply = <&reg_vcc3v3>;
		vddio-supply = <&reg_vcc3v3>;
	};
};
```

`client->irq` 由 i2c-core 从 `interrupts` 属性解析好，驱动直接用。

### regmap：寄存器访问的标准抽象

裸用 `i2c_smbus_read_byte_data()` 也能工作，但现代驱动几乎都套一层
regmap（`devm_regmap_init_i2c`）：

- 统一 API：同一份驱动逻辑可在 I2C/SPI 版本器件间复用（MPU-6000 是
  SPI 版，主线 inv_mpu6050 正是靠 regmap 同时支持两者）；
- 免费获得 debugfs 寄存器 dump、可选缓存、字段抽象（`regmap_field`）；
- `regmap_bulk_read` 天然映射为 I2C 的"写寄存器地址 + 连续读"burst 事务。

---

## 2. MPU-6050 硬件模型

驱动要用到的最小寄存器集（RM-MPU-6000A-00）：

| 寄存器 | 地址 | 用途 |
|--------|------|------|
| SMPLRT_DIV | 0x19 | 采样率 = 陀螺输出率/(1+DIV) |
| CONFIG | 0x1A | DLPF 数字低通滤波（决定陀螺输出率 8kHz/1kHz） |
| GYRO_CONFIG | 0x1B | FS_SEL 量程（0=±250dps → 131 LSB/(°/s)） |
| ACCEL_CONFIG | 0x1C | AFS_SEL 量程（0=±2g → 16384 LSB/g） |
| INT_PIN_CFG | 0x37 | INT 引脚极性/锁存（默认高有效 50µs 脉冲） |
| INT_ENABLE | 0x38 | bit0 DATA_RDY_EN |
| ACCEL_XOUT_H | 0x3B | **14 字节 burst 起点**：AX AY AZ T GX GY GZ，大端 |
| PWR_MGMT_1 | 0x6B | bit7 复位，bit6 SLEEP（**上电默认=1**），CLKSEL |
| WHO_AM_I | 0x75 | 固定 0x68 |

三个对驱动结构有决定性影响的硬件事实：

1. **上电即睡眠**：不清 SLEEP 位，所有数据寄存器恒为 0 —— probe 必须唤醒。
2. **burst 读的一致性保证**：器件内部对输出寄存器做了影子锁存，一次
   连续读 0x3B..0x48 得到的 7 个量属于同一采样点；分开逐个读则可能跨采样点。
3. **data-ready 中断**：每个采样周期 INT 脉冲一次，天然对应 IIO 的
   hardware trigger 模型。

---

## 3. 三个阶段的路线图

| 阶段 | 新增能力 | 学到的机制 |
|------|---------|-----------|
| A | probe 里认出器件并唤醒 | i2c_driver 骨架、DT 匹配、regmap、dev_err_probe |
| B | 读出三轴数据到 sysfs | burst 事务、字节序、DEVICE_ATTR、dev_groups |
| C | 接入 IIO：标准接口 + 环形缓冲 + 硬件触发 | channel/scan/trigger/buffer、claim_direct、devm 生命周期 |

---

## 4. 阶段 A：probe + WHO_AM_I + 唤醒

完整代码（可直接编译）：

```c
// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

#define MPU6050_REG_PWR_MGMT_1		0x6b
#define MPU6050_REG_WHO_AM_I		0x75

#define MPU6050_PWR1_DEVICE_RESET	BIT(7)
#define MPU6050_PWR1_SLEEP		BIT(6)
#define MPU6050_PWR1_CLKSEL_PLL_XGYRO	0x01

#define MPU6050_WHO_AM_I_VAL		0x68

struct mympu6050 {
	struct i2c_client *client;
	struct regmap *map;
	struct mutex lock;
};

static const struct regmap_config mympu6050_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MPU6050_REG_WHO_AM_I,
};

static int mympu6050_hw_init(struct mympu6050 *st)
{
	unsigned int val;
	int ret;

	ret = regmap_write(st->map, MPU6050_REG_PWR_MGMT_1,
			   MPU6050_PWR1_DEVICE_RESET);
	if (ret)
		return ret;
	msleep(100);

	ret = regmap_write(st->map, MPU6050_REG_PWR_MGMT_1,
			   MPU6050_PWR1_CLKSEL_PLL_XGYRO);
	if (ret)
		return ret;
	msleep(30);

	ret = regmap_read(st->map, MPU6050_REG_PWR_MGMT_1, &val);
	if (ret)
		return ret;
	if (val & MPU6050_PWR1_SLEEP) {
		dev_err(&st->client->dev, "still in sleep (0x%02x)\n", val);
		return -EIO;
	}
	return 0;
}

static int mympu6050_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mympu6050 *st;
	unsigned int who;
	int ret;

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	st->client = client;
	mutex_init(&st->lock);
	i2c_set_clientdata(client, st);

	st->map = devm_regmap_init_i2c(client, &mympu6050_regmap_config);
	if (IS_ERR(st->map))
		return dev_err_probe(dev, PTR_ERR(st->map),
				     "regmap init failed\n");

	ret = regmap_read(st->map, MPU6050_REG_WHO_AM_I, &who);
	if (ret)
		return dev_err_probe(dev, ret, "WHO_AM_I read failed\n");
	if (who != MPU6050_WHO_AM_I_VAL)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected WHO_AM_I 0x%02x\n", who);

	ret = mympu6050_hw_init(st);
	if (ret)
		return dev_err_probe(dev, ret, "hw init failed\n");

	dev_info(dev, "MPU-6050 at 0x%02x is awake\n", client->addr);
	return 0;
}

static void mympu6050_remove(struct i2c_client *client)
{
	struct mympu6050 *st = i2c_get_clientdata(client);

	regmap_write(st->map, MPU6050_REG_PWR_MGMT_1, MPU6050_PWR1_SLEEP);
}

static const struct of_device_id mympu6050_of_match[] = {
	{ .compatible = "myvendor,mympu6050" },
	{ .compatible = "invensense,mpu6050" },
	{ }
};
MODULE_DEVICE_TABLE(of, mympu6050_of_match);

static const struct i2c_device_id mympu6050_id[] = {
	{ "mympu6050" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mympu6050_id);

static struct i2c_driver mympu6050_driver = {
	.driver = {
		.name = "mympu6050",
		.of_match_table = mympu6050_of_match,
	},
	.probe = mympu6050_probe,
	.remove = mympu6050_remove,
	.id_table = mympu6050_id,
};
module_i2c_driver(mympu6050_driver);

MODULE_DESCRIPTION("Educational MPU-6050 I2C driver (stage A)");
MODULE_LICENSE("GPL");
```

### 讲解

- **probe 签名**：现代 `i2c_driver.probe` 是 `int (*)(struct i2c_client *)`，
  不再带 `i2c_device_id *` 参数（旧接口 `probe` 带 id，2023 年起全树迁移完毕）。
- **module_i2c_driver()**：展开为 module_init/exit 里的
  `i2c_add_driver/i2c_del_driver`，八行样板一宏搞定。
- **两张匹配表**：`of_match_table` 服务设备树路径（实际生效的一条）；
  `i2c_device_id` 表服务旧式 board_info/用户态
  `echo mympu6050 0x68 > /sys/bus/i2c/devices/i2c-0/new_device` 手工实例化，
  同时决定 modalias，两者都应提供。
- **dev_err_probe()**：一行完成"打印 + 返回错误码"，并正确处理
  `-EPROBE_DEFER`（不刷日志、记录 deferred 原因到
  `/sys/kernel/debug/devices_deferred`）。
- **devm_ 资源**：内存、regmap 都挂在 device 上，probe 失败或 unbind 时
  自动逆序释放，remove 里无需任何 free —— 这是现代驱动几乎不写
  错误回滚代码的原因。
- **WHO_AM_I 先行**：在做任何写操作前先确认器件在位且是预期型号，
  探不到就 `-ENODEV` 干净退出，这让同一 DTS 在贴片失败的板子上不会
  产生半初始化状态。

---

## 5. 阶段 B：burst 读 + sysfs

在阶段 A 之上新增三块（相对 diff，完整版本可由 A + 以下片段还原）：

### 5.1 测量配置（hw_init 末尾追加）

```c
	/* DLPF=1: 陀螺输出 1kHz、带宽 ~188Hz；SMPLRT_DIV=9 → 100Hz 采样 */
	ret = regmap_write(st->map, MPU6050_REG_CONFIG, 0x01);
	if (ret)
		return ret;
	ret = regmap_write(st->map, MPU6050_REG_SMPLRT_DIV, 9);
	if (ret)
		return ret;
	ret = regmap_write(st->map, MPU6050_REG_GYRO_CONFIG, 0x00);  /* ±250dps */
	if (ret)
		return ret;
	return regmap_write(st->map, MPU6050_REG_ACCEL_CONFIG, 0x00); /* ±2g */
```

### 5.2 burst 读

```c
#define MPU6050_REG_ACCEL_XOUT_H	0x3b
#define MPU6050_BURST_LEN		14

struct mympu6050_sample {
	s16 accel[3];
	s16 temp;
	s16 gyro[3];
};

static int mympu6050_read_sample(struct mympu6050 *st,
				 struct mympu6050_sample *s)
{
	u8 raw[MPU6050_BURST_LEN];
	int ret, i;

	mutex_lock(&st->lock);
	ret = regmap_bulk_read(st->map, MPU6050_REG_ACCEL_XOUT_H,
			       raw, sizeof(raw));
	mutex_unlock(&st->lock);
	if (ret)
		return ret;

	for (i = 0; i < 3; i++)
		s->accel[i] = get_unaligned_be16(raw + 2 * i);
	s->temp = get_unaligned_be16(raw + 6);
	for (i = 0; i < 3; i++)
		s->gyro[i] = get_unaligned_be16(raw + 8 + 2 * i);
	return 0;
}
```

要点：

- `regmap_bulk_read` 在 I2C 上就是一次
  `S addr W 0x3B  Sr addr R d0..d13 P` 组合事务 —— 原子地取走同一
  采样点的 7 个量；
- 数据是**大端**（`get_unaligned_be16`），且 raw 缓冲不保证对齐，
  用 unaligned 访问器（`<linux/unaligned.h>`）；
- 互斥锁保护的是**事务序列**层面（防止未来的模式切换与读交错），
  单次 regmap 调用本身已由 regmap 内部锁保护。

### 5.3 sysfs 导出

```c
static ssize_t accel_raw_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct mympu6050 *st = i2c_get_clientdata(to_i2c_client(dev));
	struct mympu6050_sample s;
	int ret;

	ret = mympu6050_read_sample(st, &s);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%d %d %d\n", s.accel[0], s.accel[1], s.accel[2]);
}
static DEVICE_ATTR_RO(accel_raw);
/* gyro_raw、temp_milli 同理 */

static struct attribute *mympu6050_attrs[] = {
	&dev_attr_accel_raw.attr,
	&dev_attr_gyro_raw.attr,
	&dev_attr_temp_milli.attr,
	NULL
};
ATTRIBUTE_GROUPS(mympu6050);

static struct i2c_driver mympu6050_driver = {
	.driver = {
		...
		.dev_groups = mympu6050_groups,   /* 绑定时自动创建，无竞态 */
	},
	...
};
```

要点：

- 输出用 `sysfs_emit()` 而非 sprintf（带 PAGE_SIZE 边界保护，checkpatch 强制）；
- 属性组挂在 `driver.dev_groups`，由 driver core 在 bind/unbind 时自动
  创建/删除 —— 比在 probe 里手动 `sysfs_create_group` 优雅且天然避免
  "attribute 先于 probe 完成可见"的竞态；
- sysfs 一项一值是惯例，这里"x y z"三值一行是教学期权宜 ——
  **正是这种 ad-hoc 接口的不可移植性，引出了阶段 C 的 IIO**。

---

## 6. 阶段 C：IIO 化（最终版 `mympu6050/mympu6050.c`）

### 6.1 为什么是 IIO

sysfs 自定义属性谁都能写，但每个传感器驱动各造一套名字/单位/格式，
用户空间无法通用。IIO（Industrial I/O）统一了：

- **命名与单位**：`in_accel_x_raw`、`in_anglvel_y_raw`、
  `processed = (raw + offset) × scale`，加速度恒为 m/s²、角速度恒为 rad/s；
- **数据通路**：单次读走 sysfs；连续流走触发 + 环形缓冲 + `/dev/iio:deviceN`；
- **生态**：libiio、iio-sensor-proxy、iio_readdev 等现成工具直接可用。

### 6.2 channel 描述：驱动的"数据模式声明"

```c
#define MYMPU6050_MOD_CHAN(_type, _mod, _idx) {			\
	.type = (_type),					\
	.modified = 1,						\
	.channel2 = (_mod),					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.scan_index = (_idx),					\
	.scan_type = {						\
		.sign = 's', .realbits = 16,			\
		.storagebits = 16, .endianness = IIO_BE,	\
	},							\
}
```

- `modified + channel2 = IIO_MOD_X/Y/Z`：同类型多轴的标准表达，
  生成 `in_accel_x_raw` 等文件名；
- `info_mask_separate` 每通道一个 RAW；`shared_by_type` 三轴共享一个
  SCALE（`in_accel_scale`）—— mask 的 separate/shared 之分直接决定
  sysfs 文件的粒度；
- `scan_index` 决定缓冲内的通道顺序。**故意让它等于 burst 布局
  （accel 0-2, temp 3, gyro 4-6）**，触发处理函数便可把 14 字节原样
  推入缓冲；`endianness = IIO_BE` 声明"数据是大端"，转换推给用户空间，
  内核路径零拷贝零转换。

### 6.3 read_raw：单次读与量纲

```c
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = mympu6050_read_sample(st, &s);
		iio_device_release_direct(indio_dev);
		...
```

`iio_device_claim_direct()`（当前主线接口，返回 bool）保证单次读与
buffer 模式互斥：缓冲使能期间 `cat in_accel_x_raw` 得到 `-EBUSY`，
而不是和触发处理函数抢 I2C 总线。

量纲换算全部声明给框架，驱动不做浮点：

| 通道 | 返回 | 含义 |
|------|------|------|
| accel scale | `INT_PLUS_NANO {0, 598550}` | 9.80665/16384 m/s² per LSB |
| anglvel scale | `INT_PLUS_NANO {0, 133158}` | (250/32768)·(π/180) rad/s per LSB |
| temp scale/offset | `{2, 941176}` / `12420` | (raw+12420)×2.941176 m°C ⇔ raw/340+36.53 °C |

### 6.4 triggered buffer：连续采集通路

```c
static irqreturn_t mympu6050_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct mympu6050 *st = iio_priv(indio_dev);
	IIO_DECLARE_BUFFER_WITH_TS(u8, buf, MPU6050_BURST_LEN) = { };
	int ret;

	mutex_lock(&st->lock);
	ret = regmap_bulk_read(st->map, MPU6050_REG_ACCEL_XOUT_H,
			       buf, MPU6050_BURST_LEN);
	mutex_unlock(&st->lock);
	if (!ret)
		iio_push_to_buffers_with_ts(indio_dev, buf, sizeof(buf),
					    pf->timestamp);

	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

	/* probe 中： */
	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      mympu6050_trigger_handler, NULL);
```

- `IIO_DECLARE_BUFFER_WITH_TS`：当前主线宏，自动在数据后追加 8 字节
  对齐的时间戳空间（旧驱动手写 `__aligned(8)` 结构体的替代品）；
- 处理函数运行在**线程化中断上下文**（triggered buffer 的 bottom half），
  可以睡眠，直接做 I2C 事务没问题 —— 这正是它不放在硬中断里的原因；
- `iio_pollfunc_store_time` 是框架提供的 top half：触发瞬间先记时间戳，
  保证时间戳反映的是采样时刻而非 I2C 读完成时刻。

### 6.5 data-ready 硬件触发器

```c
static int mympu6050_data_rdy_set_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct mympu6050 *st = iio_priv(indio_dev);

	return regmap_write(st->map, MPU6050_REG_INT_ENABLE,
			    state ? MPU6050_INT_DATA_RDY_EN : 0);
}
```

链路：器件每采样一次拉高 INT 50µs → GPIO 边沿中断（DTS
`interrupts = <4 2 IRQ_TYPE_EDGE_RISING>`，PE2）→
`iio_trigger_generic_data_rdy_poll`（框架通用 ISR）→ 触发所有挂在该
trigger 上的 pollfunc → 上面的 trigger_handler 读一帧。

细节：

- `set_trigger_state` 是触发器的开关回调：用户空间 `echo 1 > buffer/enable`
  时框架才调它打开 DATA_RDY_EN —— 中断按需产生，不采集时总线安静；
- INT_PIN_CFG 保持默认（非锁存 50µs 脉冲 + 高有效），配边沿触发中断，
  **无需读 INT_STATUS 清中断**，省一次 I2C 事务；
- `client->irq <= 0`（DTS 没接 INT）时跳过触发器注册，设备仍可用
  sysfs-trigger/hrtimer 软触发采集 —— 硬件能力可选，驱动分级降级。

### 6.6 devm 化的生命周期（本驱动没有 remove）

```c
	ret = devm_add_action_or_reset(dev, mympu6050_sleep, st);  /* ① */
	...
	ret = devm_iio_triggered_buffer_setup(...);                /* ② */
	...
	ret = devm_iio_device_register(dev, indio_dev);            /* ③ */
```

devres 释放严格逆序（③→②→①）：unbind 时先注销 IIO 设备（用户空间
文件全部失效）、再拆缓冲、**最后**才执行 ① 把器件写回 SLEEP。若把
睡眠写在 `.remove` 里，执行时 IIO 设备还活着，用户空间可能并发读到
睡眠后的全 0 数据。"清理动作注册在资源创建之前"是 devm 驱动排序的
核心原则。

---

## 7. 与主线 inv_mpu6050 的对照

| 维度 | mympu6050（教学） | inv_mpu6050（主线） |
|------|------------------|---------------------|
| 器件支持 | 仅 MPU-6050 | MPU-6000/6050/6500/9x50/ICM206xx 全家族（hw_info 表抽象） |
| 总线 | I2C | I2C + SPI（regmap 双后端）+ i2c-gate 级联磁力计 |
| 数据通路 | 每中断一次 burst 读 | 硬件 FIFO 批量搬运（降低中断率与 I2C 占用） |
| 电源管理 | probe 唤醒/unbind 睡眠 | runtime PM 按需上电 + system sleep |
| 量程 | 固定 ±2g/±250dps | write_raw 动态可调 + 校准偏置寄存器 |
| 其他 | — | mount-matrix、WoM 唤醒事件、debugfs |

读主线代码的建议切入点：`inv_mpu_core.c` 的 `inv_mpu_core_probe()`
（对应本文 probe 的工业级版本）与 `inv_mpu_ring.c`（FIFO 版的
trigger_handler）。

---

## 8. 上板验证

构建/加载/双驱动对照/中断与稳定性测试步骤，见：

- `mympu6050/README.md` —— 模块构建与 IIO 接口操作
- `myboard/README.md` 第 4、6 节 —— 主线驱动先行验证硬件、联调清单

核心顺序永远是：**i2cdetect 扫到 0x68 →主线驱动读数合理 → 换教学驱动
对照读数 → 中断计数增长 → 长跑无 I2C 超时**。硬件问题在第一步暴露，
驱动问题在对照步暴露，绝不混在一起排查。
