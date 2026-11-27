/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cdx_internal.h
 * @brief Internal index-engine structures shared by cdx.c (CDX read/write),
 *        idx.c (standalone IDX write) and ntx_read.c (read-only NTX import).
 *        Not part of the frozen public API (include/reynard/dbf.h).
 *
 * On-disk layout (docs/formats.md §4, spec §8.4): 500-byte little-endian
 * blocks. CDX block 0..n-1 are the tag directory (sig "RCDX", version,
 * per-block entry count, next-dir pointer, then 236-byte tag headers chained
 * across dir blocks); each tag header stores name[11], key expr[128],
 * FOR expr[64], flags u16 @204, root page u32 @206, page count u32 @210,
 * key count u64 @222. Node blocks: u16 type (1 root / 2 internal / 3 leaf),
 * u16 key count, u32 left/right sibling, u32 parent/first-child, entries
 * from offset 16 (leaf: u16 klen + key + u32 recno; internal: + u32 child).
 * IDX is the same node format with a single-tag "RIDX" header block 0.
 */
#ifndef REYNARD_CDX_INTERNAL_H
#define REYNARD_CDX_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "data.h"

#define RX_CDX_BLOCK 500
#define RX_CDX_SIG 0x58444352u /* "RCDX" */
#define RX_IDX_SIG 0x58444449u /* "RIDX" */
#define RX_CDX_VERSION 1u

#define RX_NODE_ROOT 1
#define RX_NODE_INTERNAL 2
#define RX_NODE_LEAF 3

#define RX_TAG_HDR_SIZE 236
#define RX_DIR_HDR_SIZE 24
#define RX_DIR_ENTRIES ((RX_CDX_BLOCK - RX_DIR_HDR_SIZE) / RX_TAG_HDR_SIZE)

typedef struct rx_ent {
    uint8_t* key;
    size_t klen;
    uint32_t recno;
} rx_ent;

typedef struct rx_tag {
    char name[12];
    char key_expr[256];
    char for_expr[65]; /* 64 on disk + NUL */
    int flags;
    rx_ent* ents;
    size_t n, cap;
    int dirty;
    int sorted; /**< ents[] sorted when set; cleared on append (lazy sort) */
} rx_tag;

struct rx_idx {
    char path[264];
    int is_cdx;     /* compound CDX vs standalone IDX */
    int is_ntx;     /* read-only NTX/NDX import */
    int structural; /* opened automatically with the DBF */
    int writable;
    int fd;
    rx_tag* tags;
    size_t ntags, captags;
};

/* cdx.c */
int rx_cdx_write_tree(int fd, rx_tag* t, uint32_t* next, uint32_t* root_out);
int rx_cdx_blk_read(int fd, uint32_t blk, uint8_t out[RX_CDX_BLOCK]);
int rx_cdx_blk_write(int fd, uint32_t blk, const uint8_t buf[RX_CDX_BLOCK]);
uint16_t rx_cdx_get_u16(const uint8_t* p);
uint32_t rx_cdx_get_u32(const uint8_t* p);
uint64_t rx_cdx_get_u64(const uint8_t* p);
void rx_cdx_wr_u16(uint8_t* p, uint16_t v);
void rx_cdx_wr_u32(uint8_t* p, uint32_t v);
void rx_cdx_wr_u64(uint8_t* p, uint64_t v);
void rx_cdx_tag_clear_ents(rx_tag* t);
/** Append one entry assuming ascending key order already holds (NTX import). */
int rx_cdx_tag_push(rx_tag* t, const uint8_t* key, size_t klen, uint32_t recno);
/** Sorted insert honoring the tag's UNIQUE/DESCENDING flags (lazy: O(1)
 *  append + qsort on first read). */
int rx_cdx_tag_insert_sorted(rx_tag* t, const uint8_t* key, size_t klen, uint32_t recno);
/** Sort the entry array if dirty (seek / positional access / flush). */
void rx_cdx_tag_ensure_sorted(rx_tag* t);
int rx_cdx_idx_add(rx_dbf* dbf, rx_idx* ix);
int rx_cdx_idx_add_front(rx_dbf* dbf, rx_idx* ix);
rx_idx* rx_cdx_idx_new(const char* path, int is_cdx, int is_ntx);

/* idx.c */
int rx_idx_write_file(rx_idx* ix);

/* ntx_read.c */
int rx_ntx_read_file(rx_dbf* dbf, const char* path, rx_idx* ix);

#endif /* REYNARD_CDX_INTERNAL_H */
