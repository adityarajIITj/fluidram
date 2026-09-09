// SPDX-License-Identifier: GPL-2.0-only
/*
 * FluidRAM: Galois Field GF(2^8) Sparse Delta Compression Implementation
 *
 * Copyright (C) 2026 AdiOS Systems Group.
 */

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/crc16.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define __packed __attribute__((packed))
static inline u16 crc16(u16 crc, const u8 *buffer, size_t len)
{
	while (len--) {
		crc ^= *buffer++;
		for (int i = 0; i < 8; i++) {
			if (crc & 1)
				crc = (crc >> 1) ^ 0xA001;
			else
				crc >>= 1;
		}
	}
	return crc;
}
#endif

#include "fluid_galois.h"

static u8 gf_exp[512];
static u8 gf_log[256];
static int gf_initialized = 0;

void fluid_galois_init(void)
{
	int x = 1;
	int i;

	if (gf_initialized)
		return;

	for (i = 0; i < 255; i++) {
		gf_exp[i] = (u8)x;
		gf_log[x] = (u8)i;
		x <<= 1;
		if (x & 0x100)
			x ^= FLUID_GF_POLY;
	}
	for (i = 255; i < 512; i++)
		gf_exp[i] = gf_exp[i - 255];

	gf_initialized = 1;
}

static inline u8 gf_mul(u8 a, u8 b)
{
	if (a == 0 || b == 0)
		return 0;
	return gf_exp[gf_log[a] + gf_log[b]];
}

/*
 * Fast detection of uniform pages (e.g. all 0x00, all 0xFF)
 */
static int is_page_uniform(const u8 *src, u8 *val)
{
	const u64 *q = (const u64 *)src;
	u8 first = src[0];
	u64 pattern;
	size_t words = FLUID_PAGE_SIZE / sizeof(u64);
	size_t i;

	memset(&pattern, first, sizeof(pattern));
	for (i = 0; i < words; i++) {
		if (q[i] != pattern)
			return 0;
	}
	*val = first;
	return 1;
}

/*
 * Compress a 4096-byte page using Galois differential encoding
 */
int fluid_galois_compress_page(const u8 *src, u8 *dst, size_t max_dst_len, size_t *out_len)
{
	struct fluid_page_hdr hdr;
	u8 uniform_val = 0;
	u8 *payload;
	size_t p_idx = 0;
	u16 non_zero_count = 0;
	u16 i;
	u16 page_crc;

	if (!gf_initialized)
		fluid_galois_init();

	if (max_dst_len < sizeof(hdr))
		return -ENOSPC;

	page_crc = crc16(0, src, FLUID_PAGE_SIZE);

	/* Check for zero or uniform pages */
	if (is_page_uniform(src, &uniform_val)) {
		hdr.magic = FLUID_HEADER_MAGIC;
		hdr.type = (uniform_val == 0) ? FLUID_PAGE_ZERO : FLUID_PAGE_UNIFORM;
		hdr.param = uniform_val;
		hdr.raw_len = FLUID_PAGE_SIZE;
		hdr.comp_len = 0;
		hdr.crc16 = page_crc;
		memcpy(dst, &hdr, sizeof(hdr));
		*out_len = sizeof(hdr);
		return 0;
	}

	/* Count non-zero differential values */
	for (i = 0; i < FLUID_PAGE_SIZE; i++) {
		if (src[i] != 0)
			non_zero_count++;
	}

	payload = dst + sizeof(hdr);

	/* Sparse Galois differential mode: non-zero count < 1024 (75% sparsity) */
	if (non_zero_count < 1024) {
		if (max_dst_len < sizeof(hdr) + (size_t)non_zero_count * 3 + 2)
			goto fallback_uncompressed;

		/* First 2 bytes: non-zero count */
		payload[p_idx++] = (u8)(non_zero_count & 0xFF);
		payload[p_idx++] = (u8)((non_zero_count >> 8) & 0xFF);

		/* Store (offset: 16-bit, value: 8-bit) */
		for (i = 0; i < FLUID_PAGE_SIZE; i++) {
			if (src[i] != 0) {
				payload[p_idx++] = (u8)(i & 0xFF);
				payload[p_idx++] = (u8)((i >> 8) & 0xFF);
				payload[p_idx++] = src[i];
			}
		}

		hdr.magic = FLUID_HEADER_MAGIC;
		hdr.type = FLUID_PAGE_SPARSE_GF;
		hdr.param = 0;
		hdr.raw_len = FLUID_PAGE_SIZE;
		hdr.comp_len = (u16)p_idx;
		hdr.crc16 = page_crc;
		memcpy(dst, &hdr, sizeof(hdr));
		*out_len = sizeof(hdr) + p_idx;
		return 0;
	}

	/* Dense Galois polynomial delta: differential stream */
	{
		u8 prev = 0;
		size_t run_len = 0;
		u8 run_val = 0;

		/* Byte-differential delta stream */
		for (i = 0; i < FLUID_PAGE_SIZE; i++) {
			u8 delta = src[i] ^ prev; /* Galois addition in GF(2^8) */
			prev = src[i];

			if (delta == run_val && run_len < 255) {
				run_len++;
			} else {
				if (run_len > 0) {
					if (p_idx + 2 > max_dst_len - sizeof(hdr))
						goto fallback_uncompressed;
					payload[p_idx++] = (u8)run_len;
					payload[p_idx++] = run_val;
				}
				run_val = delta;
				run_len = 1;
			}
		}
		if (run_len > 0) {
			if (p_idx + 2 > max_dst_len - sizeof(hdr))
				goto fallback_uncompressed;
			payload[p_idx++] = (u8)run_len;
			payload[p_idx++] = run_val;
		}

		/* Only accept if compression saved at least 25% */
		if (p_idx + sizeof(hdr) < (FLUID_PAGE_SIZE * 3) / 4) {
			hdr.magic = FLUID_HEADER_MAGIC;
			hdr.type = FLUID_PAGE_DENSE_GF;
			hdr.param = 0;
			hdr.raw_len = FLUID_PAGE_SIZE;
			hdr.comp_len = (u16)p_idx;
			hdr.crc16 = page_crc;
			memcpy(dst, &hdr, sizeof(hdr));
			*out_len = sizeof(hdr) + p_idx;
			return 0;
		}
	}

fallback_uncompressed:
	if (max_dst_len < sizeof(hdr) + FLUID_PAGE_SIZE)
		return -ENOSPC;

	hdr.magic = FLUID_HEADER_MAGIC;
	hdr.type = FLUID_PAGE_UNCOMPRESSED;
	hdr.param = 0;
	hdr.raw_len = FLUID_PAGE_SIZE;
	hdr.comp_len = FLUID_PAGE_SIZE;
	hdr.crc16 = page_crc;
	memcpy(dst, &hdr, sizeof(hdr));
	memcpy(dst + sizeof(hdr), src, FLUID_PAGE_SIZE);
	*out_len = sizeof(hdr) + FLUID_PAGE_SIZE;
	return 0;
}

