// SPDX-License-Identifier: GPL-2.0
/*
 * attitude_cube - 在 fbdev 上实时显示 MPU-6050 姿态角与线框立方体
 *
 * 数据通路：IIO sysfs（in_accel_*_raw / in_anglvel_*_raw + scale）
 * 姿态解算：互补滤波
 *   roll/pitch = alpha*(角速度积分) + (1-alpha)*(加速度反解)   —— 长期稳定
 *   yaw        = 纯陀螺积分（无磁力计，会缓慢漂移）
 * 渲染：320x240 RGB565 画布 → /dev/fb0（16bpp 直拷，32bpp 展开）
 *
 * 用法：
 *   attitude_cube                       # 板上实时运行（自动找 mpu6050 IIO 设备）
 *   attitude_cube --test out.ppm R P Y  # 主机自测：按给定角度(度)渲染一帧 PPM
 *
 * 构建：make            （主机自测）
 *       make CROSS_COMPILE=arm-linux-gnueabi-   （目标板）
 */

#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CANVAS_W	320
#define CANVAS_H	240

#define COL_BG		0x0000		/* 黑 */
#define COL_TEXT	0xFFFF		/* 白 */
#define COL_LABEL	0x07FF		/* 青 */
#define COL_EDGE	0xFFFF		/* 立方体棱：白 */
#define COL_XAXIS	0xF800		/* 红 */
#define COL_YAXIS	0x07E0		/* 绿 */
#define COL_ZAXIS	0x001F		/* 蓝 */

static uint16_t canvas[CANVAS_W * CANVAS_H];

/* ---------------- 5x7 点阵字体（自绘，bit4 为最左像素） ---------------- */

struct glyph { char c; uint8_t rows[7]; };

