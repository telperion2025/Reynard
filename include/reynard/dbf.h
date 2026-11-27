/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file dbf.h
 * @brief Data engine public API (spec §8.1; frozen at milestone M6).
 *
 * Every fallible function returns an Appendix D code (status.h); out
 * parameters carry values. No hidden globals except the documented
 * work-area table.
 */
#ifndef REYNARD_DBF_H
#define REYNARD_DBF_H

#include <stddef.h>
#include <stdint.h>

#include "reynard/rval.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rx_dbf rx_dbf;
typedef struct rx_idx rx_idx;
struct strbuf;

/* open modes */
#define RD_READONLY 0x1
#define RD_SHARED 0x2
#define RD_EXCLUSIVE 0x4

/** Field descriptor for rx_dbf_create (spec §8.1). */
typedef struct rx_fielddef {
    char name[11]; /**< 10 chars, uppercased, [A-Z0-9_], leading letter or _ */
    uint8_t type;  /**< RFT_C ... RFT_Y per spec §8.2 */
    uint8_t len;
    uint8_t dec;
    uint32_t flags;
} rx_fielddef;

/* Field type codes (spec §8.2). */
#define RFT_C 'C'
#define RFT_N 'N'
#define RFT_F 'F'
#define RFT_Y 'Y'
#define RFT_I 'I'
#define RFT_D 'D'
#define RFT_T 'T'
#define RFT_L 'L'
#define RFT_M 'M'
#define RFT_G 'G'
#define RFT_B 'B'
#define RFT_P 'P'

/** Open a DBF. mode is a bitmask of RD_*. */
int rx_dbf_open(const char* path, int mode, rx_dbf** out);
/** Create a new DBF with the given field layout. */
int rx_dbf_create(const char* path, const rx_fielddef* fields, size_t n, rx_dbf** out);
/** Close and free. */
int rx_dbf_close(rx_dbf* dbf);
/** Reopen in a new mode, e.g. downgrade EXCLUSIVE to SHARED. */
int rx_dbf_reopen(rx_dbf* dbf, int newmode);
/** True when opened RD_EXCLUSIVE (writes need no fcntl locks, spec \u00a78.5). */
int rx_dbf_is_exclusive(const rx_dbf* dbf);
/** True when opened RD_READONLY (NOUPDATE). */
int rx_dbf_is_readonly(const rx_dbf* dbf);
/** Raw header version byte and code page mark (for COPY STRUCTURE fidelity). */
int rx_dbf_version_mark(const rx_dbf* dbf, uint8_t* version, uint8_t* mark);
/** FOUND() state of the last LOCATE/CONTINUE (SEEK at M6). */
int rx_dbf_found(const rx_dbf* dbf);
/** Set the FOUND() flag (LOCATE/CONTINUE land here). */
void rx_dbf_set_found(rx_dbf* dbf, int found);
/** Header last-update date as civil y/m/d. */
int rx_dbf_lupdate(const rx_dbf* dbf, int* y, int* m, int* d);
/** RX_CP* id -> FoxPro code page mark byte (header byte 29); 0 = none. */
uint8_t rx_cp_to_mark(int cp);

/* Positioning & values */
int rx_dbf_goto(rx_dbf* dbf, uint32_t recno);
int rx_dbf_skip(rx_dbf* dbf, int32_t delta);
int rx_dbf_bof(const rx_dbf* dbf, int* out);
int rx_dbf_eof(const rx_dbf* dbf, int* out);
int rx_dbf_recount(const rx_dbf* dbf, uint32_t* out);
/** Current 1-based record number; 0 when past the end. */
int rx_dbf_recno(const rx_dbf* dbf, uint32_t* out);
/** Read field fldidx of the current record into out (memo loads on demand). */
int rx_dbf_get(rx_dbf* dbf, size_t fldidx, rval* out);
/** Write field fldidx of the current record (buffered until rx_flush). */
int rx_dbf_put(rx_dbf* dbf, size_t fldidx, const rval* in);
int rx_flush(rx_dbf* dbf);

/* Header introspection */
int rx_dbf_field_count(const rx_dbf* dbf, size_t* out);
int rx_dbf_field_def(const rx_dbf* dbf, size_t fldidx, rx_fielddef* out);
int rx_dbf_field_index(const rx_dbf* dbf, const char* name, size_t* out);
int rx_dbf_alias(const rx_dbf* dbf, const char** out);
int rx_dbf_set_alias(rx_dbf* dbf, const char* alias);
int rx_dbf_deleted(const rx_dbf* dbf, int* out);
int rx_dbf_path(const rx_dbf* dbf, const char** out);
int rx_dbf_codepage(const rx_dbf* dbf, int* out);

