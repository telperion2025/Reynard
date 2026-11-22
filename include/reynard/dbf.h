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
typedef struct rx_cursor rx_cursor;
typedef struct rx_idx rx_idx;

/* open modes */
#define RD_READONLY 0x1
#define RD_SHARED 0x2
#define RD_EXCLUSIVE 0x4

/** Field descriptor for rx_dbf_create (spec §8.1). */
typedef struct rx_fielddef {
    char name[11];  /**< 10 chars, uppercased, [A-Z0-9_], leading letter or _ */
    uint8_t type;   /**< RFT_C ... RFT_Y per spec §8.2 */
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
int rx_dbf_open(const char *path, int mode, rx_dbf **out);
/** Create a new DBF with the given field layout. */
int rx_dbf_create(const char *path, const rx_fielddef *fields, size_t n, rx_dbf **out);
/** Close and free. */
int rx_dbf_close(rx_dbf *dbf);
/** Reopen in a new mode, e.g. downgrade EXCLUSIVE to SHARED. */
int rx_dbf_reopen(rx_dbf *dbf, int newmode);

/* Positioning & values */
int rx_dbf_goto(rx_dbf *dbf, uint32_t recno);
int rx_dbf_skip(rx_dbf *dbf, int32_t delta);
int rx_dbf_bof(const rx_dbf *dbf, int *out);
int rx_dbf_eof(const rx_dbf *dbf, int *out);
int rx_dbf_recount(const rx_dbf *dbf, uint32_t *out);
/** Current 1-based record number; 0 when past the end. */
int rx_dbf_recno(const rx_dbf *dbf, uint32_t *out);
/** Read field fldidx of the current record into out (memo loads on demand). */
int rx_dbf_get(rx_dbf *dbf, size_t fldidx, rval *out);
/** Write field fldidx of the current record (buffered until rx_flush). */
int rx_dbf_put(rx_dbf *dbf, size_t fldidx, const rval *in);
int rx_flush(rx_dbf *dbf);

/* Header introspection */
int rx_dbf_field_count(const rx_dbf *dbf, size_t *out);
int rx_dbf_field_def(const rx_dbf *dbf, size_t fldidx, rx_fielddef *out);
int rx_dbf_field_index(const rx_dbf *dbf, const char *name, size_t *out);
int rx_dbf_alias(const rx_dbf *dbf, const char **out);
int rx_dbf_set_alias(rx_dbf *dbf, const char *alias);
int rx_dbf_deleted(const rx_dbf *dbf, int *out);
int rx_dbf_path(const rx_dbf *dbf, const char **out);
int rx_dbf_codepage(const rx_dbf *dbf, int *out);

/* Locks */
int rx_rlock(rx_dbf *dbf, uint32_t recno);
int rx_flock(rx_dbf *dbf);
void rx_unlock(rx_dbf *dbf, uint32_t recno); /* recno 0 = all locks held */
int rx_rlock_query(const rx_dbf *dbf, uint32_t recno);

/* Transactions */
int rx_begin(rx_dbf *dbf);
int rx_commit(rx_dbf *dbf);
int rx_rollback(rx_dbf *dbf);

/* Indexes */
int rx_index_attach(rx_dbf *dbf, const char *cdxpath, rx_idx **out);
int rx_index_order(rx_dbf *dbf, const char *tag);
int rx_seek(rx_cursor *cur, rval *key, int *found);
int rx_index_create(rx_dbf *dbf, const char *tag, const char *expr,
                    int descending, int unique);

/* Work areas (mirror SELECT()/USED()/ALIAS()); area 1..255, 0 = current */
int rx_wa_select(int area);
int rx_wa_alias(int area, const char **out);
int rx_wa_used(int area);
int rx_wa_current(void);
int rx_wa_open(int area, const char *path, int mode, const char *alias);
int rx_wa_close(int area);
int rx_wa_dbf(int area, rx_dbf **out);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_DBF_H */
