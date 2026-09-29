// SPDX-License-Identifier: GPL-2.0
/*
 * mympu6050 - 教学用 MPU-6050 I2C 驱动，阶段 C（最终形态）
 *
 * 阶段 A：probe/remove 生命周期 + regmap + WHO_AM_I 校验 + 复位唤醒
 * 阶段 B：量程/采样率配置 + 14 字节 burst 读 + sysfs 只读属性
 * 阶段 C（本阶段）：
 *   1. 接入 IIO 框架：channel 描述 + read_raw（RAW/SCALE/OFFSET）
 *      —— 用户空间获得标准接口 /sys/bus/iio/devices/iio:deviceN/
 *   2. triggered buffer：一次 burst 读推入环形缓冲，配合 /dev/iio:deviceN
 *   3. 可选 data-ready 硬件中断触发器（DT 中提供 interrupts 时启用）
 *
 * 各阶段完整代码见 linux-i2c-driver-framework.md；
 * 对照主线实现：drivers/iio/imu/inv_mpu6050/
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>

#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

/* ---- 寄存器定义（MPU-6000/6050 Register Map, RM-MPU-6000A-00） ---- */
#define MPU6050_REG_SMPLRT_DIV		0x19
#define MPU6050_REG_CONFIG		0x1a
#define MPU6050_REG_GYRO_CONFIG		0x1b
#define MPU6050_REG_ACCEL_CONFIG	0x1c
#define MPU6050_REG_INT_ENABLE		0x38
#define MPU6050_REG_ACCEL_XOUT_H	0x3b	/* 14 字节 burst 起点 */
#define MPU6050_REG_PWR_MGMT_1		0x6b
#define MPU6050_REG_WHO_AM_I		0x75

#define MPU6050_PWR1_DEVICE_RESET	BIT(7)
#define MPU6050_PWR1_SLEEP		BIT(6)
#define MPU6050_PWR1_CLKSEL_PLL_XGYRO	0x01	/* 推荐时钟源：X 轴陀螺 PLL */

#define MPU6050_INT_DATA_RDY_EN		BIT(0)

#define MPU6050_WHO_AM_I_VAL		0x68

/* 0x3B..0x48：AXH..AZL, TH, TL, GXH..GZL，共 14 字节 */
#define MPU6050_BURST_LEN		14

/* 采样数据（主机字节序，已从大端转换） */
struct mympu6050_sample {
	s16 accel[3];
	s16 temp;
	s16 gyro[3];
};

struct mympu6050 {
	struct i2c_client *client;
	struct regmap *map;
	/* 串行化多寄存器操作序列（复位、模式切换、burst 读） */
	struct mutex lock;
};

static const struct regmap_config mympu6050_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MPU6050_REG_WHO_AM_I,
};

/* 一次事务读出同一采样点的全部 7 个量 */
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

/* ---- IIO channel 描述 ----
 *
 * scan_index 顺序 = burst 读的内存布局（accel, temp, gyro），
 * 这样触发处理函数可以把 14 字节原样推入缓冲，零拷贝转换；
 * scan_type 标记 IIO_BE，字节序转换交给用户空间（libiio 等）。
 */
#define MYMPU6050_MOD_CHAN(_type, _mod, _idx)				\
{									\
	.type = (_type),						\
	.modified = 1,							\
	.channel2 = (_mod),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.scan_index = (_idx),						\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = 16,						\
		.storagebits = 16,					\
		.endianness = IIO_BE,					\
	},								\
}

static const struct iio_chan_spec mympu6050_channels[] = {
	MYMPU6050_MOD_CHAN(IIO_ACCEL, IIO_MOD_X, 0),
	MYMPU6050_MOD_CHAN(IIO_ACCEL, IIO_MOD_Y, 1),
	MYMPU6050_MOD_CHAN(IIO_ACCEL, IIO_MOD_Z, 2),
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.scan_index = 3,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_BE,
		},
	},
	MYMPU6050_MOD_CHAN(IIO_ANGL_VEL, IIO_MOD_X, 4),
	MYMPU6050_MOD_CHAN(IIO_ANGL_VEL, IIO_MOD_Y, 5),
	MYMPU6050_MOD_CHAN(IIO_ANGL_VEL, IIO_MOD_Z, 6),
	IIO_CHAN_SOFT_TIMESTAMP(7),
};

