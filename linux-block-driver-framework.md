# 教学用 RAM 块设备驱动路线

本文面向当前源码树 Linux 7.2，目标是先建立块层心智模型，再逐步实现一个
基于 RAM 的 `blk-mq` 块设备。第一版只使用单硬件队列、同步完成和连续内存，
暂不加入 discard、异步中断、多队列或稀疏页。

参考源码：

- `Documentation/block/blk-mq.rst`：blk-mq 的软件队列和硬件派发队列
- `include/linux/blk-mq.h`：`blk_mq_ops`、tag set 和请求完成 API
- `drivers/block/swim3.c`：精简的单队列 tag set / gendisk 初始化
- `drivers/block/null_blk/main.c`：request segment 遍历和多种完成方式
- `drivers/block/brd.c`：按需分配页面的 RAM 后端

## 1. 一次块 I/O 如何到达驱动

典型路径如下：

```text
read/write/mmap
      |
      v
VFS / 文件系统
      |
      v
bio（描述一段块 I/O）
      |
      v
request（块层合并一个或多个 bio）
      |
      v
blk-mq 软件暂存队列（struct blk_mq_ctx）
      |
      v
硬件派发队列（struct blk_mq_hw_ctx）
      |
      v
驱动 .queue_rq()
      |
      v
RAM 后端复制数据
      |
      v
blk_mq_end_request()
```

### 1.1 `bio`

`bio` 描述面向块设备的一次 I/O：

- 起始扇区
- 操作类型，例如 READ、WRITE、FLUSH、DISCARD
- 一组内存页片段 `bio_vec`

扇区位置始终以 512 字节为单位表达，即使设备的逻辑块大小是 4096 字节。

### 1.2 `request`

blk-mq 驱动通常不直接接收 `bio`，而是接收 `struct request`。块层可以把相邻
的 bio 合并成一个 request，也可以通过 I/O 调度器重新排序。常用访问接口：

```c
req_op(rq)                 /* READ、WRITE 等操作 */
blk_rq_pos(rq)             /* 起始扇区，单位 512 字节 */
blk_rq_sectors(rq)         /* 扇区数 */
blk_rq_bytes(rq)           /* 字节数 */
rq_for_each_segment(...)   /* 遍历请求中的内存片段 */
```

### 1.3 软件队列与硬件队列

`struct blk_mq_ctx` 是通常按 CPU 分布的软件暂存队列，用于合并、调度和降低
多核竞争。`struct blk_mq_hw_ctx` 对应驱动可提交请求的硬件队列。真实 NVMe
设备可能有很多硬件队列；教学 RAM 盘先只配置一个。

软件队列不保证 request 按到达顺序完成。文件系统和上层协议负责需要的顺序
语义，驱动负责正确处理它已经接受的每个 request。

### 1.4 tag

blk-mq 给在途 request 分配 tag。真实硬件驱动通常把 tag 写入命令描述符，
中断到达后再用 tag 找回 request。RAM 盘同步处理并立即完成，暂时不需要主动
管理 tag，但仍需创建 tag set，因为 request 的分配和队列深度依赖它。

### 1.5 `gendisk`、`request_queue` 与驱动私有数据

- `struct gendisk`：对外可见的磁盘，决定名称、容量、主次设备号和操作表。
- `struct request_queue`：块层队列；`blk_mq_alloc_disk()` 会为磁盘创建它。
- 驱动私有结构：保存 `gendisk`、`blk_mq_tag_set`、RAM 地址和容量。

建议第一版只创建一个全局设备：

```c
struct myram_device {
	struct blk_mq_tag_set tag_set;
	struct gendisk *disk;
	u8 *data;
	size_t size;
	int major;
};
```

这里的代码只是结构草图，不是可直接编译的完整驱动。

## 2. 为什么主线使用 blk-mq

当前树中的 `drivers/block/brd.c` 使用：

```text
block_device_operations.submit_bio -> brd_submit_bio() -> bio_endio()
```

这条路径短，适合学习 `bio` 和稀疏页面后端，但它绕过了 request 驱动模型。
要学习现代硬件驱动的提交、资源限制和完成机制，主线应使用：

