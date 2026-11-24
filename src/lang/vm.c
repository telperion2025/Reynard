/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file vm.c
 * @brief The VM (spec §7.4): runs compiled bytecode on a typed stack
 *        (depth 128), dispatches statements to the cmds_*.c handlers, and
 *        owns the built-in function registry (Appendix A).
 */
/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "lang/lang.h"
#include "lang/funcs.h"
#include "reynard/dbf.h"
#include "util/util.h"

#define VM_STACK 128

void rx_frame_free(rx_ctx *ctx, rx_frame *f);

void rx_ctx_init(rx_ctx *ctx, FILE *out, int test_mode)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->out = out;
    ctx->test_mode = test_mode;
    ctx->rng = 1; /* session seed 1 in -t mode (spec §3.3) */
    ctx->set.talk = 1;
    ctx->set.headings = 1;
    ctx->set.exact = 0;
    ctx->set.fixed = 0;
    ctx->set.century = 0;
    ctx->set.deleted = 0;
    ctx->set.escape = 1;
    ctx->set.console = 1;
    ctx->set.safety = 1;
    ctx->set.clock = 0;
    ctx->set.mouse = 1;
    ctx->set.decimals = 2;
    ctx->set.date_fmt = 0;
    ctx->set.exclusive = 1; /* FoxPro default: SET EXCLUSIVE ON */
    ctx->set.reprocess = 0;
    ctx->set.multilocks = 0;
    ctx->set.unique = 0;    /* SET UNIQUE OFF (M6) */
    char cwd[260];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(ctx->set.defdir, sizeof(ctx->set.defdir), "%s", cwd);
    }
}

void rx_ctx_free(rx_ctx *ctx)
{
    if (ctx->filter_on || ctx->filter.ops) {
        rx_code_free(&ctx->filter);
    }
    for (size_t i = 0; i < ctx->nrels; i++) {
        rx_expr_free(ctx->rels[i].e);
        free(ctx->rels[i].text);
    }
    free(ctx->rels);
    for (size_t i = 0; i < ctx->nidxexprs; i++) {
        free(ctx->idxexprs[i].keytext);
        free(ctx->idxexprs[i].fortext);
        rx_expr_free(ctx->idxexprs[i].key_e);
        rx_expr_free(ctx->idxexprs[i].for_e);
    }
    free(ctx->idxexprs);
    free(ctx->filter_text);
    for (size_t i = 0; i < ctx->nprocs; i++) {
        for (size_t j = 0; j < ctx->procs[i].nbody; j++) {
            rx_stmt_free(ctx->procs[i].body[j]);
            free(ctx->procs[i].body[j]);
        }
        free(ctx->procs[i].body);
    }
    free(ctx->procs);
    ctx->procs = NULL;
    ctx->nprocs = ctx->capprocs = 0;
    rx_rval_free(&ctx->retval);
    while (ctx->frame) {
        rx_frame *up = ctx->frame->up;
        rx_frame_free(ctx, ctx->frame);
        free(ctx->frame);
        ctx->frame = up;
    }
    /* locate_expr is borrowed from the statement list (owned by the caller) */
    rx_memvar_clear(ctx);
}

int rx_out(rx_ctx *ctx, const char *s, size_t n)
{
    if (!ctx->set.console) {
        return R_OK;
    }
    if (ctx->out_fn) {
        return ctx->out_fn(ctx->out_ud, s, n);
    }
    if (!ctx->out) {
        return R_OK;
    }
    if (fwrite(s, 1, n, ctx->out) != n) {
        return R_ERR_IO_WRITE;
    }
    return R_OK;
}

int rx_outs(rx_ctx *ctx, const char *s)
{
    return rx_out(ctx, s, strlen(s));
}

int rx_outf(rx_ctx *ctx, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return R_ERR_IO_WRITE;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    return rx_out(ctx, buf, (size_t)n);
}

/* run one source buffer; returns exit-code contribution (0 or error code) */
int rx_run_source(rx_ctx *ctx, const char *src, const char *name)
{
    rx_tok *toks;
    size_t ntok;
    r_err err;
    memset(&err, 0, sizeof(err));
    snprintf(ctx->prog, sizeof(ctx->prog), "%s", name);
    /* M5: & macros expand at execution time per statement (raw spans), not
       upfront — &var must see memvars assigned earlier in the same program */
    ctx->src = src;
    if (rx_lex(src, name, &toks, &ntok, &err) != R_OK) {
        rx_raise(ctx, err.code, err.line, err.col, "%s", err.msg);
        return err.code;
    }
    rx_stmt *stmts;
    size_t nstmt;
    if (rx_parse_src(toks, ntok, src, &stmts, &nstmt, &err) != R_OK) {
        rx_raise(ctx, err.code, err.line, err.col, "%s", err.msg);
        rx_lex_free(toks, ntok);
        return err.code;
    }
    int rc = rx_exec(ctx, stmts, nstmt);
    for (size_t i = 0; i < nstmt; i++) {
        rx_stmt_free(&stmts[i]);
    }
    free(stmts);
    rx_lex_free(toks, ntok);
    ctx->src = NULL;
    if (rc != R_OK) {
        if (!ctx->err_printed) {
            char msg[512];
            snprintf(msg, sizeof(msg), "%s", ctx->err.msg[0] ? ctx->err.msg : rx_status_str(rc));
            rx_raise(ctx, rc, ctx->err.line ? ctx->err.line : err.line,
                     ctx->err.col ? ctx->err.col : err.col, "%s", msg);
        }
        return rc;
    }
    return R_OK;
}

int rx_eval_str(rx_ctx *ctx, const char *src, rval *out, r_err *err)
{
    rx_tok *toks;
    size_t ntok;
    memset(err, 0, sizeof(*err));
    /* M5: macro expansion also applies to typed expressions (&var) */
    char *expanded = rx_macro_expand(ctx, src, err);
    if (!expanded) {
        return err->code;
    }
    if (rx_lex(expanded, "<expr>", &toks, &ntok, err) != R_OK) {
        free(expanded);
        return err->code;
    }
    rx_stmt *stmts;
    size_t nstmt;
    if (rx_parse_src(toks, ntok, expanded, &stmts, &nstmt, err) != R_OK) {
        rx_lex_free(toks, ntok);
        free(expanded);
        return err->code;
    }
    int rc = R_ERR_SYNTAX;
    snprintf(err->msg, sizeof(err->msg), "Not an expression");
    if (nstmt == 1 && stmts[0].kind == ST_EXPR && stmts[0].nexpr >= 1) {
        rc = rx_eval(ctx, stmts[0].exprs[0], out, err);
    } else {
        rx_rval_undef(out);
    }
    for (size_t i = 0; i < nstmt; i++) {
        rx_stmt_free(&stmts[i]);
    }
    free(stmts);
    rx_lex_free(toks, ntok);
    free(expanded);
    return rc;
}

/* ============================ function registry ============================ */

typedef struct {
    const rx_funcdef *defs;
    size_t n;
} fgroup;

static fgroup groups[6];
static int groups_ready = 0;

static void groups_init(void)
{
    if (groups_ready) {
        return;
    }
    groups[0] = (fgroup){ rx_funcs_str, rx_funcs_str_n };
    groups[1] = (fgroup){ rx_funcs_num, rx_funcs_num_n };
    groups[2] = (fgroup){ rx_funcs_date, rx_funcs_date_n };
    groups[3] = (fgroup){ rx_funcs_db, rx_funcs_db_n };
    groups[4] = (fgroup){ rx_funcs_sys, rx_funcs_sys_n };
    groups[5] = (fgroup){ rx_funcs_array, rx_funcs_array_n };
    groups_ready = 1;
}

int rx_func_id(const char *name, r_err *err)
{
    groups_init();
    size_t wl = strlen(name);
    int found = -1;
    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
        for (size_t i = 0; i < groups[g].n; i++) {
            const char *kw = groups[g].defs[i].name;
            int m = (wl >= 4 && strncasecmp(name, kw, wl) == 0 && strlen(kw) >= wl) ||
                    rx_strcasecmp(name, kw) == 0;
            if (m) {
                if (found >= 0) {
                    err->code = R_ERR_UNDEF_FUNC;
                    snprintf(err->msg, sizeof(err->msg), "Ambiguous function name %s", name);
                    return -1;
                }
                /* encode group and index */
                found = (int)(g * 100000 + (int)i);
            }
        }
    }
    if (found < 0) {
        err->code = R_ERR_UNDEF_FUNC;
        snprintf(err->msg, sizeof(err->msg), "Undefined function %s", name);
    }
    return found;
}

int rx_func_call(rx_ctx *ctx, int fid, size_t argc, const rval *argv, rval *out, r_err *err)
{
    groups_init();
    size_t g = (size_t)fid / 100000;
    size_t i = (size_t)fid % 100000;
    if (g >= sizeof(groups) / sizeof(groups[0]) || i >= groups[g].n) {
        err->code = R_ERR_UNDEF_FUNC;
        snprintf(err->msg, sizeof(err->msg), "Undefined function");
        return err->code;
    }
    if (err != &ctx->err) {
        memset(err, 0, sizeof(*err));
    }
    rx_rval_undef(out);
    return groups[g].defs[i].fn(ctx, argc, argv, out, err);
}