/* Locks */
int rx_rlock(rx_dbf* dbf, uint32_t recno);
int rx_flock(rx_dbf* dbf);
int rx_funlock(rx_dbf* dbf);                 /* release the file lock only */
void rx_unlock(rx_dbf* dbf, uint32_t recno); /* recno 0 = all locks held */
int rx_rlock_query(const rx_dbf* dbf, uint32_t recno);

/* ---- M2 write operations (spec §8.2/§8.5) ---- */
/** Append a blank record; *recno receives the new 1-based record number. */
int rx_dbf_append(rx_dbf* dbf, uint32_t* recno);
/** Stage raw record bytes into the current record (buffered until flush). */
int rx_dbf_put_raw(rx_dbf* dbf, const uint8_t* raw);
/** Read a raw record by number (round-trip suite). */
int rx_dbf_read_raw(const rx_dbf* dbf, uint32_t recno, uint8_t* buf);
/** Set/clear the delete flag of the current record (written immediately). */
int rx_dbf_set_deleted(rx_dbf* dbf, int del);
/** Physically remove deleted records and compact the memo file (exclusive). */
int rx_dbf_pack(rx_dbf* dbf);
/** Remove all records (exclusive). */
int rx_dbf_zap(rx_dbf* dbf);
/** Current record buffer/size accessors for import/export serializers. */
const uint8_t* rx_dbf_recbuf(const rx_dbf* dbf);
int rx_dbf_recsize(const rx_dbf* dbf, uint16_t* out);
/** Like rx_dbf_recbuf, but NULL unless the buffer provably holds the current
    record (no disk I/O on the hot LIST path, M10 spec 11.3; added after the
    M6 API freeze as a pure read-only addition). */
const uint8_t* rx_dbf_cur_recbuf(const rx_dbf* dbf, size_t* reclen);
/** Full-control create with explicit version/mark/date (COPY fidelity). */
int rx_dbf_create_full(const char* path, const rx_fielddef* fields, size_t n, uint8_t version,
                       uint8_t cp_mark, int yy, int mm, int dd, rx_dbf** out);
/** SET REPROCESS retry count (n, <0 = AUTOMATIC default policy). */
void rx_data_set_reprocess(int n);

/* ---- scopes (spec §8.7) ---- */
typedef struct rx_scope {
    int kind; /**< 0 none, 1 ALL, 2 REST, 3 NEXT, 4 RECORD */
    uint32_t n;
} rx_scope;
/** Resolve a scope to an inclusive record range; *restore set for RECORD n. */
int rx_scope_range(const rx_dbf* dbf, const rx_scope* sc, uint32_t* start, uint32_t* end,
                   int* restore);

/* ---- SDF / DELIMITED serializers (spec §8.8) ---- */
int rx_sdf_write_line(const rx_dbf* dbf, const size_t* flds, size_t nf, const uint8_t* rec,
                      char* out, size_t outsz, size_t* outn);
int rx_sdf_parse_line(const rx_dbf* dbf, const size_t* flds, size_t nf, const char* line, size_t n,
                      uint8_t* rec);
int rx_delim_write_line(const rx_dbf* dbf, const size_t* flds, size_t nf, const uint8_t* rec,
                        char delim, int quote, char* out, size_t outsz, size_t* outn);
int rx_delim_parse_line(const rx_dbf* dbf, const size_t* flds, size_t nf, const char* line,
                        size_t n, char delim, int quote, uint8_t* rec);

/* Transactions */
int rx_begin(rx_dbf* dbf);
int rx_commit(rx_dbf* dbf);
int rx_rollback(rx_dbf* dbf);

/* ---- Indexes (spec §8.4, finalized at M6) ----
 * Attached index files are kept on the dbf; the first is the structural CDX
 * (same stem as the DBF, auto-opened when present). One (file, tag) pair is
 * the controlling order; (-1,-1) means natural (physical) order. Keys are
 * rvals encoded to a canonical sortable form by the data layer; expression
 * evaluation is the caller's (lang layer) job. */

