/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file memvar.c
 * @brief Memory variable table (spec §7.5). M5: lookup walks the procedure
 *        frame chain (PRIVATE hiding) then the global table; PUBLIC lands in
 *        the global table; arrays (DIMENSION/DECLARE) live here as memvars
 *        with element storage. SAVE/RESTORE .MEM lands later (deviation).
 */
#include "lang/lang.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util/util.h"

static void norm_name(char *up, size_t sz, const char *name)
{
    /* m.var and var are the same variable (spec §7.1) */
    if (strncasecmp(name, "m.", 2) == 0) {
        name += 2;
    }
    snprintf(up, sz, "%s", name);
    for (char *s = up; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
}

static rx_memvar *find_in(rx_memvar *vars, size_t n, const char *up)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(vars[i].name, up) == 0) {
            return &vars[i];
        }
    }
    return NULL;
}

rx_memvar *rx_memvar_lookup(rx_ctx *ctx, const char *name)
{
    char up[64];
    norm_name(up, sizeof(up), name);
    for (rx_frame *f = ctx->frame; f; f = f->up) {
        rx_memvar *mv = find_in(f->vars, f->nvars, up);
        if (mv) {
            return mv;
        }
    }
    return find_in(ctx->vars, ctx->nvars, up);
}

rx_memvar *rx_memvar_find(rx_ctx *ctx, const char *name)
{
    return rx_memvar_lookup(ctx, name);
}

static void free_memvar(rx_memvar *mv)
{
    rx_rval_free(&mv->val);
    if (mv->is_array) {
        for (size_t i = 0; i < mv->anelem; i++) {
            rx_rval_free(&mv->aelem[i]);
        }
        free(mv->aelem);
        mv->aelem = NULL;
        mv->anelem = 0;
        mv->is_array = 0;
    }
}

/* deep copy of a value (strings/memos duplicated) */
static void rval_deep_impl(rval *dst, const rval *src);
void rx_rval_deep(rval *dst, const rval *src)
{
    rval_deep_impl(dst, src);
}
static void rval_deep_impl(rval *dst, const rval *src)
{
    *dst = *src;
    if (src->t == RX_T_C) {
        dst->u.c.utf8 = src->u.c.utf8 ? rx_strdup(src->u.c.utf8) : NULL;
        dst->u.c.n = src->u.c.n;
    } else if (src->t == RX_T_M) {
        dst->u.memo.blk = src->u.memo.blk;
        dst->u.memo.loaded = NULL;
        if (src->u.memo.loaded) {
            r_str *c = malloc(sizeof(r_str));
            if (!c) {
                RX_FATAL("out of memory");
            }
            c->n = src->u.memo.loaded->n;
            c->utf8 = rx_strdup(src->u.memo.loaded->utf8 ? src->u.memo.loaded->utf8 : "");
            dst->u.memo.loaded = c;
        }
    } else if (src->t == RX_T_G) {
        if (src->u.g.bytes && src->u.g.n) {
            dst->u.g.bytes = malloc(src->u.g.n);
            if (!dst->u.g.bytes) {
                RX_FATAL("out of memory");
            }
            memcpy(dst->u.g.bytes, src->u.g.bytes, src->u.g.n);
        } else {
            dst->u.g.bytes = NULL;
            dst->u.g.n = 0;
        }
    }
}

static rx_memvar *memvar_slot(rx_memvar **vars, size_t *n, size_t *cap, const char *name)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 32;
        rx_memvar *nv = realloc(*vars, nc * sizeof(rx_memvar));
        if (!nv) {
            RX_FATAL("out of memory");
        }
        *vars = nv;
        *cap = nc;
    }
    rx_memvar *mv = &(*vars)[(*n)++];
    memset(mv, 0, sizeof(*mv));
    norm_name(mv->name, sizeof(mv->name), name);
    rx_rval_undef(&mv->val);
    return mv;
}

/* new variables land in the current frame inside a procedure, else globals */
static rx_memvar *memvar_new(rx_ctx *ctx, const char *name)
{
    if (ctx->frame) {
        return memvar_slot(&ctx->frame->vars, &ctx->frame->nvars, &ctx->frame->capvars, name);
    }
    return memvar_slot(&ctx->vars, &ctx->nvars, &ctx->capvars, name);
}

int rx_memvar_set(rx_ctx *ctx, const char *name, const rval *v)
{
    rx_memvar *mv = rx_memvar_lookup(ctx, name);
    if (!mv) {
        mv = memvar_new(ctx, name);
    }
    free_memvar(mv);
    rx_rval_deep(&mv->val, v);
    return R_OK;
}

/** PUBLIC: force-create in the global table (or update it there). */
int rx_memvar_set_public(rx_ctx *ctx, const char *name, const rval *v)
{
    char up[64];
    norm_name(up, sizeof(up), name);
    rx_memvar *mv = find_in(ctx->vars, ctx->nvars, up);
    if (!mv) {
        mv = memvar_slot(&ctx->vars, &ctx->nvars, &ctx->capvars, name);
    }
    free_memvar(mv);
    rx_rval_deep(&mv->val, v);
    return R_OK;
}