static const struct glyph font[] = {
	{ '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
	{ '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ '2', { 0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F } },
	{ '3', { 0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E } },
	{ '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
	{ '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
	{ '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
	{ '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
	{ '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
	{ '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
	{ '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
	{ ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } },
	{ 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
	{ 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
	{ 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
	{ 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
	{ 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
	{ 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
	{ 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
	{ ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
};

static const uint8_t *glyph_rows(char c)
{
	size_t i;

	for (i = 0; i < sizeof(font) / sizeof(font[0]); i++)
		if (font[i].c == c)
			return font[i].rows;
	return font[sizeof(font) / sizeof(font[0]) - 1].rows; /* 空格兜底 */
}

/* ---------------- 2D 画图原语 ---------------- */

static void put_pixel(int x, int y, uint16_t col)
{
	if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H)
		canvas[y * CANVAS_W + x] = col;
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t col)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;

	for (;;) {
		put_pixel(x0, y0, col);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

/* scale 倍放大的 5x7 字符 */
static void draw_text(int x, int y, int scale, uint16_t col, const char *s)
{
	for (; *s; s++, x += 6 * scale) {
		const uint8_t *rows = glyph_rows(*s);
		int r, c, i, j;

		for (r = 0; r < 7; r++)
			for (c = 0; c < 5; c++) {
				if (!(rows[r] & (0x10 >> c)))
					continue;
				for (i = 0; i < scale; i++)
					for (j = 0; j < scale; j++)
						put_pixel(x + c * scale + j,
							  y + r * scale + i, col);
			}
	}
}

/* ---------------- 立方体渲染 ---------------- */

struct vec3 { double x, y, z; };

/* ZYX 欧拉角旋转：v' = Rz(yaw)·Ry(pitch)·Rx(roll)·v */
static struct vec3 rotate(struct vec3 v, double roll, double pitch, double yaw)
{
	double cr = cos(roll), sr = sin(roll);
	double cp = cos(pitch), sp = sin(pitch);
	double cy = cos(yaw), sy = sin(yaw);
	struct vec3 a, b, r;

	a.x = v.x;			/* Rx */
	a.y = cr * v.y - sr * v.z;
	a.z = sr * v.y + cr * v.z;
	b.x = cp * a.x + sp * a.z;	/* Ry */
	b.y = a.y;
	b.z = -sp * a.x + cp * a.z;
	r.x = cy * b.x - sy * b.y;	/* Rz */
	r.y = sy * b.x + cy * b.y;
	r.z = b.z;
	return r;
}

/* 弱透视投影到屏幕坐标（cx,cy 中心，size 半边长像素） */
static void project(struct vec3 v, int cx, int cy, double size,
		    int *px, int *py)
{
	double d = 4.0;				/* 相机距离（立方体半边=1） */
	double k = size * d / (d - v.z);	/* z 朝屏外为正 */

	*px = cx + (int)(k * v.x);
	*py = cy - (int)(k * v.y);
}

static void draw_cube(double roll, double pitch, double yaw, int cx, int cy)
{
	static const struct vec3 verts[8] = {
		{ -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
		{ -1, -1,  1 }, { 1, -1,  1 }, { 1, 1,  1 }, { -1, 1,  1 },
	};
	static const int edges[12][2] = {
		{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },	/* 底面 z=-1 */
		{ 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },	/* 顶面 z=+1 */
		{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },	/* 竖棱 */
	};
	int px[8], py[8], i;

	for (i = 0; i < 8; i++) {
		struct vec3 r = rotate(verts[i], roll, pitch, yaw);

		project(r, cx, cy, 42.0, &px[i], &py[i]);
	}
	for (i = 0; i < 12; i++)
		draw_line(px[edges[i][0]], py[edges[i][0]],
			  px[edges[i][1]], py[edges[i][1]], COL_EDGE);

	/* 机体坐标轴：X 红（机头）、Y 绿（左翼）、Z 蓝（天顶） */
	static const struct vec3 axes[3] = {
		{ 1.8, 0, 0 }, { 0, 1.8, 0 }, { 0, 0, 1.8 },
	};
	static const uint16_t axcol[3] = { COL_XAXIS, COL_YAXIS, COL_ZAXIS };

	for (i = 0; i < 3; i++) {
		struct vec3 r = rotate(axes[i], roll, pitch, yaw);
		int ax, ay;

		project(r, cx, cy, 42.0, &ax, &ay);
		draw_line(cx, cy, ax, ay, axcol[i]);
	}
}

/* ---------------- 整帧合成 ---------------- */

static void render_frame(double roll, double pitch, double yaw)
{
	char buf[32];
	int i;
	static const char *labels[3] = { "ROLL :", "PITCH:", "YAW  :" };
	double vals[3] = { roll, pitch, yaw };

	for (i = 0; i < CANVAS_W * CANVAS_H; i++)
		canvas[i] = COL_BG;

	draw_text(8, 8, 2, COL_LABEL, "ATTITUDE DEG");
	for (i = 0; i < 3; i++) {
		double deg = vals[i] * 180.0 / M_PI;

		draw_text(8, 60 + 44 * i, 2, COL_LABEL, labels[i]);
		snprintf(buf, sizeof(buf), "%7.1f", deg);
		draw_text(8 + 6 * 2 * 7, 60 + 44 * i, 2, COL_TEXT, buf);
	}
	draw_cube(roll, pitch, yaw, 235, 130);
}

/* ---------------- PPM 输出（主机自测） ---------------- */

static int write_ppm(const char *path)
{
	FILE *f = fopen(path, "w");
	int i;

	if (!f)
		return -1;
	fprintf(f, "P6 %d %d 255\n", CANVAS_W, CANVAS_H);
	for (i = 0; i < CANVAS_W * CANVAS_H; i++) {
		uint16_t p = canvas[i];
		uint8_t rgb[3] = {
			(uint8_t)(((p >> 11) & 0x1F) << 3),
			(uint8_t)(((p >> 5) & 0x3F) << 2),
			(uint8_t)((p & 0x1F) << 3),
		};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	return 0;
}

/* ---------------- IIO sysfs 读取 ---------------- */

static char iio_dir[128];

static int read_sysfs_double(const char *file, double *out)
{
	char path[256];
	FILE *f;
	int ret;

	snprintf(path, sizeof(path), "%s/%s", iio_dir, file);
	f = fopen(path, "r");
	if (!f)
		return -1;
	ret = fscanf(f, "%lf", out);
	fclose(f);
	return ret == 1 ? 0 : -1;
}

static int iio_find_mpu(void)
{
	int i;

	for (i = 0; i < 16; i++) {
		char path[160], name[64] = "";
		FILE *f;

		snprintf(path, sizeof(path),
			 "/sys/bus/iio/devices/iio:device%d/name", i);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fscanf(f, "%63s", name) != 1)
			name[0] = '\0';
		fclose(f);
		if (strstr(name, "mpu6050")) {	/* 主线 mpu6050 / 教学 mympu6050 */
			snprintf(iio_dir, sizeof(iio_dir),
				 "/sys/bus/iio/devices/iio:device%d", i);
			return 0;
		}
	}
	return -1;
}

/* ---------------- framebuffer 输出 ---------------- */

struct fb {
	int fd;
	uint8_t *mem;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
};

static int fb_open(struct fb *fb)
{
	fb->fd = open("/dev/fb0", O_RDWR);
	if (fb->fd < 0) {
		perror("open /dev/fb0");
		return -1;
	}
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) ||
	    ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix)) {
		perror("fb ioctl");
		return -1;
	}
	fb->mem = mmap(NULL, fb->fix.smem_len, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fb->fd, 0);
	if (fb->mem == MAP_FAILED) {
		perror("mmap fb");
		return -1;
	}
	fprintf(stderr, "fb0: %ux%u %ubpp\n",
		fb->var.xres, fb->var.yres, fb->var.bits_per_pixel);
	return 0;
}

static void fb_blit(struct fb *fb)
{
	unsigned int w = fb->var.xres < CANVAS_W ? fb->var.xres : CANVAS_W;
	unsigned int h = fb->var.yres < CANVAS_H ? fb->var.yres : CANVAS_H;
	unsigned int x, y;

	for (y = 0; y < h; y++) {
		uint8_t *dst = fb->mem + y * fb->fix.line_length;
		const uint16_t *src = &canvas[y * CANVAS_W];

		if (fb->var.bits_per_pixel == 16) {
			memcpy(dst, src, w * 2);
		} else if (fb->var.bits_per_pixel == 32) {
			uint32_t *d32 = (uint32_t *)dst;

			for (x = 0; x < w; x++) {
				uint16_t p = src[x];

				d32[x] = ((uint32_t)(((p >> 11) & 0x1F) << 3) << 16) |
					 ((uint32_t)(((p >> 5) & 0x3F) << 2) << 8) |
					 (uint32_t)((p & 0x1F) << 3);
			}
		}
	}
}

/* ---------------- 互补滤波主循环 ---------------- */

#define LOOP_HZ		25.0
#define FILTER_ALPHA	0.98	/* 陀螺权重 */

static int run_live(void)
{
	struct fb fb;
	double accel_scale, gyro_scale;
	double roll = 0, pitch = 0, yaw = 0;
	struct timespec period = { 0, (long)(1e9 / LOOP_HZ) };

	if (iio_find_mpu()) {
		fprintf(stderr, "no mpu6050 IIO device found\n");
		return 1;
	}
	fprintf(stderr, "using %s\n", iio_dir);

	if (read_sysfs_double("in_accel_scale", &accel_scale) ||
	    read_sysfs_double("in_anglvel_scale", &gyro_scale)) {
		fprintf(stderr, "cannot read scales\n");
		return 1;
	}
	if (fb_open(&fb))
		return 1;

	for (;;) {
		double ax, ay, az, gx, gy, gz;
		double dt = 1.0 / LOOP_HZ;

		if (read_sysfs_double("in_accel_x_raw", &ax) ||
		    read_sysfs_double("in_accel_y_raw", &ay) ||
		    read_sysfs_double("in_accel_z_raw", &az) ||
		    read_sysfs_double("in_anglvel_x_raw", &gx) ||
		    read_sysfs_double("in_anglvel_y_raw", &gy) ||
		    read_sysfs_double("in_anglvel_z_raw", &gz)) {
			fprintf(stderr, "sensor read failed\n");
			return 1;
		}
		ax *= accel_scale; ay *= accel_scale; az *= accel_scale;
		gx *= gyro_scale; gy *= gyro_scale; gz *= gyro_scale;

		/* 加速度反解的绝对姿态（静态参考，动态时噪声大） */
		double acc_roll = atan2(ay, az);
		double acc_pitch = atan2(-ax, sqrt(ay * ay + az * az));

		/* 互补滤波：短期信陀螺，长期信重力 */
		roll = FILTER_ALPHA * (roll + gx * dt) +
		       (1.0 - FILTER_ALPHA) * acc_roll;
		pitch = FILTER_ALPHA * (pitch + gy * dt) +
			(1.0 - FILTER_ALPHA) * acc_pitch;
		yaw += gz * dt;			/* 无磁力计，纯积分会漂移 */

		render_frame(roll, pitch, yaw);
		fb_blit(&fb);
		clock_nanosleep(CLOCK_MONOTONIC, 0, &period, NULL);
	}
}

int main(int argc, char **argv)
{
	if (argc == 6 && !strcmp(argv[1], "--test")) {
		double d2r = M_PI / 180.0;

		render_frame(atof(argv[3]) * d2r, atof(argv[4]) * d2r,
			     atof(argv[5]) * d2r);
		if (write_ppm(argv[2])) {
			perror("write ppm");
			return 1;
		}
		printf("wrote %s\n", argv[2]);
		return 0;
	}
	return run_live();
}