/* ============================ value helpers ============================ */

static int is_numlike(const rval *v)
{
    return v->t == RX_T_N || v->t == RX_T_I || v->t == RX_T_Y || v->t == RX_T_B;
}

static double as_num(const rval *v)
{
    double d = 0;
    rv_to_num(v, &d);
    return d;
}

static int rval_to_display(rx_ctx *ctx, const rval *v, strbuf *sb)
{
    char tmp[400];
    switch (v->t) {
    case RX_T_C:
        sb_put(sb, v->u.c.utf8 ? v->u.c.utf8 : "", v->u.c.n);
        break;
    case RX_T_N:
        rx_format_num(tmp, sizeof(tmp), v->u.n.d, v->u.n.dec, ctx->set.fixed);
        sb_puts(sb, tmp);
        break;
    case RX_T_I:
        snprintf(tmp, sizeof(tmp), "%d", v->u.i);
        sb_puts(sb, tmp);
        break;
    case RX_T_Y: {
        snprintf(tmp, sizeof(tmp), "%lld", (long long)(v->u.y / 10000));
        sb_puts(sb, tmp);
        snprintf(tmp, sizeof(tmp), ".%04lld", (long long)(v->u.y % 10000 < 0 ? -(v->u.y % 10000) : v->u.y % 10000));
        sb_puts(sb, tmp);
        break;
    }
    case RX_T_B:
        rx_format_num(tmp, sizeof(tmp), v->u.b, ctx->set.decimals, ctx->set.fixed);
        sb_puts(sb, tmp);
        break;
    case RX_T_D:
        if (v->u.d.y == 0) {
            sb_puts(sb, ctx->set.century ? "  /  /    " : "  /  /  ");
        } else if (ctx->set.century) {
            snprintf(tmp, sizeof(tmp), "%02d/%02d/%04d", v->u.d.m, v->u.d.d, v->u.d.y);
        } else {
            snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", v->u.d.m, v->u.d.d, v->u.d.y % 100);
        }
        if (v->u.d.y != 0) {
            sb_puts(sb, tmp);
        }
        break;
    case RX_T_T: {
        r_date d = v->u.tm.d;
        if (d.y == 0) {
            sb_puts(sb, "  /  /    ");
        } else {
            snprintf(tmp, sizeof(tmp), "%02d/%02d/%04d %02d:%02d:%02d", d.m, d.d, d.y,
                     v->u.tm.ms / 3600000, (v->u.tm.ms / 60000) % 60, (v->u.tm.ms / 1000) % 60);
            sb_puts(sb, tmp);
        }
        break;
    }
    case RX_T_L:
        sb_putc(sb, v->u.l == L_T ? 'T' : v->u.l == L_F ? 'F' : '?');
        break;
    case RX_T_M:
        if (v->u.memo.loaded) {
            sb_put(sb, v->u.memo.loaded->utf8, v->u.memo.loaded->n);
        }
        break;
    case RX_T_G:
        sb_puts(sb, "[General]");
        break;
    default:
        break;
    }
    return R_OK;
}

