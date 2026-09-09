// SPDX-License-Identifier: GPL-2.0-only
/*
 * FluidRAM: Hydrodynamic Slab Memory Manager Implementation
 *
 * Copyright (C) 2026 AdiOS Systems Group.
 */

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/errno.h>
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#endif

#include "fluid_slab.h"
#include "fluid_galois.h"

struct fluid_pool *fluid_pool_create(size_t num_pages, size_t quota_bytes)
{
	struct fluid_pool *pool;
	size_t entries_size;

	pool = vzalloc(sizeof(*pool));
	if (!pool)
		return NULL;

	entries_size = num_pages * sizeof(struct fluid_entry);
	pool->entries = vzalloc(entries_size);
	if (!pool->entries) {
		vfree(pool);
		return NULL;
	}

	pool->num_pages = num_pages;
	pool->quota_bytes = quota_bytes ? quota_bytes : (num_pages * FLUID_PAGE_SIZE);
	pool->phys_bytes_allocated = 0;
	pool->orig_bytes_written = 0;
	pool->borrow_count = 0;
	pool->zero_page_faults_avoided = 0;
	pool->peer_pool = NULL;
	spin_lock_init(&pool->lock);

	return pool;
}

void fluid_pool_destroy(struct fluid_pool *pool)
{
	size_t i;

	if (!pool)
		return;

	for (i = 0; i < pool->num_pages; i++) {
		if (pool->entries[i].data) {
			kfree(pool->entries[i].data);
			pool->entries[i].data = NULL;
		}
	}

	vfree(pool->entries);
	vfree(pool);
}

void fluid_pool_set_peer(struct fluid_pool *pool, struct fluid_pool *peer)
{
	if (!pool)
		return;
	pool->peer_pool = peer;
}

int fluid_pool_write_page(struct fluid_pool *pool, size_t index, const u8 *src)
{
	u8 comp_buf[FLUID_PAGE_SIZE + sizeof(struct fluid_page_hdr) + 128];
	size_t comp_len = 0;
	u8 *new_data = NULL;
	u8 *old_data = NULL;
	size_t old_len = 0;
	int ret;

	if (!pool || index >= pool->num_pages)
		return -EINVAL;

	ret = fluid_galois_compress_page(src, comp_buf, sizeof(comp_buf), &comp_len);
	if (ret < 0)
		return ret;

	/* If compressed page is pure zero, we don't need a physical allocation */
	if (comp_len == sizeof(struct fluid_page_hdr)) {
		const struct fluid_page_hdr *hdr = (const struct fluid_page_hdr *)comp_buf;
		if (hdr->type == FLUID_PAGE_ZERO) {
			spin_lock(&pool->lock);
			if (pool->entries[index].data) {
				old_data = pool->entries[index].data;
				old_len = pool->entries[index].len;
				pool->entries[index].data = NULL;
			}
			pool->entries[index].len = 0;
			pool->entries[index].flags = FLUID_ENTRY_FLAG_VALID | FLUID_ENTRY_FLAG_ZERO;
			pool->phys_bytes_allocated -= old_len;
			pool->orig_bytes_written += FLUID_PAGE_SIZE;
			pool->zero_page_faults_avoided++;
			spin_unlock(&pool->lock);

			if (old_data)
				kfree(old_data);
			return 0;
		}
	}

	new_data = kmalloc(comp_len, GFP_KERNEL);
	if (!new_data)
		return -ENOMEM;
	memcpy(new_data, comp_buf, comp_len);

	spin_lock(&pool->lock);

	/* Hydrodynamic Peer-to-Peer Borrowing Check */
	if (pool->phys_bytes_allocated + comp_len > pool->quota_bytes && pool->peer_pool) {
		/* Attempt to borrow unallocated quota from peer pool */
		spin_lock(&pool->peer_pool->lock);
		if (pool->peer_pool->phys_bytes_allocated + comp_len < pool->peer_pool->quota_bytes) {
			pool->peer_pool->quota_bytes -= comp_len;
			pool->quota_bytes += comp_len;
			pool->borrow_count++;
			pool->zero_page_faults_avoided++;
		}
		spin_unlock(&pool->peer_pool->lock);
	}

	if (pool->entries[index].data) {
		old_data = pool->entries[index].data;
		old_len = pool->entries[index].len;
	}

	pool->entries[index].data = new_data;
	pool->entries[index].len = (u16)comp_len;
	pool->entries[index].flags = FLUID_ENTRY_FLAG_VALID;

	pool->phys_bytes_allocated = pool->phys_bytes_allocated + comp_len - old_len;
	pool->orig_bytes_written += FLUID_PAGE_SIZE;
	pool->zero_page_faults_avoided++;

	spin_unlock(&pool->lock);

	if (old_data)
		kfree(old_data);

	return 0;
}

