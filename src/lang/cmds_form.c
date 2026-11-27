/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_form.c
 * @brief @ SAY / @ GET / READ / CLEAR GETS (spec §9.2, M7).
 *
 * @ r,c SAY e paints immediately (interactive: through the say_fn hook onto
 * the desktop; batch: the text goes to stdout, one line per SAY — see
 * DEVIATIONS #47). @ r,c GET var registers a pending GET object on
 * ctx->gets; READ runs the modal editor loop through ctx->read_fn (NULL in
 * batch -> R_ERR_NOUI 601, except READ with no pending GETs which is a
 * no-op, matching FoxPro).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lang/funcs.h"
#include "lang/lang.h"
#include "reynard/dbf.h"
#include "util/util.h"

static const char* clause_word(rx_stmt* s, const char* key) {
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, key) == 0) {
            return s->clauses[i].word;
        }
    }
    return NULL;
}

static rx_clause* clause_e(rx_stmt* s, const char* key) {
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, key) == 0) {
            return &s->clauses[i];
        }
    }
    return NULL;
}

static int has_clause(rx_stmt* s, const char* key) {
    return clause_word(s, key) != NULL || clause_e(s, key) != NULL;
}

/* free one GET object */
static void get_free(rx_get* g) {
    rx_rval_free(&g->val);
    rx_rval_free(&g->range_lo);
    rx_rval_free(&g->range_hi);
    rx_rval_free(&g->deflt);
    free(g->say);
    free(g->picture);
    free(g->function);
    rx_code_free(&g->when);
    rx_code_free(&g->valid);
}

static void ctx_clear_gets(rx_ctx* ctx) {
    for (size_t i = 0; i < ctx->ngets; i++) {
        get_free(&ctx->gets[i]);
    }
    ctx->ngets = 0;
}

/* resolve the GET variable: a field of the current work area wins when a
   table is open and the name is a field (FoxPro rule); else memvar */
static int get_is_field(rx_ctx* ctx, const char* name) {
    (void)ctx;
    rx_dbf* dbf;
    if (rx_wa_dbf(0, &dbf) != R_OK) {
        return 0;
    }
    size_t idx;
    return rx_dbf_field_index(dbf, name, &idx) == R_OK;
}

/* current value of the GET var (field value or memvar value) */
static int get_current(rx_ctx* ctx, rx_get* g, rval* out) {
    rx_rval_undef(out);
    if (g->is_field) {
        rx_dbf* dbf;
        if (rx_wa_dbf(0, &dbf) == R_OK) {
            size_t idx;
            if (rx_dbf_field_index(dbf, g->var, &idx) == R_OK) {
                return rx_dbf_get(dbf, idx, out);
            }
        }
        return R_ERR_FIELD;
    }
    rx_memvar* mv = rx_memvar_find(ctx, g->var);
    if (mv && !mv->is_array) {
        rx_rval_deep(out, &mv->val);
        return R_OK;
    }
    return R_ERR_VAR;
}

/* write the edited value back to the field/memvar */
int rx_form_store(rx_ctx* ctx, rx_get* g) {
    if (g->is_field) {
        rx_dbf* dbf;
        if (rx_wa_dbf(0, &dbf) != R_OK) {
            return R_ERR_DBF_NOTOPEN;
        }
        size_t idx;
        if (rx_dbf_field_index(dbf, g->var, &idx) != R_OK) {
            return R_ERR_FIELD;
        }
        int rc = rx_dbf_put(dbf, idx, &g->val);
        if (rc != R_OK) {
            return rc;
        }
        rc = rx_flush(dbf);
        if (rc != R_OK) {
            return rc;
        }
        uint32_t recno;
        rx_dbf_recno(dbf, &recno);
        return rx_index_maintain_rec(ctx, dbf, recno); /* M6 maintenance */
    }
    return rx_memvar_set(ctx, g->var, &g->val);
}

