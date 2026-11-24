/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file idx.c
 * @brief Standalone single-key IDX write path (spec §8.4, M6).
 *
 * An IDX file shares the 500-byte B+tree node format with CDX but has a
 * single implicit tag described by header block 0: sig "RIDX", version,
 * key expression[128] @8, FOR expression[64] @136, flags u16 @200,
 * root page u32 @204, key count u64 @208, page count u32 @216. Tree blocks
 * start at page 1. Reads of the same layout live in cdx.c (rx_index_attach).
 * Writes are deterministic for a given entry set (same bulk-build writer as
 * CDX), so the round-trip gate can require byte-identical output.
 */
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cdx_internal.h"

#include "util/util.h"

#define _POSIX_C_SOURCE 200809L

#include "cdx_internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/util.h"

int rx_idx_write_file(rx_idx *ix)
{
    if (!ix || ix->ntags == 0) {
        return R_ERR_CDX_TAG;
    }
    int fd = open(ix->path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return R_ERR_IO_OPEN;
    }
    rx_tag *t = &ix->tags[0];
    rx_cdx_tag_ensure_sorted(t);
    uint32_t next = 1;
    uint32_t root = 0;
    int rc = rx_cdx_write_tree(fd, t, &next, &root);
    if (rc != R_OK) {
        close(fd);
        return rc;
    }
    if (ftruncate(fd, (off_t)next * RX_CDX_BLOCK) != 0) {
        close(fd);
        return R_ERR_IO_WRITE;
    }
    uint8_t buf[RX_CDX_BLOCK];
    memset(buf, 0, sizeof(buf));
    rx_cdx_wr_u32(buf, RX_IDX_SIG);
    rx_cdx_wr_u16(buf + 4, RX_CDX_VERSION);
    memcpy(buf + 8, t->key_expr, strlen(t->key_expr) > 128 ? 128 : strlen(t->key_expr));
    memcpy(buf + 136, t->for_expr, strlen(t->for_expr) > 64 ? 64 : strlen(t->for_expr));
    rx_cdx_wr_u16(buf + 200, (uint16_t)t->flags);
    rx_cdx_wr_u32(buf + 204, root);
    rx_cdx_wr_u64(buf + 208, (uint64_t)t->n);
    rx_cdx_wr_u32(buf + 216, next - 1);
    rc = rx_cdx_blk_write(fd, 0, buf);
    close(fd);
    if (rc == R_OK) {
        t->dirty = 0;
    }
    return rc;
}
