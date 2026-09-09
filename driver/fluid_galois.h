/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FluidRAM: Galois Field GF(2^8) Sparse Delta Compression Engine
 *
 * Copyright (C) 2026 AdiOS Systems Group.
 */

#ifndef _FLUID_GALOIS_H
#define _FLUID_GALOIS_H

#include <linux/types.h>

#define FLUID_PAGE_SIZE         4096
#define FLUID_GF_POLY           0x11d   /* x^8 + x^4 + x^3 + x^2 + 1 */
#define FLUID_HEADER_MAGIC      0x464C5549 /* 'FLUI' */

enum fluid_page_type {
	FLUID_PAGE_ZERO        = 0x01,  /* All zero bytes - 0 bytes payload */
	FLUID_PAGE_UNIFORM     = 0x02,  /* Repeated single byte pattern */
	FLUID_PAGE_SPARSE_GF   = 0x03,  /* Sparse Galois differential delta */
	FLUID_PAGE_DENSE_GF    = 0x04,  /* Dense Galois polynomial stream */
	FLUID_PAGE_UNCOMPRESSED = 0x05  /* Incompressible fallback */
};

struct fluid_page_hdr {
	__u32 magic;
	__u8  type;
	__u8  param;        /* Pattern byte for UNIFORM, flags for GF */
	__u16 raw_len;      /* Should equal FLUID_PAGE_SIZE */
	__u16 comp_len;     /* Compressed payload length */
	__u16 crc16;        /* Checksum for bit-exact verification */
} __packed;

void fluid_galois_init(void);
int fluid_galois_compress_page(const u8 *src, u8 *dst, size_t max_dst_len, size_t *out_len);
int fluid_galois_decompress_page(const u8 *src, size_t src_len, u8 *dst, size_t dst_len);

#endif /* _FLUID_GALOIS_H */