/* string comparison per SET EXACT (spec §7.6) */
static int str_eq(rx_ctx *ctx, const r_str *a, const r_str *b, int exact)
{
    (void)ctx;
    if (exact) {
        return rstr_cmp(a, b) == 0;
    }
    size_t n = a->n > b->n ? a->n : b->n;
    for (size_t i = 0; i < n; i++) {
        char ca = i < a->n ? a->utf8[i] : ' ';
        char cb = i < b->n ? b->utf8[i] : ' ';
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

static int str_rel(const r_str *a, const r_str *b)
{
    size_t n = a->n < b->n ? a->n : b->n;
    int c = memcmp(a->utf8, b->utf8, n);
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    if (a->n < b->n) {
        return -1;
    }
    if (a->n > b->n) {
        return 1;
    }
    return 0;
}

/* ============================ the VM ============================ */

static int vm_fail(rx_ctx *ctx, r_err *err, int code, int line, int col, const char *msg)
{
    err->code = code;
    err->line = line;
    err->col = col;
    snprintf(err->src_file, sizeof(err->src_file), "%s", ctx->prog);
    snprintf(err->msg, sizeof(err->msg), "%s", msg);
    return code;
}

static int load_var(rx_ctx *ctx, const char *name, rval *out, r_err *err, int line, int col)
{
    rx_memvar *mv = rx_memvar_find(ctx, name);
    if (mv && mv->is_array) {
        /* bare array name in an expression: FoxPro yields element 1 (AELEMENT 1) */
        rx_rval_free(out);
        if (mv->anelem == 0) {
            rx_rval_undef(out);
            return R_OK;
        }
        *out = mv->aelem[0];
        if (out->t == RX_T_C && out->u.c.utf8) {
            out->u.c.utf8 = rx_strdup(out->u.c.utf8);
            out->u.c.n = mv->aelem[0].u.c.n;
        } else if (out->t == RX_T_M && out->u.memo.loaded) {
            r_str *c = malloc(sizeof(r_str));
            if (!c) {
                RX_FATAL("out of memory");
            }
            c->n = mv->aelem[0].u.memo.loaded->n;
            c->utf8 = rx_strdup(mv->aelem[0].u.memo.loaded->utf8 ? mv->aelem[0].u.memo.loaded->utf8
                                                                : "");
            out->u.memo.loaded = c;
        }
        return R_OK;
    }
    if (mv) {
        rx_rval_free(out);
        *out = mv->val;
        if (mv->val.t == RX_T_C && mv->val.u.c.utf8) {
            out->u.c.utf8 = rx_strdup(mv->val.u.c.utf8);
            out->u.c.n = mv->val.u.c.n;
        } else if (mv->val.t == RX_T_M) {
            out->u.memo.blk = mv->val.u.memo.blk;
            out->u.memo.loaded = NULL;
            if (mv->val.u.memo.loaded) {
                r_str *c = malloc(sizeof(r_str));
                if (!c) {
                    RX_FATAL("out of memory");
                }
                c->n = mv->val.u.memo.loaded->n;
                c->utf8 = rx_strdup(mv->val.u.memo.loaded->utf8 ? mv->val.u.memo.loaded->utf8 : "");
                out->u.memo.loaded = c;
            }
        }
        return R_OK;
    }
    /* fall back to field of current work area */
    rx_dbf *dbf;
    if (rx_wa_dbf(0, &dbf) == R_OK) {
        size_t idx;
        if (rx_dbf_field_index(dbf, name, &idx) == R_OK) {
            return rx_dbf_get(dbf, idx, out);
        }
    }
    return vm_fail(ctx, err, R_ERR_VAR, line, col, "Variable not found");
}

static int load_field(rx_ctx *ctx, const char *name, rval *out, r_err *err, int line, int col)
{
    /* name is ALIAS->FIELD */
    const char *arrow = strstr(name, "->");
    if (!arrow) {
        return load_var(ctx, name, out, err, line, col);
    }
    char alias[64];
    size_t al = (size_t)(arrow - name);
    if (al >= sizeof(alias)) {
        al = sizeof(alias) - 1;
    }
    memcpy(alias, name, al);
    alias[al] = '\0';
    const char *fld = arrow + 2;
    int area = -1;
    for (int i = 1; i <= 255; i++) {
        if (rx_wa_used(i)) {
            const char *a;
            rx_wa_alias(i, &a);
            if (strcmp(a, alias) == 0) {
                area = i;
                break;
            }
        }
    }
    if (area < 0) {
        return vm_fail(ctx, err, R_ERR_FIELD, line, col, "Unknown alias");
    }
    rx_dbf *dbf;
    if (rx_wa_dbf(area, &dbf) != R_OK) {
        return vm_fail(ctx, err, R_ERR_DBF_NOTOPEN, line, col, "No table in use");
    }
    size_t idx;
    if (rx_dbf_field_index(dbf, fld, &idx) != R_OK) {
        return vm_fail(ctx, err, R_ERR_FIELD, line, col, "Unknown field");
    }
    return rx_dbf_get(dbf, idx, out);
}

static int bin_arith(rx_ctx *ctx, rval *a, rval *b, int op, rval *out, r_err *err, int line,
                     int col)
{
    if (is_numlike(a) && is_numlike(b)) {
        double x = as_num(a), y = as_num(b);
        double r;
        switch (op) {
        case OP_PLUS: r = x + y; break;
        case OP_MINUS: r = x - y; break;
        case OP_MUL: r = x * y; break;
        case OP_DIV:
            if (y == 0) {
                return vm_fail(ctx, err, R_ERR_DIVZERO, line, col, "Division by zero");
            }
            r = x / y;
            break;
        case OP_MOD:
            if (y == 0) {
                return vm_fail(ctx, err, R_ERR_DIVZERO, line, col, "Division by zero");
            }
            r = fmod(x, y);
            break;
        case OP_POW: r = pow(x, y); break;
        default: r = 0; break;
        }
        out->t = RX_T_N;
        out->u.n.d = r;
        out->u.n.dec = ctx->set.decimals;
        return R_OK;
    }
    if (op == OP_PLUS && a->t == RX_T_C && b->t == RX_T_C) {
        strbuf sb;
        sb_init(&sb);
        sb_put(&sb, a->u.c.utf8, a->u.c.n);
        sb_put(&sb, b->u.c.utf8, b->u.c.n);
        if (!sb.buf) {
            sb_putc(&sb, '\0');
            sb.len = 0;
        }
        rval_set_str(out, sb.buf, sb.len);
        return R_OK;
    }
    if (op == OP_MINUS && a->t == RX_T_C && b->t == RX_T_C) {
        /* FoxPro: trailing spaces of the left operand shift into the gap */
        size_t an = a->u.c.n;
        while (an > 0 && a->u.c.utf8[an - 1] == ' ') {
            an--;
        }
        strbuf sb;
        sb_init(&sb);
        sb_put(&sb, a->u.c.utf8, an);
        sb_put(&sb, b->u.c.utf8, b->u.c.n);
        if (!sb.buf) {
            sb_putc(&sb, '\0');
            sb.len = 0;
        }
        rval_set_str(out, sb.buf, sb.len);
        return R_OK;
    }
    if (op == OP_PLUS && a->t == RX_T_D && is_numlike(b)) {
        long j = rx_julian(a->u.d.y, a->u.d.m, a->u.d.d) + (long)as_num(b);
        int y, m, d;
        rx_civil(j, &y, &m, &d);
        if (y < 1 || y > 9999) {
            return vm_fail(ctx, err, R_ERR_RANGE, line, col, "Date out of range");
        }
        out->t = RX_T_D;
        out->u.d.y = (int16_t)y;
        out->u.d.m = (int16_t)m;
        out->u.d.d = (int16_t)d;
        return R_OK;
    }
    if (op == OP_MINUS && a->t == RX_T_D && is_numlike(b)) {
        long j = rx_julian(a->u.d.y, a->u.d.m, a->u.d.d) - (long)as_num(b);
        int y, m, d;
        rx_civil(j, &y, &m, &d);
        if (y < 1 || y > 9999) {
            return vm_fail(ctx, err, R_ERR_RANGE, line, col, "Date out of range");
        }
        out->t = RX_T_D;
        out->u.d.y = (int16_t)y;
        out->u.d.m = (int16_t)m;
        out->u.d.d = (int16_t)d;
        return R_OK;
    }
    if (op == OP_MINUS && a->t == RX_T_D && b->t == RX_T_D) {
        long j1 = rx_julian(a->u.d.y, a->u.d.m, a->u.d.d);
        long j2 = rx_julian(b->u.d.y, b->u.d.m, b->u.d.d);
        out->t = RX_T_N;
        out->u.n.d = (double)(j1 - j2);
        out->u.n.dec = 0;
        return R_OK;
    }
    if (op == OP_PLUS && a->t == RX_T_T && is_numlike(b)) {
        out->t = RX_T_T;
        out->u.tm = a->u.tm;
        out->u.tm.ms += (int32_t)(as_num(b) * 1000);
        return R_OK;
    }
    if (op == OP_MINUS && a->t == RX_T_T && is_numlike(b)) {
        out->t = RX_T_T;
        out->u.tm = a->u.tm;
        out->u.tm.ms -= (int32_t)(as_num(b) * 1000);
        return R_OK;
    }
    if (op == OP_MINUS && a->t == RX_T_T && b->t == RX_T_T) {
        out->t = RX_T_N;
        out->u.n.d = (double)(a->u.tm.ms - b->u.tm.ms) / 1000.0;
        out->u.n.dec = ctx->set.decimals;
        return R_OK;
    }
    return vm_fail(ctx, err, R_ERR_TYPE, line, col, "Operator/operand type mismatch");
}


/* memo values behave as their loaded character text in string contexts */
static const rval *as_str_view(const rval *v, rval *tmp)
{
    if (v->t == RX_T_M) {
        if (v->u.memo.loaded) {
            tmp->t = RX_T_C;
            tmp->u.c = *v->u.memo.loaded;
            return tmp;
        }
        tmp->t = RX_T_C;
        tmp->u.c.n = 0;
        tmp->u.c.utf8 = (char *)"";
        return tmp;
    }
    return v;
}

static int bin_cmp(rx_ctx *ctx, rval *a, rval *b, int op, rval *out, r_err *err, int line, int col)
{
    int res = 0;
    int ok = 1;
    rval ta, tb;
    if (is_numlike(a) && is_numlike(b)) {
        double x = as_num(a), y = as_num(b);
        switch (op) {
        case OP_EQ:
        case OP_EEQ: res = x == y; break;
        case OP_NE: res = x != y; break;
        case OP_LT: res = x < y; break;
        case OP_LE: res = x <= y; break;
        case OP_GT: res = x > y; break;
        case OP_GE: res = x >= y; break;
        default: ok = 0; break;
        }
    } else if (as_str_view(a, &ta)->t == RX_T_C && as_str_view(b, &tb)->t == RX_T_C) {
        a = (rval *)as_str_view(a, &ta);
        b = (rval *)as_str_view(b, &tb);
        switch (op) {
        case OP_EQ:
            res = str_eq(ctx, &a->u.c, &b->u.c, ctx->set.exact);
            break;
        case OP_EEQ:
            res = rstr_cmp(&a->u.c, &b->u.c) == 0;
            break;
        case OP_NE:
            res = !str_eq(ctx, &a->u.c, &b->u.c, ctx->set.exact);
            break;
        case OP_LT:
            res = str_rel(&a->u.c, &b->u.c) < 0;
            break;
        case OP_LE:
            res = str_rel(&a->u.c, &b->u.c) <= 0;
            break;
        case OP_GT:
            res = str_rel(&a->u.c, &b->u.c) > 0;
            break;
        case OP_GE:
            res = str_rel(&a->u.c, &b->u.c) >= 0;
            break;
        case OP_IN: {
            /* $ : substring, case-sensitive (spec §7.6) */
            const r_str *needle = &a->u.c, *hay = &b->u.c;
            res = 0;
            if (needle->n == 0) {
                res = 1;
            } else if (needle->n <= hay->n) {
                for (size_t i = 0; i + needle->n <= hay->n; i++) {
                    if (memcmp(hay->utf8 + i, needle->utf8, needle->n) == 0) {
                        res = 1;
                        break;
                    }
                }
            }
            break;
        }
        default:
            ok = 0;
            break;
        }
    } else if (a->t == RX_T_D && b->t == RX_T_D) {
        long j1 = rx_julian(a->u.d.y, a->u.d.m, a->u.d.d);
        long j2 = rx_julian(b->u.d.y, b->u.d.m, b->u.d.d);
        switch (op) {
        case OP_EQ:
        case OP_EEQ: res = j1 == j2; break;
        case OP_NE: res = j1 != j2; break;
        case OP_LT: res = j1 < j2; break;
        case OP_LE: res = j1 <= j2; break;
        case OP_GT: res = j1 > j2; break;
        case OP_GE: res = j1 >= j2; break;
        default: ok = 0; break;
        }
    } else if (a->t == RX_T_L && b->t == RX_T_L) {
        int x = a->u.l == L_T, y = b->u.l == L_T;
        switch (op) {
        case OP_EQ:
        case OP_EEQ: res = x == y; break;
        case OP_NE: res = x != y; break;
        case OP_LT: res = x < y; break;
        case OP_LE: res = x <= y; break;
        case OP_GT: res = x > y; break;
        case OP_GE: res = x >= y; break;
        default: ok = 0; break;
        }
    } else {
        ok = 0;
    }
    if (!ok) {
        return vm_fail(ctx, err, R_ERR_TYPE, line, col, "Operator/operand type mismatch");
    }
    out->t = RX_T_L;
    out->u.l = res ? L_T : L_F;
    return R_OK;
}

int rx_vm_run(rx_ctx *ctx, const rx_code *code, rval *out, r_err *err)
{
    rval stack[VM_STACK];
    int sp = 0;
    for (int i = 0; i < VM_STACK; i++) {
        rx_rval_undef(&stack[i]);
    }
    /* keep ctx->err intact when it is the caller's err: ON ERROR handlers
       read the trapped error via ERROR()/MESSAGE() */
    if (err != &ctx->err) {
        memset(err, 0, sizeof(*err));
    }
    int pc = 0;
    int rc = R_OK;
    while (pc >= 0 && (size_t)pc < code->n) {
        uint8_t op = code->ops[pc];
        uint32_t arg = code->arg[pc];
        switch (op) {
        case BOP_PUSH_STR: {
            if (sp >= VM_STACK) {
                rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Stack overflow");
                goto done;
            }
            rval_set_str(&stack[sp], rx_strdup(code->strs[arg]), strlen(code->strs[arg]));
            sp++;
            break;
        }
        case BOP_PUSH_NUM:
            if (sp >= VM_STACK) {
                rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Stack overflow");
                goto done;
            }
            stack[sp].t = RX_T_N;
            stack[sp].u.n.d = code->nums[arg];
            stack[sp].u.n.dec = ctx->set.decimals;
            sp++;
            break;
        case BOP_PUSH_LOGIC:
            if (sp >= VM_STACK) {
                rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Stack overflow");
                goto done;
            }
            stack[sp].t = RX_T_L;
            stack[sp].u.l = (rx_logic)arg;
            sp++;
            break;
        case BOP_PUSH_DATE: {
            if (sp >= VM_STACK) {
                rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Stack overflow");
                goto done;
            }
            const char *s = code->strs[arg];
            stack[sp].t = RX_T_D;
            memset(&stack[sp].u.d, 0, sizeof(r_date));
            if (s[0] == '^') {
                int y = 0, m = 0, d = 0;
                if (sscanf(s + 1, "%d-%d-%d", &y, &m, &d) == 3 && rx_date_valid(y, m, d)) {
                    stack[sp].u.d.y = (int16_t)y;
                    stack[sp].u.d.m = (int16_t)m;
                    stack[sp].u.d.d = (int16_t)d;
                } else {
                    rc = vm_fail(ctx, err, R_ERR_DATE_RANGE, 0, 0, "Date out of range");
                    goto done;
                }
            }
            sp++;
            break;
        }
        case BOP_VAR:
            if (sp >= VM_STACK) {
                rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Stack overflow");
                goto done;
            }
            rc = load_var(ctx, code->strs[arg], &stack[sp], err, 0, 0);
            if (rc != R_OK) {
                goto done;
            }
            sp++;
            break;
        case BOP_ARGNAME: {
            if (sp >= VM_STACK) {
                rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Stack overflow");
                goto done;
            }
            rx_memvar *mv = rx_memvar_find(ctx, code->strs[arg]);
            if (mv && mv->is_array) {
                stack[sp].t = RX_T_C;
                stack[sp].u.c.n = strlen(mv->name);
                stack[sp].u.c.utf8 = rx_strdup(mv->name);
            } else {
                rc = load_var(ctx, code->strs[arg], &stack[sp], err, 0, 0);
                if (rc == R_ERR_VAR) {
                    /* no memvar and no field of that name: pass the name so
                       array functions can create/inspect it (ACOPY(a, b)) */
                    rc = R_OK;
                    err->code = R_OK;
                    err->msg[0] = '\0';
                    stack[sp].t = RX_T_C;
                    char up[64];
                    snprintf(up, sizeof(up), "%s", code->strs[arg]);
                    for (char *c = up; *c; c++) {
                        *c = (char)toupper((unsigned char)*c);
                    }
                    stack[sp].u.c.n = strlen(up);
                    stack[sp].u.c.utf8 = rx_strdup(up);
                } else if (rc != R_OK) {
                    goto done;
                }
            }
            sp++;
            break;
        }
        case BOP_FIELD:
            if (sp >= VM_STACK) {
                rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Stack overflow");
                goto done;
            }
            rc = load_field(ctx, code->strs[arg], &stack[sp], err, 0, 0);
            if (rc != R_OK) {
                goto done;
            }
            sp++;
            break;
        case BOP_NEG: {
            if (sp < 1 || !is_numlike(&stack[sp - 1])) {
                rc = vm_fail(ctx, err, R_ERR_TYPE, 0, 0, "Type mismatch in unary -");
                goto done;
            }
            stack[sp - 1].t = RX_T_N;
            stack[sp - 1].u.n.d = -as_num(&stack[sp - 1]);
            stack[sp - 1].u.n.dec = ctx->set.decimals;
            break;
        }
        case BOP_NOT: {
            if (sp < 1 || stack[sp - 1].t != RX_T_L) {
                rc = vm_fail(ctx, err, R_ERR_TYPE, 0, 0, ".NOT. expects logical");
                goto done;
            }
            rx_logic l = stack[sp - 1].u.l;
            stack[sp - 1].u.l = (l == L_T) ? L_F : L_T;
            break;
        }
        case BOP_JMPF:
            if (sp < 1 || stack[sp - 1].t != RX_T_L) {
                rc = vm_fail(ctx, err, R_ERR_TYPE, 0, 0, ".AND. expects logical");
                goto done;
            }
            if (stack[sp - 1].u.l != L_T) {
                rx_rval_free(&stack[sp - 1]);
                stack[sp - 1].t = RX_T_L;
                stack[sp - 1].u.l = L_F;
                pc = (int)arg;
                continue;
            }
            sp--;
            break;
        case BOP_JMPT:
            if (sp < 1 || stack[sp - 1].t != RX_T_L) {
                rc = vm_fail(ctx, err, R_ERR_TYPE, 0, 0, ".OR. expects logical");
                goto done;
            }
            if (stack[sp - 1].u.l == L_T) {
                rx_rval_free(&stack[sp - 1]);
                stack[sp - 1].t = RX_T_L;
                stack[sp - 1].u.l = L_T;
                pc = (int)arg;
                continue;
            }
            sp--;
            break;
        case BOP_ADD:
        case BOP_SUB:
        case BOP_MUL:
        case BOP_DIV:
        case BOP_MOD:
        case BOP_POW: {
            if (sp < 2) {
                rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Stack underflow");
                goto done;
            }
            int opp = OP_NONE;
            switch (op) {
            case BOP_ADD: opp = OP_PLUS; break;
            case BOP_SUB: opp = OP_MINUS; break;
            case BOP_MUL: opp = OP_MUL; break;
            case BOP_DIV: opp = OP_DIV; break;
            case BOP_MOD: opp = OP_MOD; break;
            case BOP_POW: opp = OP_POW; break;
            default: break;
            }
            rval res;
            rx_rval_undef(&res);
            rc = bin_arith(ctx, &stack[sp - 2], &stack[sp - 1], opp, &res, err, 0, 0);
            if (rc != R_OK) {
                goto done;
            }
            rx_rval_free(&stack[sp - 2]);
            rx_rval_free(&stack[sp - 1]);
            stack[sp - 2] = res;
            sp--;
            break;
        }
        case BOP_EQ:
        case BOP_EEQ:
        case BOP_NE:
        case BOP_LT:
        case BOP_LE:
        case BOP_GT:
        case BOP_GE:
        case BOP_IN: {
            if (sp < 2) {
                rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Stack underflow");
                goto done;
            }
            int opp;
            switch (op) {
            case BOP_EQ: opp = OP_EQ; break;
            case BOP_EEQ: opp = OP_EEQ; break;
            case BOP_NE: opp = OP_NE; break;
            case BOP_LT: opp = OP_LT; break;
            case BOP_LE: opp = OP_LE; break;
            case BOP_GT: opp = OP_GT; break;
            case BOP_GE: opp = OP_GE; break;
            default: opp = OP_IN; break;
            }
            rval res;
            rx_rval_undef(&res);
            rc = bin_cmp(ctx, &stack[sp - 2], &stack[sp - 1], opp, &res, err, 0, 0);
            if (rc != R_OK) {
                goto done;
            }
            rx_rval_free(&stack[sp - 2]);
            rx_rval_free(&stack[sp - 1]);
            stack[sp - 2] = res;
            sp--;
            break;
        }
        case BOP_FUNC: {
            uint32_t argc = arg;
            if (sp < (int)argc + 1 || stack[sp - (int)argc - 1].t != RX_T_C) {
                rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Stack underflow");
                goto done;
            }
            const char *fname = stack[sp - (int)argc - 1].u.c.utf8;
            r_err ferr;
            memset(&ferr, 0, sizeof(ferr));
            int fid = rx_func_id(fname ? fname : "", &ferr);
            rval res;
            rx_rval_undef(&res);
            if (fid < 0) {
                /* M5: user-defined function (FUNCTION name ... ENDFUNC) */
                rc = rx_call_proc(ctx, fname ? fname : "", argc, &stack[sp - argc], &res, &ferr);
                if (rc != R_OK) {
                    char umsg[160];
                    snprintf(umsg, sizeof(umsg), "Undefined function %s", fname ? fname : "");
                    rc = vm_fail(ctx, err, R_ERR_UNDEF_FUNC, 0, 0, umsg);
                    goto done;
                }
            } else {
                rc = rx_func_call(ctx, fid, argc, &stack[sp - argc], &res, err);
                if (rc != R_OK) {
                    goto done;
                }
            }
            rx_rval_free(&stack[sp - (int)argc - 1]);
            for (uint32_t i = 0; i < argc; i++) {
                rx_rval_free(&stack[sp - argc + i]);
            }
            sp -= (int)argc + 1;
            stack[sp++] = res;
            break;
        }
        case BOP_ARRAY_LOAD: {
            /* stack: name(str), idx1, ..., idxN ; arg = N (1 or 2) */
            uint32_t argc = arg;
            if (sp < (int)argc + 1 || stack[sp - (int)argc - 1].t != RX_T_C) {
                rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Stack underflow");
                goto done;
            }
            const char *name = stack[sp - (int)argc - 1].u.c.utf8;
            double di = 1, dj = 1;
            if (argc >= 1) {
                if (!is_numlike(&stack[sp - argc])) {
                    rc = vm_fail(ctx, err, R_ERR_TYPE, 0, 0, "Array index must be numeric");
                    goto done;
                }
                rv_to_num(&stack[sp - argc], &di);
            }
            if (argc >= 2) {
                if (!is_numlike(&stack[sp - argc + 1])) {
                    rc = vm_fail(ctx, err, R_ERR_TYPE, 0, 0, "Array index must be numeric");
                    goto done;
                }
                rv_to_num(&stack[sp - argc + 1], &dj);
            }
            rval res;
            rx_rval_undef(&res);
            rc = rx_array_get(ctx, name ? name : "", (size_t)(di < 1 ? 1 : di),
                              (size_t)(dj < 1 ? 1 : dj), &res);
            if (rc != R_OK) {
                if (rc == R_ERR_TYPE) {
                    rc = vm_fail(ctx, err, R_ERR_VAR, 0, 0, "Variable is not an array");
                } else if (rc == R_ERR_RANGE) {
                    rc = vm_fail(ctx, err, R_ERR_RANGE, 0, 0, "Array index out of range");
                }
                goto done;
            }
            rx_rval_free(&stack[sp - (int)argc - 1]);
            for (uint32_t i = 0; i < argc; i++) {
                rx_rval_free(&stack[sp - argc + i]);
            }
            sp -= (int)argc + 1;
            stack[sp++] = res;
            break;
        }
        default:
            rc = vm_fail(ctx, err, R_ERR_SYNTAX, 0, 0, "Bad bytecode");
            goto done;
        }
        pc++;
    }
    if (sp < 1) {
        rc = vm_fail(ctx, err, R_ERR_STACK, 0, 0, "Empty expression");
        goto done;
    }
    rx_rval_free(out);
    *out = stack[sp - 1];
    sp--;
done:
    for (int i = 0; i < sp; i++) {
        rx_rval_free(&stack[i]);
    }
    return rc;
}

int rx_eval(rx_ctx *ctx, const rx_expr *e, rval *out, r_err *err)
{
    rx_code code;
    int rc = rx_compile_expr(e, &code, err);
    if (rc != R_OK) {
        return rc;
    }
    rc = rx_vm_run(ctx, &code, out, err);
    rx_code_free(&code);
    if (rc != R_OK && err->line == 0) {
        err->line = e->line;
        err->col = e->col;
    }
    return rc;
}

/* ============================ M5: structured execution ============================ */

static int exec_list(rx_ctx *ctx, rx_stmt **list, size_t n);

static int truthy(const rval *v)
{
    return v->t == RX_T_L && v->u.l == L_T;
}

/* register a PROCEDURE/FUNCTION definition (ST_PROC); re-registering the
   same name replaces the body (the rx_exec pre-pass + execution both hit it) */
static void register_proc(rx_ctx *ctx, rx_stmt *s)
{
    char up[64];
    snprintf(up, sizeof(up), "%.63s", s->target);
    for (char *c = up; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    int has_body = s->nkids > 0 && s->kids[0] && s->kids[0]->kids != NULL;
    rx_proc *pr = NULL;
    for (size_t i = 0; i < ctx->nprocs; i++) {
        if (strcmp(ctx->procs[i].name, up) == 0) {
            if (!has_body) {
                return; /* already registered by the rx_exec pre-pass */
            }
            pr = &ctx->procs[i];
            for (size_t j = 0; j < pr->nbody; j++) {
                rx_stmt_free(pr->body[j]);
                free(pr->body[j]);
            }
            free(pr->body);
            memset(pr, 0, sizeof(*pr));
            break;
        }
    }
    if (!pr) {
        if (ctx->nprocs == ctx->capprocs) {
            size_t nc = ctx->capprocs ? ctx->capprocs * 2 : 8;
            rx_proc *np = realloc(ctx->procs, nc * sizeof(rx_proc));
            if (!np) {
                RX_FATAL("out of memory");
            }
            ctx->procs = np;
            ctx->capprocs = nc;
        }
        pr = &ctx->procs[ctx->nprocs++];
        memset(pr, 0, sizeof(*pr));
    }
    snprintf(pr->name, sizeof(pr->name), "%s", up);
    pr->nparams = s->nstrs > 16 ? 16 : s->nstrs;
    for (size_t i = 0; i < pr->nparams; i++) {
        snprintf(pr->params[i], sizeof(pr->params[i]), "%s", s->strs[i]);
        for (char *c = pr->params[i]; *c; c++) {
            *c = (char)toupper((unsigned char)*c);
        }
    }
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "FUNC") == 0) {
            pr->is_func = 1;
        }
    }
    if (s->nkids > 0 && s->kids[0]) {
        pr->body = s->kids[0]->kids; /* steal the body list */
        pr->nbody = s->kids[0]->nkids;
        s->kids[0]->kids = NULL;
        s->kids[0]->nkids = s->kids[0]->capkids = 0;
    }
}

