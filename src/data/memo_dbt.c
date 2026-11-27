/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file memo_dbt.c
 * @brief dBase DBT memo files (spec \u00a78.3): 512-byte blocks; block 0 is the
 *        header with a little-endian next-free block number at offset 0.
 *        Reader: payload runs from blk*512 to the first 0x1A terminator.
 *        Writer: append-per-write for compatibility (new block number from
 *        the end of file, 512-aligned, data + 0x1A, zero padding).
 */
/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "data/data.h"
#include "util/util.h"

#define DBT_BLK 512

int rx_memo_dbt_read(rx_dbf* dbf, uint32_t blk, r_str** out) {
    off_t off = (off_t)blk * DBT_BLK;
    strbuf sb;
    sb_init(&sb);
    uint8_t chunk[DBT_BLK];
    int terminated = 0;
    for (off_t p = off;; p += DBT_BLK) {
        ssize_t got = pread(dbf->memo_fd, chunk, DBT_BLK, p);
        if (got <= 0) {
            break;
        }
        for (ssize_t i = 0; i < got; i++) {
            if (chunk[i] == 0x1A) {
                terminated = 1;
                break;
            }
            sb_putc(&sb, (char)chunk[i]);
        }
        if (terminated) {
            break;
        }
    }
    if (!terminated) {
        /* no terminator: strip trailing NUL/0x1A padding */
        while (sb.len > 0 && (sb.buf[sb.len - 1] == '\0' || (uint8_t)sb.buf[sb.len - 1] == 0x1A)) {
            sb.len--;
        }
        if (sb.buf) {
            sb.buf[sb.len] = '\0';
        }
    }
    r_str* s = calloc(1, sizeof(r_str));
    if (!s) {
        sb_free(&sb);
        RX_FATAL("out of memory");
    }
    int rc = R_OK;
    if (dbf->codepage > 0) {
        char* u8 = NULL;
        size_t un = 0;
        rc = cp_to_utf8(dbf->codepage, (const uint8_t*)(sb.buf ? sb.buf : ""), sb.len, &u8, &un);
        if (rc == R_OK) {
            s->utf8 = u8;
            s->n = un;
        }
    } else {
        char* u8 = malloc(sb.len + 1);
        if (!u8) {
            sb_free(&sb);
            free(s);
            RX_FATAL("out of memory");
        }
        if (sb.buf) {
            memcpy(u8, sb.buf, sb.len);
        }
        u8[sb.len] = '\0';
        s->utf8 = u8;
        s->n = sb.len;
    }
    sb_free(&sb);
    if (rc != R_OK) {
        free(s);
        return rc;
    }
    *out = s;
    return R_OK;
}

int rx_memo_dbt_write(rx_dbf* dbf, const char* data, size_t n, uint32_t old_blk,
                      uint32_t* blk_out) {
    (void)old_blk; /* DBT writer is append-per-write: old block is abandoned */
    uint8_t* cp = NULL;
    size_t cn = 0;
    int rc = R_OK;
    if (dbf->codepage > 0) {
        cp = malloc(n ? n : 1); /* UTF-8 -> single byte never expands */
        if (!cp) {
            RX_FATAL("out of memory");
        }
        rc = utf8_to_cp(dbf->codepage, data, n, cp, &cn);
    } else {
        cp = malloc(n + 1);
        if (!cp) {
            RX_FATAL("out of memory");
        }
        memcpy(cp, data, n);
        cn = n;
    }
    if (rc != R_OK) {
        return rc;
    }
    struct stat st;
    off_t size = 0;
    if (fstat(dbf->memo_fd, &st) == 0) {
        size = st.st_size;
    }
    if (dbf->in_txn) {
        rc = rx_journal_memo(dbf, old_blk, NULL, 0, (long)size);
        if (rc != R_OK) {
            free(cp);
            return rc;
        }
    }
    /* align to the next 512-byte boundary */
    off_t blk_off = (size + DBT_BLK - 1) / DBT_BLK * DBT_BLK;
    uint32_t blk = (uint32_t)(blk_off / DBT_BLK);
    size_t total = cn + 1; /* data + 0x1A */
    size_t padded = (total + DBT_BLK - 1) / DBT_BLK * DBT_BLK;
    uint8_t* block = calloc(1, padded ? padded : DBT_BLK);
    if (!block) {
        free(cp);
        RX_FATAL("out of memory");
    }
    memcpy(block, cp, cn);
    block[cn] = 0x1A;
    free(cp);
    if (pwrite(dbf->memo_fd, block, padded, blk_off) != (ssize_t)padded) {
        free(block);
        return R_ERR_IO_WRITE;
    }
    free(block);
    uint32_t next = blk + (uint32_t)(padded / DBT_BLK);
    uint8_t nh[4] = {(uint8_t)next, (uint8_t)(next >> 8), (uint8_t)(next >> 16),
                     (uint8_t)(next >> 24)};
    if (pwrite(dbf->memo_fd, nh, 4, 0) != 4) {
        return R_ERR_IO_WRITE;
    }
    dbf->memo_next = next;
    *blk_out = blk;
    return R_OK;
}
