/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file memvar.c
 * @brief Memory variable table (spec §7.5). M1 scope: a single flat scope;
 *        PRIVATE/PUBLIC hiding scopes and SAVE/RESTORE .MEM land at M5.
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

rx_memvar *rx_memvar_find(rx_ctx *ctx, const char *name)
{
    char up[64];
    norm_name(up, sizeof(up), name);
    for (size_t i = 0; i < ctx->nvars; i++) {
        if (strcmp(ctx->vars[i].name, up) == 0) {
            return &ctx->vars[i];
        }
    }
    return NULL;
}

int rx_memvar_set(rx_ctx *ctx, const char *name, const rval *v)
{
    rx_memvar *mv = rx_memvar_find(ctx, name);
    if (!mv) {
        if (ctx->nvars == ctx->capvars) {
            size_t nc = ctx->capvars ? ctx->capvars * 2 : 32;
            rx_memvar *nv = realloc(ctx->vars, nc * sizeof(rx_memvar));
            if (!nv) {
                RX_FATAL("out of memory");
            }
            ctx->vars = nv;
            ctx->capvars = nc;
        }
        mv = &ctx->vars[ctx->nvars++];
        memset(mv, 0, sizeof(*mv));
        norm_name(mv->name, sizeof(mv->name), name);
        rx_rval_undef(&mv->val);
    }
    rx_rval_free(&mv->val);
    /* deep copy so callers keep ownership of their value */
    mv->val = *v;
    if (v->t == RX_T_C || v->t == RX_T_M) {
        if (v->u.c.utf8) {
            mv->val.u.c.utf8 = rx_strdup(v->u.c.utf8);
            mv->val.u.c.n = v->u.c.n;
        } else {
            mv->val.u.c.utf8 = NULL;
            mv->val.u.c.n = 0;
        }
        if (v->t == RX_T_M) {
            mv->val.u.memo.loaded = NULL;
        }
    } else if (v->t == RX_T_G) {
        mv->val.u.g.bytes = NULL;
        mv->val.u.g.n = 0;
    }
    return R_OK;
}

void rx_memvar_clear(rx_ctx *ctx)
{
    for (size_t i = 0; i < ctx->nvars; i++) {
        rx_rval_free(&ctx->vars[i].val);
    }
    free(ctx->vars);
    ctx->vars = NULL;
    ctx->nvars = ctx->capvars = 0;
}