static rx_proc *find_proc(rx_ctx *ctx, const char *name)
{
    char up[64];
    snprintf(up, sizeof(up), "%s", name);
    for (char *c = up; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    rx_proc *found = NULL;
    size_t wl = strlen(up);
    for (size_t i = 0; i < ctx->nprocs; i++) {
        int m = (wl >= 4 && strncasecmp(up, ctx->procs[i].name, wl) == 0 &&
                 strlen(ctx->procs[i].name) >= wl) ||
                strcmp(up, ctx->procs[i].name) == 0;
        if (m) {
            if (found && strcmp(found->name, ctx->procs[i].name) != 0) {
                return NULL; /* ambiguous */
            }
            found = &ctx->procs[i];
        }
    }
    return found;
}

void rx_frame_free(rx_ctx *ctx, rx_frame *f)
{
    (void)ctx;
    for (size_t i = 0; i < f->nvars; i++) {
        rx_rval_free(&f->vars[i].val);
        if (f->vars[i].is_array) {
            for (size_t j = 0; j < f->vars[i].anelem; j++) {
                rx_rval_free(&f->vars[i].aelem[j]);
            }
            free(f->vars[i].aelem);
        }
    }
    free(f->vars);
    f->vars = NULL;
    f->nvars = f->capvars = 0;
}

int rx_call_proc(rx_ctx *ctx, const char *name, size_t argc, const rval *argv, rval *out,
                 r_err *err)
{
    rx_proc *pr = find_proc(ctx, name);
    if (!pr) {
        err->code = R_ERR_UNDEF_FUNC;
        snprintf(err->msg, sizeof(err->msg), "Undefined procedure %s", name);
        return err->code;
    }
    if (argc > pr->nparams) {
        err->code = R_ERR_SYNTAX;
        snprintf(err->msg, sizeof(err->msg), "Too many arguments to %s", pr->name);
        return err->code;
    }
    int depth = 0;
    for (rx_frame *f = ctx->frame; f; f = f->up) {
        depth++;
    }
    if (depth >= 64) {
        err->code = R_ERR_STACK;
        snprintf(err->msg, sizeof(err->msg), "Procedure call depth limit (64)");
        return err->code;
    }
    rx_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.up = ctx->frame;
    ctx->frame = &fr;
    for (size_t i = 0; i < pr->nparams; i++) {
        /* create in frame and set value */
        rx_memvar *mv = NULL;
        if (fr.nvars == fr.capvars) {
            size_t nc = fr.capvars ? fr.capvars * 2 : 8;
            rx_memvar *nv = realloc(fr.vars, nc * sizeof(rx_memvar));
            if (!nv) {
                RX_FATAL("out of memory");
            }
            fr.vars = nv;
            fr.capvars = nc;
        }
        mv = &fr.vars[fr.nvars++];
        memset(mv, 0, sizeof(*mv));
        snprintf(mv->name, sizeof(mv->name), "%s", pr->params[i]);
        rx_rval_undef(&mv->val);
        if (i < argc) {
            /* LPARAMETERS: by reference — share via shallow copy is wrong for
               later writes; v1 passes by value (deviation noted) */
            rx_memvar_set(ctx, mv->name, &argv[i]);
        }
    }
    ctx->returning = 0;
    rx_rval_free(&ctx->retval);
    rx_rval_undef(&ctx->retval);
    int rc = exec_list(ctx, pr->body, pr->nbody);
    int prc = ctx->returning;
    (void)prc;
    ctx->returning = 0;
    if (rc != R_OK) {
        rx_frame_free(ctx, &fr);
        ctx->frame = fr.up;
        return rc;
    }
    *out = ctx->retval; /* transfer */
    rx_rval_undef(&ctx->retval);
    rx_frame_free(ctx, &fr);
    ctx->frame = fr.up;
    return R_OK;
}

/* ---- M5 command handlers that live in vm.c ---- */

int rx_cmd_do(rx_ctx *ctx, rx_stmt *s)
{
    rval *args = NULL;
    size_t na = s->nexpr;
    if (na) {
        args = calloc(na, sizeof(rval));
        if (!args) {
            RX_FATAL("out of memory");
        }
        for (size_t i = 0; i < na; i++) {
            rx_rval_undef(&args[i]);
            int rc = rx_eval(ctx, s->exprs[i], &args[i], &ctx->err);
            if (rc != R_OK) {
                for (size_t j = 0; j < i; j++) {
                    rx_rval_free(&args[j]);
                }
                free(args);
                return rc;
            }
        }
    }
    rval out;
    rx_rval_undef(&out);
    r_err err;
    memset(&err, 0, sizeof(err));
    int rc = rx_call_proc(ctx, s->target, na, args, &out, &err);
    rx_rval_free(&out);
    for (size_t i = 0; i < na; i++) {
        rx_rval_free(&args[i]);
    }
    free(args);
    if (rc != R_OK) {
        return rx_raise(ctx, err.code, s->line, s->col, "%s", err.msg);
    }
    return R_OK;
}

int rx_cmd_return(rx_ctx *ctx, rx_stmt *s)
{
    rx_rval_free(&ctx->retval);
    rx_rval_undef(&ctx->retval);
    if (s->nexpr > 0) {
        int rc = rx_eval(ctx, s->exprs[0], &ctx->retval, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
    }
    ctx->returning = 1;
    return R_OK;
}

int rx_cmd_parameters(rx_ctx *ctx, rx_stmt *s, int lpar)
{
    (void)lpar; /* by-reference binding lands with the full M5 parameter model */
    /* positional binding already happened in rx_call_proc; a PARAMETERS
       list inside a body just ensures the names exist in the frame */
    for (size_t i = 0; i < s->nstrs; i++) {
        if (!rx_memvar_lookup(ctx, s->strs[i])) {
            rx_memvar_private(ctx, s->strs[i]);
        }
    }
    return R_OK;
}

int rx_cmd_private_public(rx_ctx *ctx, rx_stmt *s, int pub)
{
    int has_all = 0, is_like = 0;
    const char *pat = NULL;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "ALL") == 0) {
            has_all = 1;
            if (rx_strcasecmp(s->clauses[i].word, "LIKE") == 0) {
                is_like = 1;
            } else if (rx_strcasecmp(s->clauses[i].word, "EXCEPT") == 0) {
                is_like = 0;
            }
        } else if (strcmp(s->clauses[i].key, "PATTERN") == 0) {
            pat = s->clauses[i].word;
        }
    }
    if (!has_all) {
        for (size_t i = 0; i < s->nstrs; i++) {
            if (pub) {
                rx_memvar_set_public(ctx, s->strs[i], &(rval){ 0 });
            } else {
                rx_memvar_private(ctx, s->strs[i]);
            }
        }
        return R_OK;
    }
    /* ALL [LIKE|EXCEPT pattern]: apply to every visible variable */
    /* gather names from global table + current frame */
    char names[256][64];
    size_t nn = 0;
    for (size_t i = 0; i < ctx->nvars && nn < 256; i++) {
        snprintf(names[nn++], 64, "%s", ctx->vars[i].name);
    }
    if (ctx->frame) {
        for (size_t i = 0; i < ctx->frame->nvars && nn < 256; i++) {
            snprintf(names[nn++], 64, "%s", ctx->frame->vars[i].name);
        }
    }
    for (size_t i = 0; i < nn; i++) {
        int match = !pat || rx_wildmatch(pat, names[i]);
        if (pat && !is_like) {
            match = !match;
        }
        if (!pat) {
            match = 1;
        }
        if (match) {
            if (pub) {
                rx_memvar_set_public(ctx, names[i], &(rval){ 0 });
            } else {
                rx_memvar_private(ctx, names[i]);
            }
        }
    }
    return R_OK;
}