/** PRIVATE: create in the current frame, hiding any outer variable. */
int rx_memvar_private(rx_ctx *ctx, const char *name)
{
    if (ctx->frame) {
        char up[64];
        norm_name(up, sizeof(up), name);
        if (find_in(ctx->frame->vars, ctx->frame->nvars, up)) {
            return R_OK;
        }
        rx_memvar *mv = memvar_slot(&ctx->frame->vars, &ctx->frame->nvars, &ctx->frame->capvars,
                                    name);
        rx_rval_undef(&mv->val);
        return R_OK;
    }
    /* top level: PRIVATE behaves like a normal (global) declaration in v1 */
    if (!rx_memvar_lookup(ctx, name)) {
        memvar_slot(&ctx->vars, &ctx->nvars, &ctx->capvars, name);
    }
    return R_OK;
}

/** RELEASE one memvar (any scope); returns 1 if removed. */
int rx_memvar_release(rx_ctx *ctx, const char *name)
{
    char up[64];
    norm_name(up, sizeof(up), name);
    for (rx_frame *f = ctx->frame; f; f = f->up) {
        for (size_t i = 0; i < f->nvars; i++) {
            if (strcmp(f->vars[i].name, up) == 0) {
                free_memvar(&f->vars[i]);
                memmove(&f->vars[i], &f->vars[i + 1], (f->nvars - i - 1) * sizeof(rx_memvar));
                f->nvars--;
                return 1;
            }
        }
    }
    for (size_t i = 0; i < ctx->nvars; i++) {
        if (strcmp(ctx->vars[i].name, up) == 0) {
            free_memvar(&ctx->vars[i]);
            memmove(&ctx->vars[i], &ctx->vars[i + 1], (ctx->nvars - i - 1) * sizeof(rx_memvar));
            ctx->nvars--;
            return 1;
        }
    }
    return 0;
}

void rx_memvar_clear(rx_ctx *ctx)
{
    for (size_t i = 0; i < ctx->nvars; i++) {
        free_memvar(&ctx->vars[i]);
    }
    free(ctx->vars);
    ctx->vars = NULL;
    ctx->nvars = ctx->capvars = 0;
}

/* ---- arrays (M5) ---- */

int rx_array_def(rx_ctx *ctx, const char *name, size_t d1, size_t d2)
{
    if (d1 == 0) {
        d1 = 1;
    }
    rx_memvar *mv = rx_memvar_lookup(ctx, name);
    if (mv && !mv->is_array) {
        free_memvar(mv);
        rx_rval_undef(&mv->val);
    }
    if (!mv) {
        if (ctx->frame) {
            mv = memvar_slot(&ctx->frame->vars, &ctx->frame->nvars, &ctx->frame->capvars, name);
        } else {
            mv = memvar_slot(&ctx->vars, &ctx->nvars, &ctx->capvars, name);
        }
    }
    if (mv->is_array) {
        for (size_t i = 0; i < mv->anelem; i++) {
            rx_rval_free(&mv->aelem[i]);
        }
        free(mv->aelem);
    }
    mv->is_array = 1;
    mv->adim[0] = d1;
    mv->adim[1] = d2;
    mv->anelem = d1 * (d2 ? d2 : 1);
    mv->aelem = calloc(mv->anelem ? mv->anelem : 1, sizeof(rval));
    if (!mv->aelem) {
        RX_FATAL("out of memory");
    }
    for (size_t i = 0; i < mv->anelem; i++) {
        rx_rval_undef(&mv->aelem[i]);
    }
    rx_rval_free(&mv->val);
    rx_rval_undef(&mv->val);
    return R_OK;
}

int rx_array_dims(rx_ctx *ctx, const char *name, size_t *d1, size_t *d2)
{
    rx_memvar *mv = rx_memvar_lookup(ctx, name);
    if (!mv || !mv->is_array) {
        *d1 = *d2 = 0;
        return R_ERR_TYPE;
    }
    *d1 = mv->adim[0];
    *d2 = mv->adim[1];
    return R_OK;
}

static size_t array_off(rx_memvar *mv, size_t i, size_t j)
{
    size_t cols = mv->adim[1] ? mv->adim[1] : 1;
    return (i - 1) * cols + (j - 1);
}

int rx_array_get(rx_ctx *ctx, const char *name, size_t i, size_t j, rval *out)
{
    rx_memvar *mv = rx_memvar_lookup(ctx, name);
    if (!mv || !mv->is_array) {
        return R_ERR_TYPE;
    }
    if (i < 1 || i > mv->adim[0] || (mv->adim[1] && (j < 1 || j > mv->adim[1]))) {
        return R_ERR_RANGE;
    }
    rx_rval_deep(out, &mv->aelem[array_off(mv, i, j)]);
    return R_OK;
}

int rx_array_set(rx_ctx *ctx, const char *name, size_t i, size_t j, const rval *v)
{
    rx_memvar *mv = rx_memvar_lookup(ctx, name);
    if (!mv || !mv->is_array) {
        return R_ERR_TYPE;
    }
    if (i < 1 || i > mv->adim[0] || (mv->adim[1] && (j < 1 || j > mv->adim[1]))) {
        return R_ERR_RANGE;
    }
    rval *slot = &mv->aelem[array_off(mv, i, j)];
    rx_rval_free(slot);
    rx_rval_deep(slot, v);
    return R_OK;
}