```text
blk_mq_ops.queue_rq -> blk_mq_start_request() -> blk_mq_end_request()
```

因此：

- 从 `swim3.c` 学最小 blk-mq 初始化；
- 从 `null_blk/main.c` 学 request 数据传输；
- 从 `brd.c` 学后续的 xarray 稀疏存储；
- 不把 `submit_bio` 和 `queue_rq` 同时放进第一版驱动。

## 3. 设备初始化与清理

### 3.1 初始化顺序

推荐顺序：

```text
分配并清零驱动私有结构
  -> 分配 RAM 后端
  -> register_blkdev(0, "myram")
  -> blk_mq_alloc_sq_tag_set()
  -> blk_mq_alloc_disk()
  -> 填充 gendisk
  -> set_capacity()
  -> add_disk()
```

#### 1. 分配后端

第一版使用 `vzalloc(size)`：

- 地址连续，复制逻辑简单；
- 内存自动清零，因此从未写区域读取时返回零；
- 支持比 `kmalloc` 更大的教学容量；
- 它并不代表真实硬件内存布局，只是降低第一阶段复杂度。

容量可先固定为 64 MiB。不要使用过大默认值，否则模块加载时会直接占用大量
内存。

#### 2. 注册主设备号

```c
major = register_blkdev(0, "myram");
```

参数 `0` 请求动态分配主设备号。返回值大于零才是成功的主设备号；负值是错误。

#### 3. 创建单队列 tag set

教学版可以使用：

```c
blk_mq_alloc_sq_tag_set(&dev->tag_set, &myram_mq_ops,
			queue_depth, 0);
```

它简化了以下字段的设置：

- `ops`
- `nr_hw_queues = 1`
- `queue_depth`
- `numa_node`
- `flags`

队列深度可先设为 64。`myram_mq_ops` 至少提供 `.queue_rq`。

#### 4. 创建磁盘和队列

先准备 `struct queue_limits`，第一版至少明确：

```c
struct queue_limits lim = {
	.logical_block_size = 512,
	.physical_block_size = 512,
	.max_hw_sectors = 1024,
};
```

然后调用：

```c
disk = blk_mq_alloc_disk(&dev->tag_set, &lim, dev);
```

第三个参数成为 `disk->queue->queuedata`，在 `queue_rq` 中可通过
`hctx->queue->queuedata` 取回设备。

#### 5. 填充并发布 `gendisk`

需设置：

- `disk->major`
- `disk->first_minor`
- `disk->minors`
- `disk->fops`
- `disk->private_data`
- `disk->disk_name`
- `set_capacity(disk, size >> SECTOR_SHIFT)`

最后调用 `add_disk(disk)`。只有 `add_disk()` 成功后，设备才对用户空间可见。
发布前所有请求路径会访问的状态都必须已经初始化完成。

### 3.2 `block_device_operations`

即使数据请求通过 `blk_mq_ops`，`gendisk` 仍需要
`struct block_device_operations`：

```c
static const struct block_device_operations myram_fops = {
	.owner = THIS_MODULE,
};
```

`.owner` 防止设备仍在使用时模块代码被卸载。第一版不需要自定义 `open`、
`release` 或 ioctl。

### 3.3 正常卸载顺序

推荐顺序：

```text
del_gendisk()
  -> put_disk()
  -> blk_mq_free_tag_set()
  -> unregister_blkdev()
  -> vfree(RAM 后端)
  -> 释放私有结构
```

`del_gendisk()` 先阻止新用户并拆除磁盘；在它完成前不能释放 request 会访问的
后端。`put_disk()` 后不要再解引用 `gendisk`。

### 3.4 初始化失败时回滚

每一步失败，只释放此前已经成功取得的资源，逆序回滚：

```text
add_disk 失败:
  put_disk
  blk_mq_free_tag_set
  unregister_blkdev
  vfree

blk_mq_alloc_disk 失败:
  blk_mq_free_tag_set
  unregister_blkdev
  vfree

tag set 分配失败:
  unregister_blkdev
  vfree
```

`blk_mq_alloc_disk()` 返回错误指针，应使用 `IS_ERR()` 和 `PTR_ERR()`，不能只判
空指针。

