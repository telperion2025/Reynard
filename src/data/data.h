/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file data.h
 * @brief Internal data-engine header. data/ includes nothing upward
 *        (spec §3.1) and returns exactly one Appendix D code per call.
 */
#ifndef REYNARD_DATA_H
#define REYNARD_DATA_H

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>

#include "reynard/dbf.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rx_dbf {
    int fd;
    int mode;
    int readonly;
    char path[260];
    char alias[11];
    uint8_t version;
    uint8_t lang_driver;
    int codepage; /**< resolved RX_CP*, 0 = no conversion */
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
};

/* codepage.c */
/** Map a DBF language-driver byte (spec §8.2) to an RX_CP* id; 0 = none. */
int rx_mark_to_cp(uint8_t mark);
/** True when the mark is known but has no conversion table yet. */
int rx_mark_known_no_table(uint8_t mark);

/* field.c */
/** Decode raw record bytes of one field into an rval (spec §8.2). */
int rx_field_decode(const rx_dbf *dbf, size_t fldidx, const uint8_t *raw, rval *out);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_DATA_H */
