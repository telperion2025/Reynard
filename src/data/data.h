/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file data.h
 * @brief Internal data-engine header. data/ includes nothing upward
 *        (spec \u00a73.1) and returns exactly one Appendix D code per call.
 *        M2 adds the write path, memo files, locking, journaling, scopes
 *        and SDF/DELIMITED serializers (spec \u00a78.2-\u00a78.8).
 */
#ifndef REYNARD_DATA_H
#define REYNARD_DATA_H

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>

#include "reynard/dbf.h"
#include "reynard/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RX_MEMO_FPT 1
#define RX_MEMO_DBT 2

struct rx_dbf {
    int fd;
    int mode;
    int readonly;   /**< RD_READONLY: no writes at all */
    int exclusive;  /**< RD_EXCLUSIVE: no fcntl locks needed for writes */
    char path[260];
    char alias[11];
    uint8_t version;
    uint8_t lang_driver; /**< code page mark (header byte 29) */
    int codepage;        /**< resolved RX_CP*, 0 = no conversion */
    uint8_t hdr_yy, hdr_mm, hdr_dd; /**< last-update date from the header */
    uint32_t nrec;
    uint16_t header_size;
    uint16_t rec_size;
    size_t nfld;
    rx_fielddef *fields;
    size_t *foff;   /**< byte offset of each field within a record */
    uint32_t recno; /**< 1-based current record; nrec+1 when EOF */
    int bof;
    int eof;
    int deleted;    /**< delete flag of current record */
    uint8_t *recbuf;
    int dirty;      /**< recbuf holds unflushed changes */
    int found;      /**< FOUND() flag (LOCATE/CONTINUE; SEEK at M6) */
    /* memo file (lazy) */
    int has_memo;
    int memo_kind;  /**< RX_MEMO_FPT / RX_MEMO_DBT */
    int memo_fd;    /**< -1 until first memo access */
    int memo_ro;    /**< memo fd opened read-only; upgrade on first write */
    char memo_path[264];
    uint32_t memo_next;   /**< FPT next-free block / DBT next block (cached) */
    uint32_t memo_bs;     /**< FPT block size from its header */
    /* record/file locks held by this process (spec \u00a78.5) */
    uint32_t *locks;
    size_t nlocks, caplocks;
    int flock_held;
    /* transaction journal (spec \u00a78.6) */
    int in_txn;
    int rjn_fd;     /**< -1 when no journal open */
    char rjn_path[264];
    uint32_t *jrecs; /**< recnos already journaled this txn */
    size_t njrecs, capjrecs;
    int jhdr;        /**< header already journaled */
    /* indexes (spec \u00a78.4, M6) */
    struct rx_idx **idxs; /**< attached index files; [0] = structural CDX */
    size_t nidxs, capidxs;
    int order_file;       /**< idxs[] entry controlling order, -1 = natural */
    int order_tag;        /**< tag within that file, -1 = natural */
};

/* ---- session state (set once from main via rx_data_session) ---- */
/** test_mode freezes the header last-update date (2000-01-01) and
 *  cp_mark is the default code page mark for rx_dbf_create. */
void rx_data_session(int test_mode, int cp_mark);
/** Read back the session values (create + lock retry policy). */
int rx_data_default_mark(void);
int rx_data_test_mode(void);
int rx_data_reprocess(void);


/* ---- dbf.c ---- */
/** Write the 32-byte header (nrec + last-update date + txn stamp). */
int rx_dbf_write_header(rx_dbf *dbf);
/** Load the current record into recbuf (internal). */
int rx_dbf_load_rec(rx_dbf *dbf);
/** Write recbuf at the current record and clear dirty (internal). */
int rx_dbf_write_rec(rx_dbf *dbf);
/** Raw record access for the round-trip suite (internal). */
/* open the memo sidecar lazily; kind/path prepared at open/create */
int rx_memo_open(rx_dbf *dbf, int for_write);

/* ---- dbf_create.c ---- */

/* ---- codepage.c ---- */
/** Map a DBF language-driver byte (spec \u00a78.2) to an RX_CP* id; 0 = none. */
int rx_mark_to_cp(uint8_t mark);
/** True when the mark is known but has no conversion table yet. */
int rx_mark_known_no_table(uint8_t mark);

/* ---- field.c ---- */
/** Decode raw record bytes of one field into an rval (spec \u00a78.2). */
int rx_field_decode(const rx_dbf *dbf, size_t fldidx, const uint8_t *raw, rval *out);
/** Encode an rval into raw field bytes (inverse of decode). Memo (M)
 *  fields are handled by the caller via the memo files; encode writes
 *  the 10-digit block placeholder. Returns R_ERR_DBF_UNSUPPORTED for
 *  G/P puts (read-only in v1.0). */
int rx_field_encode(rx_dbf *dbf, size_t fldidx, const rval *in, uint8_t *raw);

/* ---- memo_fpt.c / memo_dbt.c ---- */
/** Read memo block blk into a freshly allocated r_str (UTF-8), or NULL
 *  for a blank memo. *out may stay NULL with R_OK for blk==0. */
int rx_memo_fpt_read(rx_dbf *dbf, uint32_t blk, r_str **out);
/** Write text as a memo, returning the block number in *blk_out. */
int rx_memo_fpt_write(rx_dbf *dbf, const char *data, size_t n, uint32_t old_blk,
                      uint32_t *blk_out);
int rx_memo_dbt_read(rx_dbf *dbf, uint32_t blk, r_str **out);
int rx_memo_dbt_write(rx_dbf *dbf, const char *data, size_t n, uint32_t old_blk,
                      uint32_t *blk_out);
/** Read a G/B/P payload block raw (read-only v1.0). */
int rx_memo_fpt_read_raw(rx_dbf *dbf, uint32_t blk, uint8_t **bytes, size_t *n);


/* ---- cdx.c (spec \u00a78.4, M6) ---- */
/** Attach the structural CDX (same stem as the DBF) if present (internal). */
int rx_index_attach_structural(rx_dbf *dbf);

/* ---- journal.c (spec \u00a78.6) ---- */
/** Journal a record before-image (idempotent per recno per txn). */
int rx_journal_rec(rx_dbf *dbf, uint32_t recno);
/** Journal the 32-byte header before-image. */
int rx_journal_header(rx_dbf *dbf);
/** Journal an append (recno is the new record). */
int rx_journal_append(rx_dbf *dbf, uint32_t recno);
/** Journal a memo before-image. */
int rx_journal_memo(rx_dbf *dbf, uint32_t blk, const uint8_t *old, size_t oldn,
                    long oldsize);
/** Recover a stamped header: roll back if the sidecar still exists. */
int rx_journal_recover(rx_dbf *dbf);


#ifdef __cplusplus
}
#endif

#endif /* REYNARD_DATA_H */
