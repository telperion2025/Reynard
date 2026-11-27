/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file journal.c
 * @brief Before-image transaction journal (spec \u00a78.6): BEGIN TRANSACTION
 *        opens a <dbfstem>.RJN sidecar and stamps header byte 30; record
 *        writes, appends, header changes and memo writes journal their
 *        before-images; COMMIT clears the stamp and removes the sidecar;
 *        ROLLBACK replays the sidecar in reverse. A fatal error mid-commit
 *        leaves the stamp, and the next open completes-or-rolls back
 *        (rx_journal_recover). PACK/ZAP are refused inside a transaction.
 *
 * Sidecar layout: "RJN1" then entries, each: 1 tag byte + payload.
 *   'R' u32le recno + rec_size raw record bytes
 *   'H' 32 raw header bytes
 *   'A' u32le recno (appended record)
 *   'M' u32le blk + u32le has_payload + u32le oldn + oldn bytes + i64le oldsize
 *   'C' commit marker
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

static void w32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t r32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int jwrite(rx_dbf* dbf, const void* buf, size_t n) {
    ssize_t w = write(dbf->rjn_fd, buf, n);
    return w == (ssize_t)n ? R_OK : R_ERR_IO_WRITE;
}

static int jentry(rx_dbf* dbf, char tag, const void* payload, size_t n) {
    if (jwrite(dbf, &tag, 1) != R_OK) {
        return R_ERR_IO_WRITE;
    }
    return jwrite(dbf, payload, n);
}

int rx_journal_rec(rx_dbf* dbf, uint32_t recno) {
    if (!dbf || !dbf->in_txn) {
        return R_OK;
    }
    for (size_t i = 0; i < dbf->njrecs; i++) {
        if (dbf->jrecs[i] == recno) {
            return R_OK; /* first before-image wins */
        }
    }
    if (dbf->njrecs == dbf->capjrecs) {
        size_t nc = dbf->capjrecs ? dbf->capjrecs * 2 : 16;
        uint32_t* na = realloc(dbf->jrecs, nc * sizeof(uint32_t));
        if (!na) {
            RX_FATAL("out of memory");
        }
        dbf->jrecs = na;
        dbf->capjrecs = nc;
    }
    dbf->jrecs[dbf->njrecs++] = recno;
    uint8_t hdr4[4];
    w32(hdr4, recno);
    int rc = jentry(dbf, 'R', hdr4, 4);
    if (rc != R_OK) {
        return rc;
    }
    /* The before-image is the record as it sits on disk right now: callers
       (rx_flush) journal AFTER rx_dbf_put has already encoded the new value
       into recbuf, so journaling recbuf would capture the after-image.
       The jrecs dedup above guarantees this runs only on the record's first
       flush inside the transaction, when disk still holds the pre-transaction
       image (any earlier change to this record was flushed -- and thus
       journaled -- already). Fall back to recbuf if the pread comes up short
       (e.g. record not yet on disk). */
    uint8_t* img = malloc(dbf->rec_size);
    if (!img) {
        RX_FATAL("out of memory");
    }
    off_t off = (off_t)dbf->header_size + (off_t)(recno - 1) * dbf->rec_size;
    if (pread(dbf->fd, img, dbf->rec_size, off) != (ssize_t)dbf->rec_size) {
        memcpy(img, dbf->recbuf, dbf->rec_size);
    }
    rc = jwrite(dbf, img, dbf->rec_size);
    free(img);
    return rc;
}

