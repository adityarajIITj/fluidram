// SPDX-License-Identifier: GPL-2.0-only
/*
 * FluidRAM: Linux Block Device Driver Implementation
 *
 * Hydrodynamic memory compression block device for Linux.
 *
 * Copyright (C) 2026 AdiOS Systems Group.
 */

#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/device.h>

#include "fluidram_drv.h"

static int num_devices = 1;
module_param(num_devices, int, 0444);
MODULE_PARM_DESC(num_devices, "Number of initial fluidram devices");

static unsigned long default_size_mb = 1024;
module_param(default_size_mb, ulong, 0644);
MODULE_PARM_DESC(default_size_mb, "Default virtual size in megabytes");

static int fluidram_major;
static struct fluidram_device *fluidram_devs[FLUIDRAM_MAX_DEVICES];
static DEFINE_MUTEX(fluidram_global_mutex);

/*
 * BIO Request Handler
 */
static void fluidram_submit_bio(struct bio *bio)
{
	struct fluidram_device *fdev = bio->bi_bdev->bd_disk->private_data;
	struct bvec_iter iter;
	struct bio_vec bv;
	sector_t sector = bio->bi_iter.bi_sector;
	int op = bio_op(bio);

	if (!fdev || !fdev->pool) {
		bio_io_error(bio);
		return;
	}

	bio_for_each_segment(bv, bio, iter) {
		size_t page_idx = (sector >> (PAGE_SHIFT - SECTOR_SHIFT));
		void *mem;

		if (op == REQ_OP_DISCARD) {
			fluid_pool_discard_page(fdev->pool, page_idx);
			sector += PAGE_SECTORS;
			continue;
		}

		mem = kmap_local_page(bv.bv_page);

		if (op == REQ_OP_WRITE) {
			int ret = fluid_pool_write_page(fdev->pool, page_idx, (const u8 *)mem);
			if (ret < 0) {
				kunmap_local(mem);
				bio_io_error(bio);
				return;
			}
		} else if (op == REQ_OP_READ) {
			int ret = fluid_pool_read_page(fdev->pool, page_idx, (u8 *)mem);
			if (ret < 0) {
				kunmap_local(mem);
				bio_io_error(bio);
				return;
			}
			flush_dcache_page(bv.bv_page);
		}

		kunmap_local(mem);
		sector += PAGE_SECTORS;
	}

	bio_endio(bio);
}

static const struct block_device_operations fluidram_devops = {
	.owner      = THIS_MODULE,
	.submit_bio = fluidram_submit_bio,
};

/*
 * Sysfs Attributes
 */
static ssize_t disksize_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct fluidram_device *fdev = disk->private_data;
	return sysfs_emit(buf, "%llu\n", fdev->disksize);
}

static ssize_t orig_data_size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct fluidram_device *fdev = disk->private_data;
	size_t phys_bytes = 0, orig_bytes = 0;

	if (fdev->pool)
		fluid_pool_get_stats(fdev->pool, &phys_bytes, &orig_bytes, NULL, NULL);

	return sysfs_emit(buf, "%zu\n", orig_bytes);
}

static ssize_t mem_used_total_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct fluidram_device *fdev = disk->private_data;
	size_t phys_bytes = 0, orig_bytes = 0;

	if (fdev->pool)
		fluid_pool_get_stats(fdev->pool, &phys_bytes, &orig_bytes, NULL, NULL);

	return sysfs_emit(buf, "%zu\n", phys_bytes);
}

static ssize_t compression_ratio_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct fluidram_device *fdev = disk->private_data;
	size_t phys_bytes = 0, orig_bytes = 0;
	u64 ratio_x100 = 100;

	if (fdev->pool) {
		fluid_pool_get_stats(fdev->pool, &phys_bytes, &orig_bytes, NULL, NULL);
		if (phys_bytes > 0)
			ratio_x100 = (orig_bytes * 100) / phys_bytes;
	}

	return sysfs_emit(buf, "%llu.%02llux\n", ratio_x100 / 100, ratio_x100 % 100);
}

static ssize_t borrow_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct fluidram_device *fdev = disk->private_data;
	u64 borrow_count = 0;

	if (fdev->pool)
		fluid_pool_get_stats(fdev->pool, NULL, NULL, &borrow_count, NULL);

	return sysfs_emit(buf, "%llu\n", borrow_count);
}

static ssize_t zero_page_faults_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	struct fluidram_device *fdev = disk->private_data;
	u64 faults_avoided = 0;

	if (fdev->pool)
		fluid_pool_get_stats(fdev->pool, NULL, NULL, NULL, &faults_avoided);

	return sysfs_emit(buf, "%llu\n", faults_avoided);
}

static DEVICE_ATTR_RO(disksize);
static DEVICE_ATTR_RO(orig_data_size);
static DEVICE_ATTR_RO(mem_used_total);
static DEVICE_ATTR_RO(compression_ratio);
static DEVICE_ATTR_RO(borrow_count);
static DEVICE_ATTR_RO(zero_page_faults);