int rx_cmd_release(rx_ctx *ctx, rx_stmt *s)
{
    int has_all = 0, is_like = 0;
    const char *pat = NULL;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "ALL") == 0) {
            has_all = 1;
            if (rx_strcasecmp(s->clauses[i].word, "LIKE") == 0) {
                is_like = 1;
            }
        } else if (strcmp(s->clauses[i].key, "PATTERN") == 0) {
            pat = s->clauses[i].word;
        }
    }
    if (!has_all) {
        for (size_t i = 0; i < s->nstrs; i++) {
            rx_memvar_release(ctx, s->strs[i]);
        }
        return R_OK;
    }
    char names[256][64];
    size_t nn = 0;
    for (size_t i = 0; i < ctx->nvars && nn < 256; i++) {
        snprintf(names[nn++], 64, "%s", ctx->vars[i].name);
    }
    if (ctx->frame) {
        for (size_t i = 0; i < ctx->frame->nvars && nn < 256; i++) {
            snprintf(names[nn++], 64, "%s", ctx->frame->vars[i].name);
        }
    }
    for (size_t i = 0; i < nn; i++) {
        int match = !pat || rx_wildmatch(pat, names[i]);
        if (pat && !is_like) {
            match = !match;
        }
        if (!pat) {
            match = 1;
        }
        if (match) {
            rx_memvar_release(ctx, names[i]);
        }
    }
    return R_OK;
}

