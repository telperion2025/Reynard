/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file rpp.h
 * @brief .RPP application container (spec 9.6, M9): a ZIP archive with
 *        STORED (uncompressed) entries only, written byte-deterministically
 *        (fixed DOS date 1980-01-01, no extra fields, entries in add order)
 *        so builds are reproducible under -t. Layout:
 *          manifest.json        project manifest (name, main, file table)
 *          src/<base>           raw program/menu source (.prg/.mpr)
 *          bin/<stem>.rxp       compiled RXP1 blob (sha256 of source as hash)
 *          res/<base>           raw screen/report/label (.scr/.frm/.rlb)
 *          data/<base>          raw .dbf tables
 *          data/dictionary.json data dictionary generated from the DBF headers
 *        The reader accepts stored entries only; compressed entries get a
 *        diagnostic (R_ERR_DBF_UNSUPPORTED).
 */
#ifndef REYNARD_RPP_H
#define REYNARD_RPP_H

#include <stddef.h>
#include <stdint.h>

#include "util/util.h"

#ifdef __cplusplus
extern "C" {
#endif

/** CRC-32 (IEEE, reflected, as used by ZIP). */
uint32_t rx_crc32(const void* data, size_t n);

/* ---------------- writer ---------------- */

typedef struct rx_zipw rx_zipw;

rx_zipw* rx_zipw_new(void);
/** Append one stored entry. name is truncated to 127 bytes. */
int rx_zipw_add(rx_zipw* w, const char* name, const void* data, size_t n);
/** Serialize to canonical bytes (owned buffer in *out). */
int rx_zipw_finish(rx_zipw* w, char** out, size_t* outn);
/** Number of entries added so far. */
size_t rx_zipw_count(const rx_zipw* w);
void rx_zipw_free(rx_zipw* w);

/* ---------------- reader ---------------- */

typedef struct rx_rpp_ent {
    char name[128];
    uint32_t crc;
    uint32_t size;
    uint32_t lho; /**< local header offset */
} rx_rpp_ent;

typedef struct rx_rpp {
    uint8_t* data; /**< whole file (owned) */
    size_t len;
    rx_rpp_ent* ents; /**< central directory (owned) */
    size_t nents;
} rx_rpp;

/** Open and index a .rpp file (stored entries only). */
int rx_rpp_open(const char* path, rx_rpp* out);
/** Index an in-memory image. */
int rx_rpp_open_mem(const void* data, size_t n, rx_rpp* out);
const rx_rpp_ent* rx_rpp_find(const rx_rpp* r, const char* name);
/** Read + CRC-verify one entry into an owned buffer. */
int rx_rpp_read(const rx_rpp* r, const rx_rpp_ent* e, uint8_t** out, size_t* outn);
void rx_rpp_close(rx_rpp* r);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_RPP_H */
