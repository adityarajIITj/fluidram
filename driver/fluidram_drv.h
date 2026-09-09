/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FluidRAM: Linux Block Device Driver Header
 *
 * Copyright (C) 2026 AdiOS Systems Group.
 */

#ifndef _FLUIDRAM_DRV_H
#define _FLUIDRAM_DRV_H

#include <linux/types.h>
#ifdef __KERNEL__
#include <linux/blkdev.h>
#include <linux/mutex.h>
#endif

#include "fluid_slab.h"

#define FLUIDRAM_NAME           "fluidram"
#define FLUIDRAM_MAX_DEVICES    16
#define SECTOR_SHIFT            9
#define SECTOR_SIZE             (1 << SECTOR_SHIFT)
#define PAGE_SECTORS            (FLUID_PAGE_SIZE >> SECTOR_SHIFT)

struct fluidram_device {
	int                 dev_id;
	u64                 disksize;
	struct fluid_pool  *pool;
#ifdef __KERNEL__
	struct gendisk     *disk;
	struct mutex        ctl_mutex;
	bool                init_done;
#endif
};

#endif /* _FLUIDRAM_DRV_H */