static int mympu6050_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	struct mympu6050 *st = iio_priv(indio_dev);
	struct mympu6050_sample s;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/* 与 buffer 模式互斥：buffer 使能期间拒绝单次读 */
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = mympu6050_read_sample(st, &s);
		iio_device_release_direct(indio_dev);
		if (ret)
			return ret;

		switch (chan->type) {
		case IIO_ACCEL:
			*val = s.accel[chan->scan_index];
			break;
		case IIO_TEMP:
			*val = s.temp;
			break;
		case IIO_ANGL_VEL:
			*val = s.gyro[chan->scan_index - 4];
			break;
		default:
			return -EINVAL;
		}
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			/* ±2g: 16384 LSB/g → 9.80665/16384 m/s² per LSB */
			*val = 0;
			*val2 = 598550;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_ANGL_VEL:
			/* ±250dps: 131.072 LSB/(°/s) → ×π/180 rad/s per LSB */
			*val = 0;
			*val2 = 133158;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_TEMP:
			/* Temp(°C) = raw/340 + 36.53 → 毫摄氏度斜率 1000/340 */
			*val = 2;
			*val2 = 941176;
			return IIO_VAL_INT_PLUS_MICRO;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:
		/* (raw + offset) * scale：36.53 * 340 ≈ 12420 */
		*val = 12420;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static const struct iio_info mympu6050_info = {
	.read_raw = mympu6050_read_raw,
};

/* ---- triggered buffer：触发到来时 burst 读一帧并推入环形缓冲 ---- */

static irqreturn_t mympu6050_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct mympu6050 *st = iio_priv(indio_dev);
	/* 14 字节数据 + 8 字节对齐的时间戳空间 */
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

/* ---- 可选 data-ready 触发器（需要 DT 中的 interrupts 属性） ----
 *
 * INT_PIN_CFG 保持默认：INT 引脚推挽、高有效、50µs 脉冲（非锁存），
 * 因此 DTS 用 IRQ_TYPE_EDGE_RISING，且无需读 INT_STATUS 清中断。
 */

static int mympu6050_data_rdy_set_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct mympu6050 *st = iio_priv(indio_dev);

	return regmap_write(st->map, MPU6050_REG_INT_ENABLE,
			    state ? MPU6050_INT_DATA_RDY_EN : 0);
}

static const struct iio_trigger_ops mympu6050_trigger_ops = {
	.set_trigger_state = mympu6050_data_rdy_set_state,
};

static int mympu6050_setup_trigger(struct iio_dev *indio_dev)
{
	struct mympu6050 *st = iio_priv(indio_dev);
	struct device *dev = &st->client->dev;
	struct iio_trigger *trig;
	int ret;

	trig = devm_iio_trigger_alloc(dev, "%s-dev%d", indio_dev->name,
				      iio_device_id(indio_dev));
	if (!trig)
		return -ENOMEM;

	trig->ops = &mympu6050_trigger_ops;
	iio_trigger_set_drvdata(trig, indio_dev);

	/* 触发类型（沿/极性）来自设备树 interrupts 属性，故 flags 传 0 */
	ret = devm_request_irq(dev, st->client->irq,
			       iio_trigger_generic_data_rdy_poll, 0,
			       "mympu6050", trig);
	if (ret)
		return ret;

	ret = devm_iio_trigger_register(dev, trig);
	if (ret)
		return ret;

	/* 设为本设备默认触发器，用户空间无须手动 current_trigger */
	indio_dev->trig = iio_trigger_get(trig);
	return 0;
}

/* ---- 硬件初始化与生命周期 ---- */