int rx_cmd_sayget(rx_ctx* ctx, rx_stmt* s) {
    if (s->nexpr < 2) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "@ requires row,col");
    }
    rval rv, cv;
    rx_rval_undef(&rv);
    rx_rval_undef(&cv);
    int rc = rx_eval(ctx, s->exprs[0], &rv, &ctx->err);
    if (rc == R_OK) {
        rc = rx_eval(ctx, s->exprs[1], &cv, &ctx->err);
    }
    if (rc != R_OK) {
        return rc;
    }
    double rd = 0, cd = 0;
    int ok1 = rv_to_num(&rv, &rd);
    int ok2 = rv_to_num(&cv, &cd);
    rx_rval_free(&rv);
    rx_rval_free(&cv);
    if (!ok1 || !ok2) {
        return rx_raise(ctx, R_ERR_TYPE, s->line, s->col, "@ row,col must be numeric");
    }
    int row = (int)rd, col = (int)cd;

    /* @ r,c CLEAR / @ r,c BOX */
    if (has_clause(s, "CLEAR")) {
        if (ctx->say_fn) {
            ctx->say_fn(ctx->say_ud, row, col, "\x01CLEAR"); /* control: clear region */
        }
        return R_OK;
    }
    if (has_clause(s, "BOX")) {
        if (ctx->say_fn) {
            ctx->say_fn(ctx->say_ud, row, col, "\x01BOX");
        }
        return R_OK;
    }

    rx_clause* sayc = clause_e(s, "SAY");
    const char* getname = clause_word(s, "GET");

    /* SAY text */
    char* saytext = NULL;
    if (sayc) {
        rval v;
        rx_rval_undef(&v);
        rc = rx_eval(ctx, sayc->e, &v, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        strbuf sb;
        sb_init(&sb);
        /* SAY-side PICTURE/FUNCTION (before the GET keyword) format the label */
        const char* pic = clause_word(s, "SAYPICTURE");
        const char* fn = clause_word(s, "SAYFUNCTION");
        if (pic) {
            rx_picture_render(ctx, &v, pic, fn, &sb);
        } else {
            rx_vm_display(ctx, &v, &sb);
        }
        rx_rval_free(&v);
        saytext = sb.buf ? sb.buf : rx_strdup("");
        sb.buf = NULL;
        sb_free(&sb);
        if (ctx->say_fn) {
            ctx->say_fn(ctx->say_ud, row, col, saytext); /* label shows with GET too */
        } else if (!getname) {
            /* batch: @ SAY prints the text as one line */
            rx_out(ctx, saytext, strlen(saytext));
            rx_out(ctx, "\n", 1);
        }
    }

    if (!getname) {
        free(saytext);
        return R_OK;
    }

    /* register the GET */
    if (ctx->ngets == ctx->capgets) {
        size_t nc = ctx->capgets ? ctx->capgets * 2 : 8;
        rx_get* ng = realloc(ctx->gets, nc * sizeof(rx_get));
        if (!ng) {
            free(saytext);
            RX_FATAL("out of memory");
        }
        ctx->gets = ng;
        ctx->capgets = nc;
    }
    rx_get* g = &ctx->gets[ctx->ngets++];
    memset(g, 0, sizeof(*g));
    snprintf(g->var, sizeof(g->var), "%.63s", getname);
    for (char* c = g->var; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    g->is_field = get_is_field(ctx, g->var);
    g->row = row;
    /* FoxPro: @ r,c SAY text GET var places the GET right after the SAY text */
    g->col = col + (saytext ? (int)strlen(saytext) : 0);
    g->size_h = 1;
    g->size_w = 10;
    g->say = saytext;
    g->say_row = row;
    g->say_col = col;
    const char* pic = clause_word(s, "PICTURE");
    const char* fn = clause_word(s, "FUNCTION");
    if (pic) {
        g->picture = rx_strdup(pic);
    }
    if (fn) {
        g->function = rx_strdup(fn);
    }
    /* SIZE h,w */
    rx_clause *sh = clause_e(s, "SIZEH"), *sw = clause_e(s, "SIZEW");
    if (sh && sw) {
        rval h, w;
        rx_rval_undef(&h);
        rx_rval_undef(&w);
        if (rx_eval(ctx, sh->e, &h, &ctx->err) == R_OK &&
            rx_eval(ctx, sw->e, &w, &ctx->err) == R_OK) {
            double hd, wd;
            if (rv_to_num(&h, &hd) && rv_to_num(&w, &wd)) {
                g->size_h = (int)hd;
                g->size_w = (int)wd;
            }
        }
        rx_rval_free(&h);
        rx_rval_free(&w);
    }
    /* DEFAULT / current value */
    rc = get_current(ctx, g, &g->val);
    if (rc != R_OK) {
        rx_clause* dc = clause_e(s, "DEFAULT");
        if (dc) {
            rx_rval_undef(&g->val);
            rc = rx_eval(ctx, dc->e, &g->val, &ctx->err);
            g->has_default = 1;
        } else {
            rx_rval_undef(&g->val); /* stays undefined until READ assigns */
        }
    }
    /* WHEN / VALID / RANGE compiled */
    rx_clause* wc = clause_e(s, "WHEN");
    if (wc) {
        g->has_when = 1;
        rc = rx_compile_expr(wc->e, &g->when, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
    }
    rx_clause* vc = clause_e(s, "VALID");
    if (vc) {
        g->has_valid = 1;
        rc = rx_compile_expr(vc->e, &g->valid, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
    }
    rx_clause *r1 = clause_e(s, "RANGE"), *r2 = clause_e(s, "RANGE2");
    if (r1) {
        g->has_range = 1;
        rx_rval_undef(&g->range_lo);
        rc = rx_eval(ctx, r1->e, &g->range_lo, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        if (r2) {
            rx_rval_undef(&g->range_hi);
            rc = rx_eval(ctx, r2->e, &g->range_hi, &ctx->err);
            if (rc != R_OK) {
                return rc;
            }
        }
    }
    g->enabled = 1;
    return R_OK;
}

int rx_cmd_read(rx_ctx* ctx, rx_stmt* s) {
    if (has_clause(s, "CLEARGETS")) {
        ctx_clear_gets(ctx);
        return R_OK;
    }
    if (has_clause(s, "CLEARREAD")) {
        ctx->read_clear = 1;
        ctx->read_active = 0;
        return R_OK;
    }
    ctx->read_cycle = has_clause(s, "CYCLE") ? 1 : 0;
    ctx->read_clear = 0;
    ctx->read_abort = 0;
    if (ctx->ngets == 0) {
        return R_OK; /* FoxPro: READ with no GETs is a no-op */
    }
    if (!ctx->read_fn) {
        return rx_raise(ctx, R_ERR_NOUI, s->line, s->col, "READ requires the interactive UI");
    }
    ctx->read_active = 1;
    int rc = ctx->read_fn(ctx->read_ud);
    ctx->read_active = 0;
    if (rc != R_OK) {
        return rc;
    }
    /* store edited values back */
    for (size_t i = 0; i < ctx->ngets; i++) {
        int src = rx_form_store(ctx, &ctx->gets[i]);
        if (src != R_OK) {
            return rx_raise(ctx, src, s->line, s->col, "Cannot store GET value");
        }
    }
    return R_OK;
}

/* called from rx_ctx_free */
void rx_form_free_ctx(rx_ctx* ctx) {
    ctx_clear_gets(ctx);
    free(ctx->gets);
    ctx->gets = NULL;
    ctx->ngets = ctx->capgets = 0;
}