int rx_cmd_on(rx_ctx *ctx, rx_stmt *s)
{
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "ERROR") == 0) {
            if (s->nstrs > 0) {
                snprintf(ctx->on_error, sizeof(ctx->on_error), "%s", s->strs[0]);
            } else {
                ctx->on_error[0] = '\0'; /* ON ERROR clears */
            }
        }
        /* ESCAPE / KEY LABEL are accepted and stored for the interactive
           layer (M5 batch: no dispatch); ON KEY LABEL F1 help at M9 */
    }
    return R_OK;
}

int rx_cmd_wait(rx_ctx *ctx, rx_stmt *s)
{
    const char *msg = s->nstrs > 0 ? s->strs[0] : NULL;
    if (s->nexpr > 0) {
        rval v;
        rx_rval_undef(&v);
        int rc = rx_eval(ctx, s->exprs[0], &v, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        strbuf sb;
        sb_init(&sb);
        rx_vm_display(ctx, &v, &sb);
        if (sb.buf) {
            rx_out(ctx, sb.buf, sb.len);
        }
        sb_free(&sb);
        rx_rval_free(&v);
        msg = NULL;
    }
    if (msg) {
        rx_out(ctx, msg, strlen(msg));
    } else if (s->nexpr == 0 && s->nstrs == 0) {
        rx_outs(ctx, "Press any key to continue...");
    }
    rx_outs(ctx, "\n");
    /* in test mode never block on a keypress */
    if (!ctx->test_mode && ctx->in) {
        char line[1024];
        if (!fgets(line, sizeof(line), ctx->in)) {
            /* EOF: proceed */
        }
    }
    return R_OK;
}

int rx_cmd_input(rx_ctx *ctx, rx_stmt *s, int eval)
{
    if (s->nstrs > 0) {
        rx_out(ctx, s->strs[0], strlen(s->strs[0]));
    } else if (s->nexpr > 0) {
        rval v;
        rx_rval_undef(&v);
        int rc = rx_eval(ctx, s->exprs[0], &v, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        strbuf sb;
        sb_init(&sb);
        rx_vm_display(ctx, &v, &sb);
        if (sb.buf) {
            rx_out(ctx, sb.buf, sb.len);
        }
        sb_free(&sb);
        rx_rval_free(&v);
    }
    char line[4096];
    if (!ctx->in || !fgets(line, sizeof(line), ctx->in)) {
        line[0] = '\0';
    }
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
        line[--l] = '\0';
    }
    if (eval) {
        /* INPUT evaluates the line as an expression */
        rval v;
        rx_rval_undef(&v);
        r_err err;
        memset(&err, 0, sizeof(err));
        char *expanded = rx_macro_expand(ctx, line, &err);
        int rc;
        if (!expanded) {
            return rx_raise(ctx, err.code, s->line, s->col, "%s", err.msg);
        }
        rx_tok *toks;
        size_t ntok;
        if (rx_lex(expanded, "<input>", &toks, &ntok, &err) == R_OK) {
            rx_stmt *st;
            size_t ns;
            if (rx_parse(toks, ntok, &st, &ns, &err) == R_OK && ns == 1 && st[0].kind == ST_EXPR) {
                rc = rx_eval(ctx, st[0].exprs[0], &v, &err);
                for (size_t i = 0; i < ns; i++) {
                    rx_stmt_free(&st[i]);
                }
                free(st);
            } else {
                rc = R_ERR_SYNTAX;
            }
            rx_lex_free(toks, ntok);
        } else {
            rc = err.code;
        }
        free(expanded);
        if (rc != R_OK) {
            return rx_raise(ctx, rc, s->line, s->col, "%s", err.msg[0] ? err.msg : "Syntax error");
        }
        rc = rx_memvar_set(ctx, s->target, &v);
        rx_rval_free(&v);
        return rc;
    }
    /* ACCEPT: raw string */
    rval v;
    rx_rval_undef(&v);
    v.t = RX_T_C;
    v.u.c.n = l;
    v.u.c.utf8 = rx_strdup(line);
    int rc = rx_memvar_set(ctx, s->target, &v);
    rx_rval_free(&v);
    return rc;
}

int rx_cmd_dimension(rx_ctx *ctx, rx_stmt *s)
{
    for (size_t k = 0; k < s->nkids; k++) {
        rx_stmt *kid = s->kids[k];
        size_t d1 = 1, d2 = 0;
        if (kid->nexpr >= 1) {
            rval v;
            rx_rval_undef(&v);
            int rc = rx_eval(ctx, kid->exprs[0], &v, &ctx->err);
            if (rc != R_OK) {
                return rc;
            }
            double d = 0;
            rv_to_num(&v, &d);
            rx_rval_free(&v);
            d1 = (size_t)(d < 0 ? 0 : d);
        }
        if (kid->nexpr >= 2) {
            rval v;
            rx_rval_undef(&v);
            int rc = rx_eval(ctx, kid->exprs[1], &v, &ctx->err);
            if (rc != R_OK) {
                return rc;
            }
            double d = 0;
            rv_to_num(&v, &d);
            rx_rval_free(&v);
            d2 = (size_t)(d < 0 ? 0 : d);
        }
        rx_array_def(ctx, kid->target, d1, d2);
    }
    return R_OK;
}

/* ---- macro expansion (&var, §7.2, depth <= 16) ---- */

static char *expand_rec(rx_ctx *ctx, const char *src, int depth, r_err *err)
{
    if (depth > 16) {
        err->code = R_ERR_MACRO_DEPTH;
        snprintf(err->msg, sizeof(err->msg), "Macro expansion depth limit (16)");
        return NULL;
    }
    strbuf out;
    sb_init(&out);
    int substituted = 0;
    size_t i = 0, n = strlen(src);
    while (i < n) {
        char c = src[i];
        if (c == '"' || c == '\'') {
            /* string literal: copy verbatim, honoring doubled-quote escapes */
            char q = c;
            sb_putc(&out, c);
            i++;
            while (i < n) {
                sb_putc(&out, src[i]);
                if (src[i] == q) {
                    if (i + 1 < n && src[i + 1] == q) {
                        sb_putc(&out, src[i + 1]);
                        i += 2;
                        continue;
                    }
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        if (c == '&' && i + 1 < n && src[i + 1] == '&') {
            /* && comment: copy the rest of the line verbatim */
            while (i < n) {
                sb_putc(&out, src[i++]);
            }
            break;
        }
        if (c == '&' && i + 1 < n && (isalpha((unsigned char)src[i + 1]) || src[i + 1] == '_')) {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)src[j]) || src[j] == '_' || src[j] == '.')) {
                j++;
            }
            char name[128];
            size_t nl = j - (i + 1);
            if (nl >= sizeof(name)) {
                nl = sizeof(name) - 1;
            }
            memcpy(name, src + i + 1, nl);
            name[nl] = '\0';
            rx_memvar *mv = rx_memvar_lookup(ctx, name);
            if (!mv || mv->is_array || mv->val.t != RX_T_C || !mv->val.u.c.utf8) {
                err->code = R_ERR_VAR;
                snprintf(err->msg, sizeof(err->msg), "Variable not found: %s", name);
                sb_free(&out);
                return NULL;
            }
            sb_puts(&out, mv->val.u.c.utf8);
            substituted = 1;
            i = j;
            continue;
        }
        sb_putc(&out, c);
        i++;
    }
    if (!out.buf) {
        sb_putc(&out, '\0');
        out.len = 0;
    }
    if (substituted) {
        char *again = expand_rec(ctx, out.buf, depth + 1, err);
        if (!again) {
            sb_free(&out);
            return NULL;
        }
        sb_free(&out);
        return again;
    }
    return out.buf;
}

