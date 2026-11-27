/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file memo_fpt.c
 * @brief FoxPro FPT memo files (spec \u00a78.3): 512-byte header block with a
 *        big-endian next-free block number (offset 0) and block size
 *        (offset 6, default 64). Blocks carry an 8-byte header: type (BE32,
 *        0 = text) and payload length (BE32), then the payload. Writes
 *        overwrite in place when the new memo fits the old allocation,
 *        otherwise extend from the free list. Text is converted between
 *        the DBF code page and UTF-8 at this boundary.
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

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wrbe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* convert UTF-8 text to the DBF code page for storage */
static int to_file_cp(const rx_dbf* dbf, const char* utf8, size_t n, uint8_t** out, size_t* outn) {
    if (dbf->codepage > 0) {
        uint8_t* b = malloc(n ? n : 1); /* UTF-8 -> single byte never expands */
        if (!b) {
            RX_FATAL("out of memory");
        }
        size_t bn = 0;
        int rc = utf8_to_cp(dbf->codepage, utf8, n, b, &bn);
        if (rc != R_OK) {
            free(b);
            return rc;
        }
        *out = b;
        *outn = bn;
        return R_OK;
    }
    uint8_t* b = malloc(n + 1);
    if (!b) {
        RX_FATAL("out of memory");
    }
    memcpy(b, utf8, n);
    *out = b;
    *outn = n;
    return R_OK;
}

static int from_file_cp(const rx_dbf* dbf, const uint8_t* raw, size_t n, r_str* out) {
    if (dbf->codepage > 0) {
        char* u8 = NULL;
        size_t un = 0;
        int rc = cp_to_utf8(dbf->codepage, raw, n, &u8, &un);
        if (rc != R_OK) {
            return rc;
        }
        out->utf8 = u8;
        out->n = un;
        return R_OK;
    }
    char* u8 = malloc(n + 1);
    if (!u8) {
        RX_FATAL("out of memory");
    }
    memcpy(u8, raw, n);
    u8[n] = '\0';
    out->utf8 = u8;
    out->n = n;
    return R_OK;
}

int rx_memo_fpt_read_raw(rx_dbf* dbf, uint32_t blk, uint8_t** bytes, size_t* n) {
    uint32_t bs = dbf->memo_bs ? dbf->memo_bs : 64;
    off_t off = (off_t)blk * bs;
    uint8_t h[8];
    ssize_t got = pread(dbf->memo_fd, h, sizeof(h), off);
    if (got < 8) {
        return R_ERR_DBF_MEMO;
    }
    uint32_t len = be32(h + 4);
    if (len > 64u * 1024 * 1024) {
        return R_ERR_DBF_MEMO; /* implausible: corrupt header */
    }
    uint8_t* buf = malloc(len ? len : 1);
    if (!buf) {
        RX_FATAL("out of memory");
    }
    got = pread(dbf->memo_fd, buf, len, off + 8);
    if (got < 0 || (size_t)got != len) {
        free(buf);
        return R_ERR_DBF_MEMO;
    }
    *bytes = buf;
    *n = len;
    return R_OK;
}

int rx_memo_fpt_read(rx_dbf* dbf, uint32_t blk, r_str** out) {
    uint8_t* raw = NULL;
    size_t rn = 0;
    int rc = rx_memo_fpt_read_raw(dbf, blk, &raw, &rn);
    if (rc != R_OK) {
        return rc;
    }
    r_str* s = calloc(1, sizeof(r_str));
    if (!s) {
        free(raw);
        RX_FATAL("out of memory");
    }
    rc = from_file_cp(dbf, raw, rn, s);
    free(raw);
    if (rc != R_OK) {
        free(s);
        return rc;
    }
    *out = s;
    return R_OK;
}

int rx_memo_fpt_write(rx_dbf* dbf, const char* data, size_t n, uint32_t old_blk,
                      uint32_t* blk_out) {
    uint8_t* cp = NULL;
    size_t cn = 0;
    int rc = to_file_cp(dbf, data, n, &cp, &cn);
    if (rc != R_OK) {
        return rc;
    }
    uint32_t bs = dbf->memo_bs ? dbf->memo_bs : 64;
    /* journal the before-image before touching anything */
    if (dbf->in_txn && old_blk) {
        uint8_t h[8];
        off_t ooff = (off_t)old_blk * bs;
        if (pread(dbf->memo_fd, h, 8, ooff) == 8) {
            uint32_t old_len = be32(h + 4);
            uint8_t* old = malloc(old_len ? old_len : 1);
            struct stat st;
            long oldsize = 0;
            if (fstat(dbf->memo_fd, &st) == 0) {
                oldsize = (long)st.st_size;
            }
            if (old && pread(dbf->memo_fd, old, old_len, ooff + 8) == (ssize_t)old_len) {
                rc = rx_journal_memo(dbf, old_blk, old, old_len, oldsize);
            } else {
                rc = rx_journal_memo(dbf, old_blk, NULL, 0, oldsize);
            }
            free(old);
            if (rc != R_OK) {
                free(cp);
                return rc;
            }
        }
    }
    uint32_t need = 8 + (uint32_t)cn;
    uint32_t alloc = (need + bs - 1) / bs * bs;
    uint32_t blk = 0;
    int in_place = 0;
    if (old_blk) {
        uint8_t h[8];
        off_t ooff = (off_t)old_blk * bs;
        if (pread(dbf->memo_fd, h, 8, ooff) == 8) {
            uint32_t old_len = be32(h + 4);
            uint32_t old_alloc = (8 + old_len + bs - 1) / bs * bs;
            if (need <= old_alloc) {
                blk = old_blk;
                in_place = 1;
            }
        }
    }
    if (!in_place) {
        blk = dbf->memo_next;
        dbf->memo_next = blk + alloc / bs;
        uint8_t nh[4];
        wrbe32(nh, dbf->memo_next);
        if (pwrite(dbf->memo_fd, nh, 4, 0) != 4) {
            free(cp);
            return R_ERR_IO_WRITE;
        }
    }
    uint8_t* block = calloc(1, alloc ? alloc : bs);
    if (!block) {
        free(cp);
        RX_FATAL("out of memory");
    }
    wrbe32(block, 0); /* type 0 = text */
    wrbe32(block + 4, (uint32_t)cn);
    memcpy(block + 8, cp, cn);
    free(cp);
    off_t off = (off_t)blk * bs;
    if (pwrite(dbf->memo_fd, block, alloc, off) != (ssize_t)alloc) {
        free(block);
        return R_ERR_IO_WRITE;
    }
    free(block);
    *blk_out = blk;
    return R_OK;
}
