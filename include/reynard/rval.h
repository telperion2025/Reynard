/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file rval.h
 * @brief The universal VM value: tagged union RVAL (spec §4).
 *
 * Character strings are UTF-8 internally and converted at DBF boundaries.
 * Empty-but-present vs undefined follows FoxPro 2.6a semantics (spec §7.6),
 * not VFP .NULL. semantics: logical has an explicit L_UNDEF state, and an
 * empty date is a valid "empty date" value distinct from undefined.
 */
#ifndef REYNARD_RVAL_H
#define REYNARD_RVAL_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Value type tags. */
typedef enum {
    RX_T_U = 0, /**< undefined (unknown variable / TYPE() "U") */
    RX_T_C,     /**< character: r_str */
    RX_T_N,     /**< numeric: r_num (honors SET DECIMALS at display) */
    RX_T_I,     /**< integer: int32_t */
    RX_T_Y,     /**< currency: int64_t scaled by 10000 */
    RX_T_B,     /**< binary float: double */
    RX_T_D,     /**< date: r_date, no time component */
    RX_T_T,     /**< datetime: r_dt, UTC internally */
    RX_T_L,     /**< logical: L_UNDEF / L_T / L_F */
    RX_T_M,     /**< memo: lazily loaded r_str */
    RX_T_G      /**< general/blob: raw bytes, read-only in v1.0 */
} rx_type;

/** UTF-8 string. n is the byte length excluding the terminating NUL. */
typedef struct r_str {
    size_t n;   /**< byte length */
    char *utf8; /**< NUL-terminated UTF-8, owned when inside an RVAL */
} r_str;

/** Numeric with display precision (SET DECIMALS). */
typedef struct r_num {
    double d;
    int dec;
} r_num;

/** Calendar date. All fields 0 denotes the empty date {}. */
typedef struct r_date {
    int16_t y, m, d;
} r_date;

/** Date-time: date plus milliseconds since midnight, UTC internally. */
typedef struct r_dt {
    r_date d;
    int32_t ms;
} r_dt;

/** Three-valued logic. DBF stores only T/F; U appears in expressions. */
typedef enum { L_UNDEF = 0, L_T = 1, L_F = 2 } rx_logic;

/** The universal value. Owned members: u.c.utf8 (C), u.memo (M), u.g.bytes (G). */
typedef struct rval {
    rx_type t;
    union {
        r_str c;                /**< RX_T_C */
        r_num n;                /**< RX_T_N */
        int32_t i;              /**< RX_T_I */
        int64_t y;              /**< RX_T_Y, scaled by 10000 */
        double b;               /**< RX_T_B */
        r_date d;               /**< RX_T_D */
        r_dt tm;                /**< RX_T_T */
        rx_logic l;             /**< RX_T_L */
        struct {
            uint32_t blk;       /**< FPT/DBT block number from the DBF */
            r_str *loaded;      /**< lazily loaded memo text, owned ref */
        } memo;                 /**< RX_T_M */
        struct {
            size_t n;
            uint8_t *bytes;     /**< owned */
        } g;                    /**< RX_T_G */
    } u;
} rval;

/** Set @p v to the undefined value. */
static inline void rx_rval_undef(rval *v)
{
    memset(v, 0, sizeof(*v));
    v->t = RX_T_U;
}

/** Free owned members of @p v and reset it to undefined. */
static inline void rx_rval_free(rval *v)
{
    if (v->t == RX_T_C || v->t == RX_T_M) {
        free(v->u.c.utf8);
        if (v->t == RX_T_M) {
            free(v->u.memo.loaded);
        }
    } else if (v->t == RX_T_G) {
        free(v->u.g.bytes);
    }
    rx_rval_undef(v);
}

/** Shallow-copy @p src into @p dst without transferring ownership. */
static inline void rx_rval_set_shallow(rval *dst, const rval *src)
{
    *dst = *src;
}

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_RVAL_H */