## 4. 分阶段实现数据路径

每阶段都应保持模块可编译、可加载，并在进入下一阶段前完成对应检查点。

### 阶段 A：最小 request 驱动

目标是让磁盘出现，并观察 request 到达 `.queue_rq`。此阶段可以对所有请求返回
不支持或 I/O 错误，但必须遵守 request 状态机。

最小同步路径：

```c
static blk_status_t myram_queue_rq(struct blk_mq_hw_ctx *hctx,
				   const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;

	blk_mq_start_request(rq);
	blk_mq_end_request(rq, BLK_STS_NOTSUPP);
	return BLK_STS_OK;
}
```

这里有两种不同的“返回”：

- `blk_mq_end_request()` 的状态是该 I/O 的最终结果；
- `.queue_rq()` 返回 `BLK_STS_OK` 表示驱动已经接管并完成了 request。

不要在 `blk_mq_end_request()` 后再次读取 `rq`。完成调用可能让 request 立即被
释放或复用。

检查点：

- 模块加载成功；
- `/sys/block/myram0` 出现；
- `lsblk` 能显示容量；
- 读请求得到预期错误，内核没有 WARN、Oops 或悬挂 request。

### 阶段 B：固定容量 RAM 后端

目标是支持 READ 和 WRITE，其他操作暂时返回明确状态。

#### 1. 确定操作和范围

进入 segment 循环前：

```text
op = req_op(rq)
start = blk_rq_pos(rq) << SECTOR_SHIFT
length = blk_rq_bytes(rq)
检查 start <= size 且 length <= size - start
```

用 `length <= size - start`，不要直接用 `start + length <= size`，后者在通用
代码中可能整数溢出。

操作策略：

- `REQ_OP_READ`：RAM 复制到 request 页；
- `REQ_OP_WRITE`：request 页复制到 RAM；
- `REQ_OP_FLUSH`：连续 RAM 没有易失写缓存，可直接成功；
- 其他操作：返回 `BLK_STS_NOTSUPP`。

#### 2. 遍历 request 的数据页

`rq_for_each_segment()` 返回的每个 `bio_vec` 包含：

- `bv_page`：页；
- `bv_offset`：页内偏移；
- `bv_len`：片段长度。

处理框架：

```c
pos = blk_rq_pos(rq) << SECTOR_SHIFT;
rq_for_each_segment(bvec, rq, iter) {
	void *buffer = kmap_local_page(bvec.bv_page) + bvec.bv_offset;

	if (req_op(rq) == REQ_OP_WRITE)
		memcpy(dev->data + pos, buffer, bvec.bv_len);
	else
		memcpy(buffer, dev->data + pos, bvec.bv_len);

	kunmap_local(buffer);
	pos += bvec.bv_len;
}
```

实际编写时应保存 `kmap_local_page()` 返回的页基址，再将同一基址传给
`kunmap_local()`，这样更清楚，也避免依赖带偏移地址是否被接受：

```c
void *kaddr = kmap_local_page(bvec.bv_page);
void *buffer = kaddr + bvec.bv_offset;
/* copy */
kunmap_local(kaddr);
```

写入 request 页前后是否需要 `flush_dcache_page()` 与目标架构有关。为了让练习
在非缓存一致架构上也正确，可参考 `null_transfer()`：读设备时在写 request 页
后刷新，写设备时在读取 request 页前刷新。

#### 3. 同步完成

第一版复制完成后直接：

```c
blk_mq_end_request(rq, status);
return BLK_STS_OK;
```

RAM 后端无需模拟中断。等同步版本稳定后，再学习
`blk_mq_complete_request()` 和 `.complete` 回调。

#### 4. 是否需要锁

对普通对齐的块 I/O，块层可能同时派发多个不重叠 request；但两个 request 也
可能访问同一区域。单纯 `memcpy` 不能提供复杂的重叠写原子性。

教学第一版可以：

- 只配置一个硬件队列；
- 用一个自旋锁串行化复制；
- 保证持锁期间不分配内存、不睡眠。