int fluid_pool_read_page(struct fluid_pool *pool, size_t index, u8 *dst)
{
	u8 *comp_data = NULL;
	size_t comp_len = 0;
	u16 flags = 0;
	int ret;

	if (!pool || index >= pool->num_pages)
		return -EINVAL;

	spin_lock(&pool->lock);
	if (!(pool->entries[index].flags & FLUID_ENTRY_FLAG_VALID)) {
		spin_unlock(&pool->lock);
		/* Unwritten page returns clean zeros */
		memset(dst, 0, FLUID_PAGE_SIZE);
		return 0;
	}

	if (pool->entries[index].flags & FLUID_ENTRY_FLAG_ZERO) {
		spin_unlock(&pool->lock);
		memset(dst, 0, FLUID_PAGE_SIZE);
		return 0;
	}

	comp_data = pool->entries[index].data;
	comp_len = pool->entries[index].len;
	flags = pool->entries[index].flags;

	if (!comp_data || comp_len == 0) {
		spin_unlock(&pool->lock);
		memset(dst, 0, FLUID_PAGE_SIZE);
		return 0;
	}

	ret = fluid_galois_decompress_page(comp_data, comp_len, dst, FLUID_PAGE_SIZE);
	spin_unlock(&pool->lock);

	return ret;
}

int fluid_pool_discard_page(struct fluid_pool *pool, size_t index)
{
	u8 *old_data = NULL;
	size_t old_len = 0;

	if (!pool || index >= pool->num_pages)
		return -EINVAL;

	spin_lock(&pool->lock);
	if (pool->entries[index].data) {
		old_data = pool->entries[index].data;
		old_len = pool->entries[index].len;
		pool->entries[index].data = NULL;
		pool->entries[index].len = 0;
		pool->entries[index].flags = 0;
		pool->phys_bytes_allocated -= old_len;
	}
	spin_unlock(&pool->lock);

	if (old_data)
		kfree(old_data);

	return 0;
}

void fluid_pool_get_stats(struct fluid_pool *pool, size_t *phys_bytes, size_t *orig_bytes,
                          u64 *borrow_count, u64 *faults_avoided)
{
	if (!pool)
		return;

	spin_lock(&pool->lock);
	if (phys_bytes)
		*phys_bytes = pool->phys_bytes_allocated;
	if (orig_bytes)
		*orig_bytes = pool->orig_bytes_written;
	if (borrow_count)
		*borrow_count = pool->borrow_count;
	if (faults_avoided)
		*faults_avoided = pool->zero_page_faults_avoided;
	spin_unlock(&pool->lock);
}

#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(fluid_pool_create);
EXPORT_SYMBOL_GPL(fluid_pool_destroy);
EXPORT_SYMBOL_GPL(fluid_pool_set_peer);
EXPORT_SYMBOL_GPL(fluid_pool_write_page);
EXPORT_SYMBOL_GPL(fluid_pool_read_page);
EXPORT_SYMBOL_GPL(fluid_pool_discard_page);
EXPORT_SYMBOL_GPL(fluid_pool_get_stats);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("AdiOS Systems Group");
MODULE_DESCRIPTION("Hydrodynamic Slab Memory Pool Allocator");
#endif