static struct attribute *fluidram_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_orig_data_size.attr,
	&dev_attr_mem_used_total.attr,
	&dev_attr_compression_ratio.attr,
	&dev_attr_borrow_count.attr,
	&dev_attr_zero_page_faults.attr,
	NULL,
};

static const struct attribute_group fluidram_disk_attr_group = {
	.attrs = fluidram_disk_attrs,
};

static const struct attribute_group *fluidram_disk_attr_groups[] = {
	&fluidram_disk_attr_group,
	NULL,
};

/*
 * Device Initialization
 */
static int fluidram_create_device(int dev_id, u64 size_bytes)
{
	struct fluidram_device *fdev;
	struct gendisk *disk;
	size_t num_pages;
	int ret;

	fdev = kzalloc(sizeof(*fdev), GFP_KERNEL);
	if (!fdev)
		return -ENOMEM;

	fdev->dev_id = dev_id;
	fdev->disksize = size_bytes;
	num_pages = size_bytes >> PAGE_SHIFT;

	fdev->pool = fluid_pool_create(num_pages, size_bytes / 2);
	if (!fdev->pool) {
		kfree(fdev);
		return -ENOMEM;
	}

	disk = blk_alloc_disk(NUMA_NO_NODE);
	if (!disk) {
		fluid_pool_destroy(fdev->pool);
		kfree(fdev);
		return -ENOMEM;
	}

	disk->major = fluidram_major;
	disk->first_minor = dev_id;
	disk->minors = 1;
	disk->fops = &fluidram_devops;
	disk->private_data = fdev;
	disk->disk_attrs = fluidram_disk_attr_groups;
	snprintf(disk->disk_name, sizeof(disk->disk_name), "fluidram%d", dev_id);

	set_capacity(disk, size_bytes >> SECTOR_SHIFT);

	/* Set physical limits */
	blk_queue_flag_set(QUEUE_FLAG_NONROT, disk->queue);
	blk_queue_flag_set(QUEUE_FLAG_SYNCHRONOUS, disk->queue);

	ret = add_disk(disk);
	if (ret) {
		put_disk(disk);
		fluid_pool_destroy(fdev->pool);
		kfree(fdev);
		return ret;
	}

	fdev->disk = disk;
	fdev->init_done = true;
	fluidram_devs[dev_id] = fdev;

	pr_info("fluidram: created /dev/%s capacity %llu MB\n",
	        disk->disk_name, size_bytes / (1024 * 1024));
	return 0;
}

static void fluidram_destroy_device(int dev_id)
{
	struct fluidram_device *fdev = fluidram_devs[dev_id];

	if (!fdev)
		return;

	if (fdev->disk) {
		del_gendisk(fdev->disk);
		put_disk(fdev->disk);
	}
	if (fdev->pool)
		fluid_pool_destroy(fdev->pool);

	kfree(fdev);
	fluidram_devs[dev_id] = NULL;
}

static int __init fluidram_init(void)
{
	int ret, i;

	pr_info("fluidram: Initializing Hydrodynamic Memory Compression Driver (AdiOS Systems)\n");
	fluid_galois_init();

	fluidram_major = register_blkdev(0, FLUIDRAM_NAME);
	if (fluidram_major < 0)
		return fluidram_major;

	if (num_devices > FLUIDRAM_MAX_DEVICES)
		num_devices = FLUIDRAM_MAX_DEVICES;

	for (i = 0; i < num_devices; i++) {
		ret = fluidram_create_device(i, (u64)default_size_mb * 1024 * 1024);
		if (ret) {
			while (--i >= 0)
				fluidram_destroy_device(i);
			unregister_blkdev(fluidram_major, FLUIDRAM_NAME);
			return ret;
		}
	}

	/* Pair adjacent devices for hydrodynamic slab borrowing */
	if (num_devices >= 2) {
		fluid_pool_set_peer(fluidram_devs[0]->pool, fluidram_devs[1]->pool);
		fluid_pool_set_peer(fluidram_devs[1]->pool, fluidram_devs[0]->pool);
	}

	pr_info("fluidram: %d device(s) ready with Galois Field GF(2^8) acceleration\n", num_devices);
	return 0;
}

static void __exit fluidram_exit(void)
{
	int i;

	for (i = 0; i < num_devices; i++)
		fluidram_destroy_device(i);

	unregister_blkdev(fluidram_major, FLUIDRAM_NAME);
	pr_info("fluidram: driver unloaded\n");
}

module_init(fluidram_init);
module_exit(fluidram_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AdiOS Systems Group");
MODULE_DESCRIPTION("FluidRAM: Hydrodynamic Memory Compression Driver for Linux");
MODULE_VERSION("1.0.0");
#endif