int rx_journal_header(rx_dbf* dbf) {
    if (!dbf || !dbf->in_txn || dbf->jhdr) {
        return R_OK;
    }
    dbf->jhdr = 1;
    uint8_t hdr[32];
    if (pread(dbf->fd, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
        return R_ERR_IO_READ;
    }
    return jentry(dbf, 'H', hdr, sizeof(hdr));
}

int rx_journal_append(rx_dbf* dbf, uint32_t recno) {
    if (!dbf || !dbf->in_txn) {
        return R_OK;
    }
    uint8_t b[4];
    w32(b, recno);
    return jentry(dbf, 'A', b, 4);
}

int rx_journal_memo(rx_dbf* dbf, uint32_t blk, const uint8_t* old, size_t oldn, long oldsize) {
    if (!dbf || !dbf->in_txn) {
        return R_OK;
    }
    uint8_t h[20];
    w32(h, blk);
    w32(h + 4, old ? 1 : 0);
    w32(h + 8, (uint32_t)oldn);
    uint64_t os = (uint64_t)oldsize;
    for (int i = 0; i < 8; i++) {
        h[12 + i] = (uint8_t)(os >> (8 * i));
    }
    int rc = jentry(dbf, 'M', h, 20);
    if (rc != R_OK) {
        return rc;
    }
    if (old && oldn) {
        rc = jwrite(dbf, old, oldn);
    }
    return rc;
}

int rx_begin(rx_dbf* dbf) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->readonly) {
        return R_ERR_IO_PERM;
    }
    if (dbf->in_txn) {
        return R_ERR_DBF_UNSUPPORTED; /* no nested transactions */
    }
    int fd = open(dbf->rjn_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return R_ERR_IO_OPEN;
    }
    dbf->rjn_fd = fd;
    dbf->in_txn = 1;
    dbf->njrecs = 0;
    dbf->jhdr = 0;
    if (jwrite(dbf, "RJN1", 4) != R_OK) {
        rx_rollback(dbf);
        return R_ERR_IO_WRITE;
    }
    if (rx_dbf_write_header(dbf) != R_OK) { /* stamps byte 30 = 1 */
        rx_rollback(dbf);
        return R_ERR_IO_WRITE;
    }
    return R_OK;
}

/* replay the sidecar in reverse, restoring before-images */
static int rollback_apply(rx_dbf* dbf) {
    rx_dbf_cache_inv(dbf); /* replay rewrites the file: drop read-ahead (M10) */
    struct stat st;
    if (fstat(dbf->rjn_fd, &st) != 0 || st.st_size < 4) {
        return R_ERR_IO_READ;
    }
    size_t total = (size_t)st.st_size;
    uint8_t* buf = malloc(total);
    if (!buf) {
        RX_FATAL("out of memory");
    }
    if (pread(dbf->rjn_fd, buf, total, 0) != (ssize_t)total) {
        free(buf);
        return R_ERR_IO_READ;
    }
    if (memcmp(buf, "RJN1", 4) != 0) {
        free(buf);
        return R_ERR_DBF_TRUNCATED;
    }
    /* collect entry offsets, then walk backwards */
    size_t* offs = NULL;
    size_t noff = 0, capoff = 0;
    size_t p = 4;
    while (p < total) {
        if (noff == capoff) {
            size_t nc = capoff ? capoff * 2 : 32;
            size_t* na = realloc(offs, nc * sizeof(size_t));
            if (!na) {
                free(offs);
                free(buf);
                RX_FATAL("out of memory");
            }
            offs = na;
            capoff = nc;
        }
        offs[noff++] = p;
        char tag = (char)buf[p];
        size_t plen = 0;
        if (tag == 'R') {
            plen = 4 + dbf->rec_size;
        } else if (tag == 'H') {
            plen = 32;
        } else if (tag == 'A') {
            plen = 4;
        } else if (tag == 'M') {
            if (p + 20 > total) {
                break;
            }
            plen = 20 + r32(buf + p + 1 + 8);
        } else if (tag == 'C') {
            plen = 0;
        } else {
            break;
        }
        p += 1 + plen;
    }
    int rc = R_OK;
    for (size_t i = noff; i-- > 0 && rc == R_OK;) {
        size_t e = offs[i];
        char tag = (char)buf[e];
        const uint8_t* pl = buf + e + 1;
        if (tag == 'R') {
            uint32_t recno = r32(pl);
            off_t off = (off_t)dbf->header_size + (off_t)(recno - 1) * dbf->rec_size;
            if (pwrite(dbf->fd, pl + 4, dbf->rec_size, off) != (ssize_t)dbf->rec_size) {
                rc = R_ERR_IO_WRITE;
            }
        } else if (tag == 'H') {
            if (pwrite(dbf->fd, pl, 32, 0) != 32) {
                rc = R_ERR_IO_WRITE;
            } else {
                dbf->nrec = r32(pl + 4);
            }
        } else if (tag == 'A') {
            uint32_t recno = r32(pl);
            off_t off = (off_t)dbf->header_size + (off_t)(recno - 1) * dbf->rec_size + 1;
            if (rx_ftruncate(dbf->fd, (long)off) != 0) {
                rc = R_ERR_IO_WRITE;
            } else {
                dbf->nrec = recno - 1;
            }
        } else if (tag == 'M') {
            uint32_t blk = r32(pl);
            uint32_t has = r32(pl + 4);
            uint32_t oldn = r32(pl + 8);
            uint64_t oldsize = 0;
            for (int k = 0; k < 8; k++) {
                oldsize |= (uint64_t)pl[12 + k] << (8 * k);
            }
            if (dbf->has_memo && dbf->memo_fd >= 0) {
                off_t moff =
                    dbf->memo_kind == RX_MEMO_FPT ? (off_t)blk * dbf->memo_bs : (off_t)blk * 512;
                if (has && oldn) {
                    if (pwrite(dbf->memo_fd, pl + 20, oldn, moff + 8) != (ssize_t)oldn) {
                        rc = R_ERR_IO_WRITE;
                    }
                }
                if (rx_ftruncate(dbf->memo_fd, (long)oldsize) != 0) {
                    rc = R_ERR_IO_WRITE;
                }
            }
        }
    }
    free(offs);
    free(buf);
    if (rc != R_OK) {
        return rc;
    }
    /* clear the transaction stamp */
    uint8_t hdr[32];
    if (pread(dbf->fd, hdr, sizeof(hdr), 0) == (ssize_t)sizeof(hdr)) {
        hdr[30] = 0;
        if (pwrite(dbf->fd, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
            return R_ERR_IO_WRITE;
        }
        dbf->nrec = r32(hdr + 4);
    }
    if (dbf->recno > dbf->nrec + 1) {
        dbf->recno = dbf->nrec + 1;
        dbf->eof = 1;
    }
    dbf->dirty = 0;
    if (!dbf->eof && dbf->nrec) {
        rx_dbf_load_rec(dbf);
    }
    return R_OK;
}

int rx_rollback(rx_dbf* dbf) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (!dbf->in_txn) {
        return R_ERR_DBF_UNSUPPORTED; /* ROLLBACK without BEGIN (App. D surface) */
    }
    int rc = R_OK;
    if (dbf->rjn_fd >= 0) {
        rc = rollback_apply(dbf);
        close(dbf->rjn_fd);
        dbf->rjn_fd = -1;
        rx_unlink(dbf->rjn_path);
    }
    dbf->in_txn = 0;
    dbf->njrecs = 0;
    dbf->jhdr = 0;
    /* make sure the on-disk stamp is clear even if the sidecar was lost */
    uint8_t hdr[32];
    if (pread(dbf->fd, hdr, sizeof(hdr), 0) == (ssize_t)sizeof(hdr) && hdr[30] != 0) {
        hdr[30] = 0;
        pwrite(dbf->fd, hdr, sizeof(hdr), 0);
    }
    return rc;
}

