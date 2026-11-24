/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file ntx_read.c
 * @brief Read-only NTX/NDX (dBase III/IV) import (spec §8.4, M6).
 *
 * NTX files use 1024-byte pages. The 512-byte header carries the version
 * (u16 @0, 3 = dBase III), key length (u16 @8), max keys per page (u16 @12),
 * root page (u32 @16), a flags byte (@28, bit 0 = compressed) and the key
 * expression text (@32, 120 bytes). Pages hold u16 key count @0 followed by
 * that many entries of (u32 child page, u32 recno, key bytes), then one final
 * u32 right-most child pointer.
 *
 * Only uncompressed indexes are imported; compressed variants produce the
 * R_ERR_CDX_VARIANT diagnostic (spec §8.4: users convert via INDEX ON ... TO
 * new). Entries are pushed in leaf order into a single tag named after the
 * file stem. Best effort without reference fixtures: any structural
 * inconsistency also yields R_ERR_CDX_VARIANT (see docs/DEVIATIONS.md).
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cdx_internal.h"

#include "util/util.h"

#define _POSIX_C_SOURCE 200809L

#include "cdx_internal.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/util.h"

#define NTX_PAGE 1024

static int ntx_read_page(int fd, uint32_t page, uint8_t out[NTX_PAGE])
{
    off_t off = (off_t)page * NTX_PAGE;
    size_t got = 0;
    while (got < NTX_PAGE) {
        ssize_t r = pread(fd, out + got, NTX_PAGE - got, off + (off_t)got);
        if (r <= 0) {
            return R_ERR_IO_READ;
        }
        got += (size_t)r;
    }
    return R_OK;
}

/* walk one node; leaf entries (recno != 0 marker handled by caller) */
static int ntx_walk(int fd, rx_tag *t, uint32_t page, uint16_t keylen, uint32_t depth,
                    uint32_t *visited)
{
    if (depth > 32 || page > 100000000u || (*visited)++ > 10000000u) {
        return R_ERR_CDX_NODE;
    }
    uint8_t buf[NTX_PAGE];
    int rc = ntx_read_page(fd, page, buf);
    if (rc != R_OK) {
        return rc;
    }
    uint16_t nk = rx_cdx_get_u16(buf);
    if (nk == 0 || 2 + (size_t)(nk + 1) * 4 + (size_t)nk * keylen > NTX_PAGE) {
        return R_ERR_CDX_NODE;
    }
    size_t off = 2;
    for (uint16_t i = 0; i < nk; i++) {
        uint32_t child = rx_cdx_get_u32(buf + off);
        uint32_t recno = rx_cdx_get_u32(buf + off + 4);
        const uint8_t *key = buf + off + 8;
        off += 8 + keylen;
        if (child != 0) {
            rc = ntx_walk(fd, t, child, keylen, depth + 1, visited);
            if (rc != R_OK) {
                return rc;
            }
        }
        if (recno == 0) {
            return R_ERR_CDX_NODE;
        }
        rc = rx_cdx_tag_push(t, key, keylen, recno);
        if (rc != R_OK) {
            return rc;
        }
    }
    uint32_t right = rx_cdx_get_u32(buf + off);
    if (right != 0) {
        rc = ntx_walk(fd, t, right, keylen, depth + 1, visited);
    }
    return rc;
}

int rx_ntx_read_file(rx_dbf *dbf, const char *path, rx_idx *ix)
{
    (void)dbf;
    if (!path || !ix) {
        return R_ERR_RANGE;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return R_ERR_IO_NOTFOUND;
    }
    uint8_t hdr[512];
    ssize_t got = pread(fd, hdr, sizeof(hdr), 0);
    if (got < 32) {
        close(fd);
        return R_ERR_CDX_VARIANT; /* too small to be an NTX header */
    }
    uint16_t version = rx_cdx_get_u16(hdr);
    if (version != 3) {
        close(fd);
        return R_ERR_CDX_VARIANT; /* not a dBase III NTX/NDX file */
    }
    if (hdr[28] & 0x01) {
        close(fd);
        return R_ERR_CDX_VARIANT; /* compressed NTX: diagnostic per spec §8.4 */
    }
    uint16_t keylen = rx_cdx_get_u16(hdr + 8);
    uint32_t root = rx_cdx_get_u32(hdr + 16);
    if (keylen == 0 || keylen > 100 || root == 0) {
        close(fd);
        return R_ERR_CDX_VARIANT;
    }
    ix->tags = calloc(1, sizeof(rx_tag));
    if (!ix->tags) {
        RX_FATAL("out of memory");
    }
    ix->captags = 1;
    rx_tag *t = &ix->tags[ix->ntags++];
    memset(t, 0, sizeof(*t));
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t sl = 0;
    while (base[sl] && base[sl] != '.' && sl < 11) {
        t->name[sl] = (char)toupper((unsigned char)base[sl]);
        sl++;
    }
    t->name[sl] = '\0';
    memcpy(t->key_expr, hdr + 32, 120);
    t->key_expr[120] = '\0';
    uint32_t visited = 0;
    int rc = ntx_walk(fd, t, root, keylen, 0, &visited);
    close(fd);
    if (rc != R_OK) {
        rx_cdx_tag_clear_ents(t);
        free(ix->tags);
        ix->tags = NULL;
        ix->ntags = ix->captags = 0;
        return R_ERR_CDX_VARIANT;
    }
    t->dirty = 0;
    return R_OK;
}