但大段 `memcpy` 持有自旋锁会增加延迟。更合理的后续设计是按页或按范围加锁，
或明确设备只保证逻辑块粒度的并发语义。不要用一个 mutex，除非 tag set 标记
`BLK_MQ_F_BLOCKING`，因为普通 `.queue_rq` 上下文不得睡眠。

检查点：

- 全新设备读取为全零；
- 写入一个扇区后能读回相同内容；
- 写入设备末尾最后一个合法逻辑块成功；
- 越界请求得到 `BLK_STS_IOERR`；
- 不支持的操作不会让请求永久在途。

### 阶段 C：边界、操作语义和健壮性

#### 1. 扇区与字节

必须始终区分：

```text
sector_t 扇区位置       以 512 字节为单位
logical_block_size      用户可提交的最小对齐单位
RAM 偏移                字节
set_capacity() 参数     512 字节扇区数
```

64 MiB 容量对应：

```text
64 * 1024 * 1024 / 512 = 131072 sectors
```

#### 2. 状态码

常用结果：

- `BLK_STS_OK`：完成成功；
- `BLK_STS_IOERR`：地址越界或数据传输失败；
- `BLK_STS_NOTSUPP`：操作不支持；
- `BLK_STS_RESOURCE` / `BLK_STS_DEV_RESOURCE`：临时无资源，请块层重试。

资源状态不是普通 I/O 错误。只有驱动稍后确实能接受并会重新触发队列时，才应
返回它们。同步 RAM 盘通常不需要资源重试。

#### 3. request 所有权

建议第一版遵守单一规则：

1. 在拒绝接管前可以直接从 `.queue_rq()` 返回资源状态；
2. 一旦调用 `blk_mq_start_request()`，驱动负责完成它；
3. 调用 `blk_mq_end_request()` 后不再访问 request 或其 PDU；
4. 已自行完成时，`.queue_rq()` 返回 `BLK_STS_OK`。

### 阶段 D：稀疏页面后端

连续 `vzalloc` 版本稳定后，可改为和 `brd` 相似的 xarray：

```text
xarray key = sector >> PAGE_SECTORS_SHIFT
xarray value = struct page *
```

读取：

- 查找页面；
- 页面存在则复制；
- 页面不存在则向 request 缓冲区填零。

写入：

- 用 `GFP_NOIO` 分配清零页面；
- 在 xarray 锁下插入；
- 如果另一个写者已插入同一索引，使用已有页面并释放新页面；
- 复制数据。

使用 `GFP_NOIO` 是为了避免内存回收递归进入块 I/O，形成回收死锁。若分配路径
可能睡眠，`.queue_rq` 所属 tag set 需要 `BLK_MQ_F_BLOCKING`，或者把工作转移
到允许睡眠的 worker。

`brd_lookup_page()` 展示了 RCU 查找和页面引用保护，`brd_insert_page()` 展示
了 `__xa_cmpxchg()` 处理并发插入。不要直接删减引用计数步骤，否则 discard 或
卸载可能与在途复制形成 use-after-free。

## 5. 构建和验证路线

本文阶段不创建驱动源码。开始编码时，建议把树外练习放在独立目录，使用当前
内核树的构建系统：

```makefile
obj-m += myram.o
```

```sh
make -C /home/zhangb/linux M="$PWD" modules
```

模块只能安全加载到 ABI 和配置匹配的内核。仅仅在此源码树编译成功，不代表能
加载到正在运行的其他版本内核。

### 5.1 环境确认

```sh
uname -r
ls -l /lib/modules/"$(uname -r)"/build
```

如果当前运行内核不是这棵 7.2 源码构建出的内核，可在虚拟机中启动该内核，
或者使用运行内核对应的 headers 构建树外模块。

### 5.2 设备出现

```sh
sudo insmod myram.ko
dmesg
lsblk
ls -l /dev/myram0
cat /sys/block/myram0/size
```

若 udev 没有创建设备节点，可根据 `/proc/devices` 中的动态 major 临时使用
`mknod`。不要硬编码一个可能与其他驱动冲突的 major。

### 5.3 原始数据测试

先测试一个逻辑块，避免文件系统掩盖基础错误：