char *rx_macro_expand(rx_ctx *ctx, const char *src, r_err *err)
{
    return expand_rec(ctx, src, 0, err);
}

/* ---- .prg file runner with .RXP cache (rxp.c) ---- */

int rx_run_file(rx_ctx *ctx, const char *path);

/* ============================ statement execution ============================ */

static int exec_one(rx_ctx *ctx, rx_stmt *s)
{
    int rc = R_OK;
    switch (s->kind) {
    case ST_EMPTY:
        break;
    case ST_PROC:
        register_proc(ctx, s);
        break;
    case ST_IF: {
        int done = 0;
        for (size_t b = 0; b < s->ncond && b < s->nkids; b++) {
            rval v;
            rx_rval_undef(&v);
            rc = rx_eval(ctx, s->exprs[b], &v, &ctx->err);
            if (rc != R_OK) {
                return rc;
            }
            int t = truthy(&v);
            rx_rval_free(&v);
            if (t) {
                rc = exec_list(ctx, s->kids[b]->kids, s->kids[b]->nkids);
                done = 1;
                break;
            }
        }
        if (!done && s->nkids > s->ncond) {
            rc = exec_list(ctx, s->kids[s->ncond]->kids, s->kids[s->ncond]->nkids);
        }
        return rc;
    }
    case ST_CASE: {
        int done = 0;
        for (size_t b = 0; b < s->ncond && b < s->nkids; b++) {
            rval v;
            rx_rval_undef(&v);
            rc = rx_eval(ctx, s->exprs[b], &v, &ctx->err);
            if (rc != R_OK) {
                return rc;
            }
            int t = truthy(&v);
            rx_rval_free(&v);
            if (t) {
                rc = exec_list(ctx, s->kids[b]->kids, s->kids[b]->nkids);
                done = 1;
                break;
            }
        }
        if (!done && s->nkids > s->ncond) {
            rc = exec_list(ctx, s->kids[s->ncond]->kids, s->kids[s->ncond]->nkids);
        }
        return rc;
    }
    case ST_WHILE: {
        for (;;) {
            if (ctx->abort || ctx->returning || ctx->loop_ctl == RX_CTL_EXIT) {
                break;
            }
            rval v;
            rx_rval_undef(&v);
            rc = rx_eval(ctx, s->exprs[0], &v, &ctx->err);
            if (rc != R_OK) {
                return rc;
            }
            int t = truthy(&v);
            rx_rval_free(&v);
            if (!t) {
                break;
            }
            ctx->loop_ctl = RX_CTL_NONE;
            rc = exec_list(ctx, s->kids[0]->kids, s->kids[0]->nkids);
            if (rc != R_OK) {
                return rc;
            }
            if (ctx->loop_ctl == RX_CTL_LOOP) {
                ctx->loop_ctl = RX_CTL_NONE;
                continue;
            }
            if (ctx->loop_ctl == RX_CTL_EXIT) {
                ctx->loop_ctl = RX_CTL_NONE; /* EXIT leaves this loop only */
                break;
            }
        }
        return R_OK;
    }
    case ST_FOR: {
        rval vs, ve, vstep;
        rx_rval_undef(&vs);
        rx_rval_undef(&ve);
        rx_rval_undef(&vstep);
        rc = rx_eval(ctx, s->exprs[0], &vs, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        rc = rx_eval(ctx, s->exprs[1], &ve, &ctx->err);
        if (rc != R_OK) {
            rx_rval_free(&vs);
            return rc;
        }
        double step = 1;
        if (s->ncond >= 3) {
            rc = rx_eval(ctx, s->exprs[2], &vstep, &ctx->err);
            if (rc != R_OK) {
                rx_rval_free(&vs);
                rx_rval_free(&ve);
                return rc;
            }
            rv_to_num(&vstep, &step);
            rx_rval_free(&vstep);
        }
        double cur = 0, end = 0;
        rv_to_num(&vs, &cur);
        rv_to_num(&ve, &end);
        rx_rval_free(&vs);
        rx_rval_free(&ve);
        if (step == 0) {
            step = 1;
        }
        for (;;) {
            if (ctx->abort || ctx->returning || ctx->loop_ctl == RX_CTL_EXIT) {
                break;
            }
            int in = step > 0 ? cur <= end : cur >= end;
            if (!in) {
                break;
            }
            rval cv;
            rx_rval_undef(&cv);
            cv.t = RX_T_N;
            cv.u.n.d = cur;
            cv.u.n.dec = 0;
            rc = rx_memvar_set(ctx, s->target, &cv);
            rx_rval_free(&cv);
            if (rc != R_OK) {
                return rc;
            }
            ctx->loop_ctl = RX_CTL_NONE;
            rc = exec_list(ctx, s->kids[0]->kids, s->kids[0]->nkids);
            if (rc != R_OK) {
                return rc;
            }
            if (ctx->loop_ctl == RX_CTL_LOOP) {
                ctx->loop_ctl = RX_CTL_NONE;
                cur += step;
                continue;
            }
            if (ctx->loop_ctl == RX_CTL_EXIT) {
                ctx->loop_ctl = RX_CTL_NONE; /* EXIT leaves this loop only */
                break;
            }
            cur += step;
        }
        return R_OK;
    }
    case ST_SCAN:
        return rx_cmd_scan(ctx, s);
    case ST_TEXT:
        if (s->raw) {
            rx_out(ctx, s->raw, strlen(s->raw));
        }
        return R_OK;
    case ST_DO:
        return rx_cmd_do(ctx, s);
    case ST_RETURN:
        return rx_cmd_return(ctx, s);
    case ST_EXIT:
        ctx->loop_ctl = RX_CTL_EXIT;
        return R_OK;
    case ST_LOOP:
        ctx->loop_ctl = RX_CTL_LOOP;
        return R_OK;
    case ST_RETRY:
        ctx->retry = 1;
        ctx->returning = 1; /* unwind out of the handler back to the error site */
        return R_OK;
    case ST_PARAMETERS: {
        int lpar = 0;
        for (size_t i = 0; i < s->nclause; i++) {
            if (strcmp(s->clauses[i].key, "LPAR") == 0) {
                lpar = 1;
            }
        }
        return rx_cmd_parameters(ctx, s, lpar);
    }
    case ST_PRIVATE:
        return rx_cmd_private_public(ctx, s, 0);
    case ST_PUBLIC:
        return rx_cmd_private_public(ctx, s, 1);
    case ST_RELEASE:
        return rx_cmd_release(ctx, s);
    case ST_ON:
        return rx_cmd_on(ctx, s);
    case ST_WAIT:
        return rx_cmd_wait(ctx, s);
    case ST_INPUT:
        return rx_cmd_input(ctx, s, 1);
    case ST_ACCEPT:
        return rx_cmd_input(ctx, s, 0);
    case ST_CANCEL:
        ctx->abort = 1;
        ctx->quit = 1;
        return R_OK;
    case ST_SUSPEND:
        /* interactive SUSPEND/RESUME land with the M9 event loop; here: note */
        if (ctx->msg_line && !ctx->out) {
            ctx->msg_line(ctx->msg_ud, "SUSPEND - program paused (RESUME to continue)");
        }
        return R_OK;
    case ST_RESUME:
        return R_OK;
    case ST_ENDMARK:
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "Unexpected end-of-block keyword");
    case ST_DIMENSION:
        return rx_cmd_dimension(ctx, s);
    case ST_PRINT:
        rc = rx_cmd_print(ctx, s, 1);
        break;
    case ST_PRINTN:
        rc = rx_cmd_print(ctx, s, 0);
        break;
    case ST_USE:
        rc = rx_cmd_use(ctx, s);
        break;
    case ST_CLOSE:
        rc = rx_cmd_close(ctx, s);
        break;
    case ST_SELECT:
        rc = rx_cmd_select(ctx, s);
        break;
    case ST_LIST:
        rc = rx_cmd_list(ctx, s, 0);
        break;
    case ST_DISPLAY:
        rc = rx_cmd_list(ctx, s, 1);
        break;
    case ST_GO:
        rc = rx_cmd_go(ctx, s);
        break;
    case ST_SKIP:
        rc = rx_cmd_skip(ctx, s);
        break;
    case ST_ASSIGN:
        rc = rx_cmd_assign(ctx, s);
        break;
    case ST_STORE:
        rc = rx_cmd_store(ctx, s);
        break;
    case ST_SET:
        rc = rx_cmd_set(ctx, s);
        break;
    case ST_QUIT:
        ctx->quit = 1;
        ctx->abort = 1;
        break;
    case ST_CLEAR:
        break;
    case ST_BROWSE:
        rc = rx_cmd_browse(ctx, s);
        break;
    case ST_APPEND:
        rc = rx_cmd_append(ctx, s);
        break;
    case ST_REPLACE:
        rc = rx_cmd_replace(ctx, s);
        break;
    case ST_DELETE:
        rc = rx_cmd_delete(ctx, s, 0);
        break;
    case ST_RECALL:
        rc = rx_cmd_delete(ctx, s, 1);
        break;
    case ST_PACK:
        rc = rx_cmd_pack(ctx, s);
        break;
    case ST_ZAP:
        rc = rx_cmd_zap(ctx, s);
        break;
    case ST_LOCATE:
        rc = rx_cmd_locate(ctx, s, 0);
        break;
    case ST_CONTINUE:
        rc = rx_cmd_locate(ctx, s, 1);
        break;
    case ST_INDEX:
        rc = rx_cmd_index_on(ctx, s);
        break;
    case ST_SEEK:
        rc = rx_cmd_seek(ctx, s);
        break;
    case ST_REINDEX:
        rc = rx_cmd_reindex(ctx, s);
        break;
    case ST_COPY:
        rc = rx_cmd_copy(ctx, s);
        break;
    case ST_CREATE:
        rc = rx_cmd_create(ctx, s);
        break;
    case ST_FLUSH:
        rc = rx_cmd_flush(ctx, s);
        break;
    case ST_TYPE:
        rc = rx_cmd_type(ctx, s);
        break;
    case ST_UNLOCK:
        rc = rx_cmd_unlock(ctx, s);
        break;
    case ST_EXPR: {
        rval v;
        rx_rval_undef(&v);
        rc = rx_eval(ctx, s->exprs[0], &v, &ctx->err);
        rx_rval_free(&v);
        break;
    }
    }
    return rc;
}