static int mympu6050_hw_init(struct mympu6050 *st)
{
	unsigned int val;
	int ret;

	/* 复位所有寄存器到默认值；手册要求等待 ~100ms */
	ret = regmap_write(st->map, MPU6050_REG_PWR_MGMT_1,
			   MPU6050_PWR1_DEVICE_RESET);
	if (ret)
		return ret;
	msleep(100);

	/*
	 * 唤醒：清 SLEEP 位。同时把时钟源从默认的内部 8MHz RC
	 * 切到 X 轴陀螺 PLL（手册推荐，精度更高）。
	 */
	ret = regmap_write(st->map, MPU6050_REG_PWR_MGMT_1,
			   MPU6050_PWR1_CLKSEL_PLL_XGYRO);
	if (ret)
		return ret;
	msleep(30);	/* 等 PLL 稳定 */

	ret = regmap_read(st->map, MPU6050_REG_PWR_MGMT_1, &val);
	if (ret)
		return ret;
	if (val & MPU6050_PWR1_SLEEP) {
		dev_err(&st->client->dev, "device still in sleep (0x%02x)\n", val);
		return -EIO;
	}

	/*
	 * 基础测量配置。
	 * CONFIG.DLPF_CFG=1 → 陀螺输出 1kHz、带宽 ~188Hz
	 * SMPLRT_DIV=9      → 采样率 = 1kHz/(1+9) = 100Hz
	 * 量程用默认 FS_SEL=0（±250dps）/ AFS_SEL=0（±2g），
	 * 显式写一遍以摆脱对复位默认值的隐式依赖。
	 */
	ret = regmap_write(st->map, MPU6050_REG_CONFIG, 0x01);
	if (ret)
		return ret;
	ret = regmap_write(st->map, MPU6050_REG_SMPLRT_DIV, 9);
	if (ret)
		return ret;
	ret = regmap_write(st->map, MPU6050_REG_GYRO_CONFIG, 0x00);
	if (ret)
		return ret;
	return regmap_write(st->map, MPU6050_REG_ACCEL_CONFIG, 0x00);
}

/*
 * devm action：在 IIO 设备注销之后（devres 后进先出）把器件送回睡眠。
 * 若放在 ->remove 里，执行时 IIO 设备尚未注销，可能与用户空间并发。
 */
static void mympu6050_sleep(void *data)
{
	struct mympu6050 *st = data;

	regmap_write(st->map, MPU6050_REG_PWR_MGMT_1, MPU6050_PWR1_SLEEP);
}

static int mympu6050_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct mympu6050 *st;
	unsigned int who;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;
	st = iio_priv(indio_dev);
	st->client = client;
	mutex_init(&st->lock);

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

	ret = devm_add_action_or_reset(dev, mympu6050_sleep, st);
	if (ret)
		return ret;

	indio_dev->name = "mympu6050";
	indio_dev->channels = mympu6050_channels;
	indio_dev->num_channels = ARRAY_SIZE(mympu6050_channels);
	indio_dev->info = &mympu6050_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      mympu6050_trigger_handler, NULL);
	if (ret)
		return dev_err_probe(dev, ret, "buffer setup failed\n");

	if (client->irq > 0) {
		ret = mympu6050_setup_trigger(indio_dev);
		if (ret)
			return dev_err_probe(dev, ret, "trigger setup failed\n");
	}

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "iio register failed\n");

	dev_info(dev, "MPU-6050 at 0x%02x registered as %s (irq %d)\n",
		 client->addr, indio_dev->name, client->irq);
	return 0;
}

static const struct of_device_id mympu6050_of_match[] = {
	{ .compatible = "myvendor,mympu6050" },
	{ .compatible = "invensense,mpu6050" }, /* 可直接顶替主线驱动绑定 */
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
	.id_table = mympu6050_id,
};
module_i2c_driver(mympu6050_driver);

MODULE_AUTHOR("zhangb");
MODULE_DESCRIPTION("Educational MPU-6050 IIO driver (stage C)");
MODULE_LICENSE("GPL");