int rx_commit(rx_dbf* dbf) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (!dbf->in_txn) {
        return R_ERR_DBF_UNSUPPORTED;
    }
    if (dbf->dirty) {
        int rc = rx_journal_rec(dbf, dbf->recno);
        if (rc != R_OK) {
            return rc;
        }
        rc = rx_dbf_write_rec(dbf);
        if (rc != R_OK) {
            return rc;
        }
    }
    dbf->in_txn = 0;
    int rc = rx_dbf_write_header(dbf); /* clears the stamp */
    if (rc != R_OK) {
        return rc;
    }
    if (dbf->rjn_fd >= 0) {
        char c = 'C';
        if (jwrite(dbf, &c, 1) != R_OK) {
            /* data is already committed; journal removal below is best effort */
        }
        close(dbf->rjn_fd);
        dbf->rjn_fd = -1;
        rx_unlink(dbf->rjn_path);
    }
    dbf->njrecs = 0;
    dbf->jhdr = 0;
    return R_OK;
}

int rx_journal_recover(rx_dbf* dbf) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    int fd = open(dbf->rjn_path, O_RDWR);
    if (fd < 0) {
        return R_OK; /* no sidecar: nothing to recover */
    }
    dbf->rjn_fd = fd;
    dbf->in_txn = 1;
    int rc = rollback_apply(dbf);
    close(fd);
    dbf->rjn_fd = -1;
    dbf->in_txn = 0;
    rx_unlink(dbf->rjn_path);
    return rc;
}