/* M5 runtime macro expansion (§7.2): a statement whose raw source span
   contains '&' is re-expanded, re-lexed and re-parsed at execution time, so
   &var sees the memvar state at that moment (FoxPro semantics). */
static int exec_expanded(rx_ctx *ctx, rx_stmt *s)
{
    r_err err;
    memset(&err, 0, sizeof(err));
    char *ex = rx_macro_expand(ctx, s->raw, &err);
    if (!ex) {
        ctx->err.code = err.code;
        snprintf(ctx->err.msg, sizeof(ctx->err.msg), "%s", err.msg);
        ctx->err.line = s->line;
        ctx->err.col = s->col;
        return err.code;
    }
    rx_tok *toks;
    size_t ntok;
    int rc = rx_lex(ex, ctx->prog, &toks, &ntok, &err);
    if (rc == R_OK) {
        rx_stmt *st;
        size_t ns;
        rc = rx_parse_src(toks, ntok, ex, &st, &ns, &err);
        if (rc == R_OK) {
            rx_stmt **list = malloc((ns ? ns : 1) * sizeof(rx_stmt *));
            if (!list) {
                RX_FATAL("out of memory");
            }
            for (size_t i = 0; i < ns; i++) {
                list[i] = &st[i];
            }
            rc = exec_list(ctx, list, ns);
            free(list);
            for (size_t i = 0; i < ns; i++) {
                rx_stmt_free(&st[i]);
            }
            free(st);
        }
        rx_lex_free(toks, ntok);
    }
    free(ex);
    if (rc != R_OK) {
        if (err.line > 0) {
            ctx->err.line = s->line + err.line - 1;
            ctx->err.col = err.col;
        } else {
            ctx->err.line = s->line;
            ctx->err.col = s->col;
        }
        if (err.code) {
            ctx->err.code = err.code;
        }
        if (err.msg[0]) {
            snprintf(ctx->err.msg, sizeof(ctx->err.msg), "%s", err.msg);
        }
    }
    return rc;
}

static int exec_list(rx_ctx *ctx, rx_stmt **list, size_t n)
{
    size_t retries = 0;
    for (size_t i = 0; i < n; i++) {
        if (ctx->abort || ctx->returning || ctx->loop_ctl == RX_CTL_EXIT ||
            ctx->loop_ctl == RX_CTL_LOOP) {
            break;
        }
        rx_stmt *s = list[i];
        int rc;
        if (s->raw && s->kind != ST_TEXT && strchr(s->raw, '&')) {
            rc = exec_expanded(ctx, s);
        } else {
            rc = exec_one(ctx, s);
        }
        if (rc != R_OK) {
            /* M5 ON ERROR dispatch (§7.4): run the handler, then resume after
               the erroring statement — or re-run it when the handler RETRYs */
            if (ctx->on_error[0] && !ctx->in_on_error && !ctx->quit) {
                if (ctx->err.line == 0) {
                    ctx->err.line = s->line;
                    ctx->err.col = s->col;
                }
                ctx->err_printed = 1; /* suppress the default diagnostic */
                ctx->in_on_error = 1;
                ctx->retry = 0;
                rval dummy;
                rx_rval_undef(&dummy);
                r_err herr;
                memset(&herr, 0, sizeof(herr));
                rx_call_proc(ctx, ctx->on_error, 0, NULL, &dummy, &herr);
                rx_rval_free(&dummy);
                ctx->in_on_error = 0;
                if (ctx->retry) {
                    ctx->retry = 0;
                    ctx->err.code = R_OK; /* re-run re-raises if it fails again */
                    ctx->err.msg[0] = '\0';
                    if (++retries > 1000) {
                        ctx->abort = 1;
                        ctx->err_printed = 0;
                        return rc; /* RETRY storm: give up with the last error */
                    }
                    i--; /* RETRY: re-run the same statement */
                    continue;
                }
                retries = 0;
                ctx->abort = 0;
                ctx->err.code = R_OK;
                ctx->err.msg[0] = '\0';
                continue;
            }
            if (ctx->err.line == 0) {
                ctx->err.line = s->line;
                ctx->err.col = s->col;
            }
            ctx->abort = 1;
            return rc;
        }
    }
    return R_OK;
}

int rx_exec(rx_ctx *ctx, rx_stmt *stmts, size_t n)
{
    /* FoxPro compiles the whole file first: every top-level PROCEDURE/FUNCTION
       is callable from any statement, regardless of textual order */
    for (size_t i = 0; i < n; i++) {
        if (stmts[i].kind == ST_PROC) {
            register_proc(ctx, &stmts[i]);
        }
    }
    rx_stmt **list = malloc((n ? n : 1) * sizeof(rx_stmt *));
    if (!list) {
        RX_FATAL("out of memory");
    }
    for (size_t i = 0; i < n; i++) {
        list[i] = &stmts[i];
    }
    int rc = exec_list(ctx, list, n);
    free(list);
    if (ctx->loop_ctl != RX_CTL_NONE) {
        ctx->loop_ctl = RX_CTL_NONE; /* LOOP/EXIT outside a loop ends the program */
    }
    return rc;
}

/* public wrapper used by SCAN to run a body per record */
int rx_exec_block(rx_ctx *ctx, rx_stmt **list, size_t n)
{
    return exec_list(ctx, list, n);
}

/* used by cmds_env.c to render values */
int rx_vm_display(rx_ctx *ctx, const rval *v, strbuf *sb)
{
    return rval_to_display(ctx, v, sb);
}