```sh
sudo dd if=/dev/myram0 bs=4096 count=1 status=none | od -An -tx1
printf 'myram-test\n' | sudo dd of=/dev/myram0 bs=512 conv=sync status=none
sudo dd if=/dev/myram0 bs=512 count=1 status=none | od -An -tc
```

再做随机数据往返校验：

```sh
dd if=/dev/urandom of=/tmp/myram.input bs=4096 count=256 status=none
sudo dd if=/tmp/myram.input of=/dev/myram0 bs=4096 conv=fsync status=none
sudo dd if=/dev/myram0 of=/tmp/myram.output bs=4096 count=256 status=none
cmp /tmp/myram.input /tmp/myram.output
```

### 5.4 文件系统测试

原始读写稳定后：

```sh
sudo mkfs.ext4 -F /dev/myram0
sudo mkdir -p /mnt/myram
sudo mount /dev/myram0 /mnt/myram
sudo cp -a /usr/include /mnt/myram/
sync
sudo diff -qr /usr/include /mnt/myram/include
sudo umount /mnt/myram
```

卸载模块前必须先卸载文件系统，并确认没有进程打开设备：

```sh
sudo rmmod myram
dmesg
```

### 5.5 压力和错误检查

基础版本可用后再运行：

```sh
sudo fio --name=myram-rw --filename=/dev/myram0 \
	--rw=randrw --bs=4k --iodepth=32 --direct=1 \
	--size=32m --runtime=30 --time_based
```

同时观察：

```sh
dmesg -w
cat /sys/block/myram0/inflight
```

若能控制测试内核，建议启用 KASAN、KCSAN、lockdep 和内存泄漏检测。它们比
只看读写结果更容易发现越界、竞态和错误释放。

## 6. 常见故障定位

### 设备没有出现

依次检查：

1. `register_blkdev()` 是否返回负值；
2. tag set 和 disk 是否分配成功；
3. `add_disk()` 是否返回错误；
4. `disk_name`、major、minor、capacity 是否在发布前设置；
5. udev 是否创建设备节点。

### 第一次读取就卡住

最常见原因是：

- 忘记 `blk_mq_start_request()`；
- 某个分支没有调用 `blk_mq_end_request()`；
- `.queue_rq()` 返回资源状态后没有条件重新运行队列；
- 完成了 request，却返回了与所有权不一致的状态。

### 数据错位

重点检查：

- 是否把 `blk_rq_pos()` 当成字节而漏掉 `<< SECTOR_SHIFT`；
- 是否每处理一个 bvec 都递增 RAM 字节偏移；
- 是否计入 `bv_offset`；
- 容量是否按 512 字节扇区传给 `set_capacity()`；
- 最后一个 segment 是否超过 request 或设备边界。

### 卸载时崩溃

重点检查：

- 是否先 `del_gendisk()` 再释放 RAM；
- 是否在 `put_disk()` 后继续访问 disk；
- 是否在磁盘仍挂载时强行卸载；
- 是否在所有引用消失前销毁 tag set 或后端页面。

### 出现 “sleeping function called from invalid context”

说明普通 `.queue_rq` 路径里调用了可能睡眠的函数，例如 mutex、`GFP_KERNEL`
内存分配或某些 vmap 操作。解决方向：

- 把第一版路径限制为预分配内存和自旋锁；
- 或给 tag set 使用 `BLK_MQ_F_BLOCKING`；
- 或把请求转交 workqueue，并异步完成。

## 7. 后续练习顺序

建议一次只增加一个能力：

1. `REQ_OP_DISCARD`：连续内存版清零，稀疏版释放完整页面；
2. 异步完成：worker 或 hrtimer 完成 request；
3. `.timeout`：模拟丢失中断并处理完成竞争；
4. 多硬件队列：为每个 `hctx` 建立私有上下文；
5. I/O 统计和 debugfs；
6. 可配置容量和设备数量；
7. 稀疏存储和按页锁；
8. flush/FUA 与写缓存语义。

不要在第一个版本中模拟所有真实硬件行为。一个生命周期正确、边界安全、
每个 request 都恰好完成一次的单队列 RAM 盘，是继续学习 DMA、描述符环和
中断完成路径的可靠基础。