/** Open an index file (.cdx compound or .idx standalone) and attach it. */
int rx_index_attach(rx_dbf* dbf, const char* path, rx_idx** out);
/** Close and free all attached index files (called from rx_dbf_close). */
int rx_index_close_all(rx_dbf* dbf);
/** Close all attached index files except the structural CDX (SET INDEX TO). */
int rx_index_close_nonsstructural(rx_dbf* dbf);
/** Flush dirty index files to disk (called from rx_dbf_close / FLUSH). */
int rx_index_flush(rx_dbf* dbf);

/** Number of attached index files. */
int rx_index_file_count(const rx_dbf* dbf);
/** Path of attached index file fi. */
int rx_index_file_path(const rx_dbf* dbf, int fi, const char** out);
/** Nonzero when file fi is a compound .cdx (vs standalone .idx / .ntx). */
int rx_index_file_is_cdx(const rx_dbf* dbf, int fi);

/** Tag count of index file fi. */
int rx_tag_count(const rx_dbf* dbf, int fi);
#define RX_TAG_DESC 1   /**< descending flag */
#define RX_TAG_UNIQUE 2 /**< unique flag */
/** Tag name / key expression text / flags of tag ti in file fi. */
int rx_tag_info(const rx_dbf* dbf, int fi, int ti, const char** name, const char** key, int* flags);
/** FOR filter expression text of tag ti in file fi ("" when none). */
int rx_tag_for_expr(const rx_dbf* dbf, int fi, int ti, const char** for_expr);

/** Set the controlling order to (fi, ti); (-1,-1) restores natural order. */
int rx_index_order_set(rx_dbf* dbf, int fi, int ti);
/** Read the controlling order. */
int rx_index_order_get(const rx_dbf* dbf, int* fi, int* ti);

/** Create a (possibly empty) tag; cdx_path NULL = structural CDX. The key
 *  and FOR expressions are stored as text; the caller fills entries. */
int rx_index_create(rx_dbf* dbf, const char* tag, const char* cdx_path, const char* expr,
                    const char* for_expr, int flags, int additive);
/** Drop all entries of tag (fi, ti). */
int rx_index_clear(rx_dbf* dbf, int fi, int ti);
/** Insert one (key, recno) entry into a tag (sorted insert; UNIQUE drops
 *  exact duplicates). */
int rx_index_insert(rx_dbf* dbf, int fi, int ti, const rval* key, uint32_t recno);
/** Remove every entry with this recno from every tag of every file. */
int rx_index_delete_rec(rx_dbf* dbf, uint32_t recno);
/** Seek in tag (fi, ti): exact=1 requires equality; otherwise first key >=
 *  the probe (FoxPro SEEK with SET EXACT OFF). *found set accordingly. */
int rx_index_seek_tag(rx_dbf* dbf, int fi, int ti, const rval* key, int exact, uint32_t* recno_out,
                      int* found);
/** Seek in the controlling order. */
int rx_index_seek(rx_dbf* dbf, const rval* key, int exact, uint32_t* recno_out, int* found);
/** Number of entries in tag (fi, ti). */
int rx_index_key_count(const rx_dbf* dbf, int fi, int ti, uint32_t* out);
/** recno at 0-based ordered position pos in the controlling order. */
int rx_index_recno_at(const rx_dbf* dbf, uint32_t pos, uint32_t* recno_out);
/** 0-based ordered position of recno in the controlling order, -1 if absent. */
int rx_index_pos_of(const rx_dbf* dbf, uint32_t recno, int64_t* pos_out);
/** Encode an rval to the canonical sortable key form (internal, cdx.c). */
int rx_key_encode(const rval* v, struct strbuf* out);

/* Work areas (mirror SELECT()/USED()/ALIAS()); area 1..255, 0 = current */
int rx_wa_select(int area);
int rx_wa_alias(int area, const char** out);
int rx_wa_used(int area);
int rx_wa_current(void);
int rx_wa_open(int area, const char* path, int mode, const char* alias);
int rx_wa_close(int area);
int rx_wa_dbf(int area, rx_dbf** out);

/* Session configuration (called once from main; the work-area table is the
   only other module state). test_mode freezes the header last-update date
   at 2000-01-01 for deterministic output; cp_mark is the default code page
   mark (header byte 29) written by rx_dbf_create. */
void rx_data_session(int test_mode, int cp_mark);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_DBF_H */
