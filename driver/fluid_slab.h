/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FluidRAM: Hydrodynamic Slab Memory Manager Header
 *
 * Copyright (C) 2026 AdiOS Systems Group.
 */

#ifndef _FLUID_SLAB_H
#define _FLUID_SLAB_H

#include <linux/types.h>
#ifdef __KERNEL__
#include <linux/spinlock.h>
#include <linux/slab.h>
#else
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef pthread_mutex_t spinlock_t;
#define spin_lock_init(l)   pthread_mutex_init(l, NULL)
#define spin_lock(l)        pthread_mutex_lock(l)
#define spin_unlock(l)      pthread_mutex_unlock(l)
#define spin_lock_irqsave(l, f) do { (void)(f); pthread_mutex_lock(l); } while(0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); pthread_mutex_unlock(l); } while(0)
#define kmalloc(s, f)       malloc(s)
#define kfree(p)            free(p)
#define vzalloc(s)          calloc(1, s)
#define vfree(p)            free(p)
#define GFP_ATOMIC          0
#define GFP_KERNEL          0
#endif

#include "fluid_galois.h"

struct fluid_entry {
	u8  *data;
	u16 len;
	u16 flags;
};

#define FLUID_ENTRY_FLAG_VALID    0x0001
#define FLUID_ENTRY_FLAG_ZERO     0x0002
#define FLUID_ENTRY_FLAG_BORROWED 0x0004

struct fluid_pool {
	struct fluid_entry *entries;
	size_t              num_pages;
	size_t              quota_bytes;
	size_t              phys_bytes_allocated;
	size_t              orig_bytes_written;
	u64                 borrow_count;
	u64                 zero_page_faults_avoided;
	spinlock_t          lock;
	struct fluid_pool  *peer_pool; /* Hydrodynamic peer for quota borrowing */
};

struct fluid_pool *fluid_pool_create(size_t num_pages, size_t quota_bytes);
void fluid_pool_destroy(struct fluid_pool *pool);
void fluid_pool_set_peer(struct fluid_pool *pool, struct fluid_pool *peer);
int fluid_pool_write_page(struct fluid_pool *pool, size_t index, const u8 *src);
int fluid_pool_read_page(struct fluid_pool *pool, size_t index, u8 *dst);
int fluid_pool_discard_page(struct fluid_pool *pool, size_t index);
void fluid_pool_get_stats(struct fluid_pool *pool, size_t *phys_bytes, size_t *orig_bytes,
                          u64 *borrow_count, u64 *faults_avoided);

#endif /* _FLUID_SLAB_H */
