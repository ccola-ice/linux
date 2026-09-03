// SPDX-License-Identifier: GPL-2.0
/*
 * myram - teaching RAM block device
 *
 * Stage D: sparse xarray-backed storage. Pages are allocated on first write,
 * holes read as zero, and discard releases fully covered pages.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/xarray.h>

#define MYRAM_NAME		"myram"
#define MYRAM_QUEUE_DEPTH	64
#define MYRAM_DEFAULT_SIZE_MB	64

struct myram_device {
	struct blk_mq_tag_set tag_set;
	struct gendisk *disk;
	struct mutex lock;
	struct xarray pages;
	size_t size;
	u64 nr_pages;
	unsigned int blocksize;
	int major;
	sector_t capacity;
	bool tag_set_allocated;
	bool disk_added;
};

static struct myram_device *myram_dev;

static unsigned int myram_size_mb = MYRAM_DEFAULT_SIZE_MB;
module_param(myram_size_mb, uint, 0444);
MODULE_PARM_DESC(myram_size_mb, "Device capacity in MiB");

static unsigned int myram_blocksize = 512;
module_param(myram_blocksize, uint, 0444);
MODULE_PARM_DESC(myram_blocksize, "Logical block size in bytes (512 or 4096)");

static bool myram_range_valid(struct myram_device *dev, u64 pos, size_t len)
{
	if (len == 0)
		return true;

	if (pos > dev->size || len > dev->size - (size_t)pos)
		return false;

	if (!IS_ALIGNED(pos, dev->blocksize) || !IS_ALIGNED(len, dev->blocksize))
		return false;

	return true;
}

static struct page *myram_get_page(struct myram_device *dev, u64 pos,
				   bool create, blk_opf_t opf)
{
	unsigned long index = pos >> PAGE_SHIFT;
	gfp_t gfp = (opf & REQ_NOWAIT) ? GFP_NOWAIT : GFP_NOIO;
	struct page *page;
	void *old;

	page = xa_load(&dev->pages, index);
	if (page || !create)
		return page;

	page = alloc_page(gfp | __GFP_ZERO | __GFP_HIGHMEM);
	if (!page)
		return ERR_PTR(-ENOMEM);

	old = xa_cmpxchg(&dev->pages, index, NULL, page, gfp);
	if (xa_is_err(old)) {
		put_page(page);
		return ERR_PTR(xa_err(old));
	}

	if (old) {
		put_page(page);
		return old;
	}

	dev->nr_pages++;
	return page;
}

static blk_status_t myram_transfer(struct myram_device *dev, struct request *rq)
{
	u64 pos = (u64)blk_rq_pos(rq) << SECTOR_SHIFT;
	unsigned int max_bytes = blk_rq_bytes(rq);
	unsigned int transferred = 0;
	bool is_write = req_op(rq) == REQ_OP_WRITE;
	struct req_iterator iter;
	struct bio_vec bvec;
	blk_status_t status = BLK_STS_OK;

	if (!myram_range_valid(dev, pos, max_bytes))
		return BLK_STS_IOERR;

	mutex_lock(&dev->lock);

	rq_for_each_segment(bvec, rq, iter) {
		unsigned int segment_done = 0;
		unsigned int segment_len = min(bvec.bv_len,
					       max_bytes - transferred);
		void *kaddr = kmap_local_page(bvec.bv_page);

		if (is_write)
			flush_dcache_page(bvec.bv_page);

		while (segment_done < segment_len) {
			unsigned int page_offset = offset_in_page(pos);
			unsigned int len = min(segment_len - segment_done,
					       PAGE_SIZE - page_offset);
			struct page *page;
			void *buf = kaddr + bvec.bv_offset + segment_done;

			page = myram_get_page(dev, pos, is_write, rq->cmd_flags);
			if (IS_ERR(page)) {
				status = (rq->cmd_flags & REQ_NOWAIT) ?
					 BLK_STS_AGAIN : BLK_STS_RESOURCE;
				goto out_unmap;
			}

			if (is_write)
				memcpy_to_page(page, page_offset, buf, len);
			else if (page)
				memcpy_from_page(buf, page, page_offset, len);
			else
				memset(buf, 0, len);

			pos += len;
			segment_done += len;
			transferred += len;
		}

		if (!is_write)
			flush_dcache_page(bvec.bv_page);
		kunmap_local(kaddr);

		if (transferred >= max_bytes)
			break;
		continue;

out_unmap:
		kunmap_local(kaddr);
		break;
	}

	mutex_unlock(&dev->lock);
	return status;
}

static blk_status_t myram_zero_range(struct myram_device *dev, struct request *rq)
{
	u64 pos = (u64)blk_rq_pos(rq) << SECTOR_SHIFT;
	size_t len = blk_rq_bytes(rq);

	if (!myram_range_valid(dev, pos, len))
		return BLK_STS_IOERR;

	mutex_lock(&dev->lock);

	while (len) {
		unsigned long index = pos >> PAGE_SHIFT;
		unsigned int page_offset = offset_in_page(pos);
		unsigned int chunk = min_t(size_t, len,
					   PAGE_SIZE - page_offset);
		struct page *page = xa_load(&dev->pages, index);

		if (page && page_offset == 0 && chunk == PAGE_SIZE) {
			page = xa_erase(&dev->pages, index);
			if (page) {
				put_page(page);
				dev->nr_pages--;
			}
		} else if (page) {
			zero_user_segment(page, page_offset, page_offset + chunk);
		}

		pos += chunk;
		len -= chunk;
	}

	mutex_unlock(&dev->lock);

	return BLK_STS_OK;
}

static blk_status_t myram_queue_rq(struct blk_mq_hw_ctx *hctx,
				   const struct blk_mq_queue_data *bd)
{
	struct myram_device *dev = hctx->queue->queuedata;
	struct request *rq = bd->rq;
	blk_status_t status;

	if (blk_rq_is_passthrough(rq)) {
		blk_mq_start_request(rq);
		blk_mq_end_request(rq, BLK_STS_NOTSUPP);
		return BLK_STS_OK;
	}

	blk_mq_start_request(rq);

	switch (req_op(rq)) {
	case REQ_OP_READ:
	case REQ_OP_WRITE:
		status = myram_transfer(dev, rq);
		break;
	case REQ_OP_FLUSH:
		status = BLK_STS_OK;
		break;
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		status = myram_zero_range(dev, rq);
		break;
	default:
		status = BLK_STS_NOTSUPP;
		break;
	}

	blk_mq_end_request(rq, status);
	return BLK_STS_OK;
}

static const struct blk_mq_ops myram_mq_ops = {
	.queue_rq = myram_queue_rq,
};

static const struct block_device_operations myram_fops = {
	.owner = THIS_MODULE,
};

static void myram_free_pages(struct myram_device *dev)
{
	struct page *page;
	unsigned long index;

	xa_for_each(&dev->pages, index, page) {
		put_page(page);
		cond_resched();
	}
	xa_destroy(&dev->pages);
	dev->nr_pages = 0;
}

static void myram_cleanup(void)
{
	if (!myram_dev)
		return;

	if (myram_dev->disk) {
		if (myram_dev->disk_added)
			del_gendisk(myram_dev->disk);
		put_disk(myram_dev->disk);
		myram_dev->disk = NULL;
		myram_dev->disk_added = false;
	}

	if (myram_dev->tag_set_allocated) {
		blk_mq_free_tag_set(&myram_dev->tag_set);
		myram_dev->tag_set_allocated = false;
	}

	if (myram_dev->major > 0) {
		unregister_blkdev(myram_dev->major, MYRAM_NAME);
		myram_dev->major = 0;
	}

	myram_free_pages(myram_dev);
	kfree(myram_dev);
	myram_dev = NULL;
}

static int __init myram_init(void)
{
	struct queue_limits lim;
	struct gendisk *disk;
	size_t size;
	int ret;

	if (myram_size_mb == 0)
		return -EINVAL;

	if (myram_blocksize != 512 && myram_blocksize != 4096)
		return -EINVAL;

	if (myram_size_mb > (SIZE_MAX >> 20))
		return -EOVERFLOW;

	size = (size_t)myram_size_mb << 20;
	if (size < myram_blocksize || !IS_ALIGNED(size, myram_blocksize))
		return -EINVAL;

	myram_dev = kzalloc(sizeof(*myram_dev), GFP_KERNEL);
	if (!myram_dev)
		return -ENOMEM;

	xa_init(&myram_dev->pages);
	myram_dev->size = size;
	myram_dev->blocksize = myram_blocksize;
	mutex_init(&myram_dev->lock);

	myram_dev->major = register_blkdev(0, MYRAM_NAME);
	if (myram_dev->major < 0) {
		ret = myram_dev->major;
		goto fail;
	}

	ret = blk_mq_alloc_sq_tag_set(&myram_dev->tag_set, &myram_mq_ops,
				      MYRAM_QUEUE_DEPTH, BLK_MQ_F_BLOCKING);
	if (ret)
		goto fail;
	myram_dev->tag_set_allocated = true;

	lim = (struct queue_limits) {
		.logical_block_size	= myram_blocksize,
		.physical_block_size	= myram_blocksize,
		.max_hw_sectors		= 1024,
		.max_hw_discard_sectors	= UINT_MAX,
		.max_discard_segments	= 1,
		.discard_granularity	= myram_blocksize,
		.features		= BLK_FEAT_SYNCHRONOUS,
	};

	disk = blk_mq_alloc_disk(&myram_dev->tag_set, &lim, myram_dev);
	if (IS_ERR(disk)) {
		ret = PTR_ERR(disk);
		goto fail;
	}
	myram_dev->disk = disk;

	disk->major = myram_dev->major;
	disk->first_minor = 0;
	disk->minors = 1;
	disk->fops = &myram_fops;
	disk->private_data = myram_dev;
	snprintf(disk->disk_name, DISK_NAME_LEN, "%s0", MYRAM_NAME);

	myram_dev->capacity = size >> SECTOR_SHIFT;
	set_capacity(disk, myram_dev->capacity);

	ret = add_disk(disk);
	if (ret)
		goto fail;
	myram_dev->disk_added = true;

	pr_info("myram: loaded major=%d size=%u MiB blocksize=%u (sparse)\n",
		myram_dev->major, myram_size_mb, myram_blocksize);
	return 0;

fail:
	myram_cleanup();
	return ret;
}

static void __exit myram_exit(void)
{
	u64 nr_pages = myram_dev ? myram_dev->nr_pages : 0;

	myram_cleanup();
	pr_info("myram: unloaded, freed %llu backing pages\n", nr_pages);
}

module_init(myram_init);
module_exit(myram_exit);

MODULE_AUTHOR("Teaching example");
MODULE_DESCRIPTION("Teaching RAM block device - stage D sparse xarray backend");
MODULE_LICENSE("GPL");
