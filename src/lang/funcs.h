/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file funcs.h
 * @brief Built-in function registry (spec Appendix A). Each funcs_*.c
 *        exports a name/handler table; vm.c owns lookup and dispatch.
 */
#ifndef REYNARD_FUNCS_H
#define REYNARD_FUNCS_H

#include "lang/lang.h"
#include "util/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*rx_fnh)(rx_ctx* ctx, size_t argc, const rval* argv, rval* out, r_err* err);

typedef struct rx_funcdef {
    const char* name;
    rx_fnh fn;
} rx_funcdef;

extern const rx_funcdef rx_funcs_str[];
extern const size_t rx_funcs_str_n;
extern const rx_funcdef rx_funcs_num[];
extern const size_t rx_funcs_num_n;
extern const rx_funcdef rx_funcs_date[];
extern const size_t rx_funcs_date_n;
extern const rx_funcdef rx_funcs_db[];
extern const size_t rx_funcs_db_n;
extern const rx_funcdef rx_funcs_sys[];
extern const size_t rx_funcs_sys_n;
extern const rx_funcdef rx_funcs_array[];
extern const size_t rx_funcs_array_n;

/* ---- coercion helpers shared by all funcs_*.c ---- */

/** Numeric coercion: N/I/Y/B -> double. Returns 0 on type mismatch. */
static inline int rv_to_num(const rval* v, double* d) {
    switch (v->t) {
    case RX_T_N:
        *d = v->u.n.d;
        return 1;
    case RX_T_I:
        *d = v->u.i;
        return 1;
    case RX_T_Y:
        *d = v->u.y / 10000.0;
        return 1;
    case RX_T_B:
        *d = v->u.b;
        return 1;
    default:
        return 0;
    }
}

static inline int rv_to_logic(const rval* v, rx_logic* l) {
    if (v->t != RX_T_L) {
        return 0;
    }
    *l = v->u.l;
    return 1;
}

static inline int rv_to_str(const rval* v, const char** s, size_t* n) {
    if (v->t == RX_T_C) {
        *s = v->u.c.utf8 ? v->u.c.utf8 : "";
        *n = v->u.c.n;
        return 1;
    }
    if (v->t == RX_T_M && v->u.memo.loaded) {
        *s = v->u.memo.loaded->utf8;
        *n = v->u.memo.loaded->n;
        return 1;
    }
    return 0;
}

static inline void f_num(rx_ctx* ctx, rval* out, double d) {
    out->t = RX_T_N;
    out->u.n.d = d;
    out->u.n.dec = ctx ? ctx->set.decimals : 2;
}

static inline void f_int(rval* out, int32_t i) {
    out->t = RX_T_I;
    out->u.i = i;
}

static inline void f_logic(rval* out, rx_logic l) {
    out->t = RX_T_L;
    out->u.l = l;
}

static inline void f_date(rval* out, r_date d) {
    out->t = RX_T_D;
    out->u.d = d;
}

/** out becomes a C string value; takes ownership of utf8. */
static inline void f_str(rval* out, char* utf8, size_t n) {
    rval_set_str(out, utf8, n);
}

static inline int f_argc(r_err* err, size_t argc, size_t want, const char* name) {
    if (argc != want) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() expects %zu argument(s)", name, want);
        return -1;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_FUNCS_H */