/*
 * Bit-exact reconstruction / decompression
 */
int fluid_galois_decompress_page(const u8 *src, size_t src_len, u8 *dst, size_t dst_len)
{
	const struct fluid_page_hdr *hdr;
	const u8 *payload;
	u16 check_crc;

	if (!gf_initialized)
		fluid_galois_init();

	if (src_len < sizeof(*hdr) || dst_len < FLUID_PAGE_SIZE)
		return -EINVAL;

	hdr = (const struct fluid_page_hdr *)src;
	if (hdr->magic != FLUID_HEADER_MAGIC)
		return -EBADMSG;

	payload = src + sizeof(*hdr);

	switch (hdr->type) {
	case FLUID_PAGE_ZERO:
		memset(dst, 0, FLUID_PAGE_SIZE);
		break;

	case FLUID_PAGE_UNIFORM:
		memset(dst, hdr->param, FLUID_PAGE_SIZE);
		break;

	case FLUID_PAGE_SPARSE_GF: {
		u16 non_zero_count;
		size_t p_idx = 0;
		u16 k;

		if (hdr->comp_len < 2)
			return -EBADMSG;

		non_zero_count = (u16)payload[0] | ((u16)payload[1] << 8);
		p_idx = 2;

		if (hdr->comp_len != 2 + non_zero_count * 3)
			return -EBADMSG;

		memset(dst, 0, FLUID_PAGE_SIZE);
		for (k = 0; k < non_zero_count; k++) {
			u16 offset = (u16)payload[p_idx] | ((u16)payload[p_idx + 1] << 8);
			u8 val = payload[p_idx + 2];
			p_idx += 3;
			if (offset >= FLUID_PAGE_SIZE)
				return -EBADMSG;
			dst[offset] = val;
		}
		break;
	}

	case FLUID_PAGE_DENSE_GF: {
		size_t p_idx = 0;
		size_t out_idx = 0;
		u8 prev = 0;

		while (p_idx + 1 < hdr->comp_len && out_idx < FLUID_PAGE_SIZE) {
			u8 count = payload[p_idx++];
			u8 delta = payload[p_idx++];
			while (count-- && out_idx < FLUID_PAGE_SIZE) {
				prev ^= delta;
				dst[out_idx++] = prev;
			}
		}
		if (out_idx != FLUID_PAGE_SIZE)
			return -EBADMSG;
		break;
	}

	case FLUID_PAGE_UNCOMPRESSED:
		if (hdr->comp_len != FLUID_PAGE_SIZE)
			return -EBADMSG;
		memcpy(dst, payload, FLUID_PAGE_SIZE);
		break;

	default:
		return -EINVAL;
	}

	/* Bit-exact verification */
	check_crc = crc16(0, dst, FLUID_PAGE_SIZE);
	if (check_crc != hdr->crc16)
		return -EILSEQ;

	return 0;
}

#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(fluid_galois_init);
EXPORT_SYMBOL_GPL(fluid_galois_compress_page);
EXPORT_SYMBOL_GPL(fluid_galois_decompress_page);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("AdiOS Systems Group");
MODULE_DESCRIPTION("Galois Field GF(2^8) Sparse Differential Compression");
#endif
