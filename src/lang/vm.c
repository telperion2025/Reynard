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
    /* locate_expr is borrowed from the statement list (owned by the caller) */
    rx_memvar_clear(ctx);
}

int rx_out(rx_ctx *ctx, const char *s, size_t n)
{
    if (!ctx->out || !ctx->set.console) {
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

/* ============================ function registry ============================ */

typedef struct {
    const rx_funcdef *defs;
    size_t n;
} fgroup;

static fgroup groups[5];
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
    memset(err, 0, sizeof(*err));
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
    memset(err, 0, sizeof(*err));
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
            if (fid < 0) {
                rc = vm_fail(ctx, err, R_ERR_UNDEF_FUNC, 0, 0,
                             ferr.msg[0] ? ferr.msg : "Undefined function");
                goto done;
            }
            rval res;
            rx_rval_undef(&res);
            rc = rx_func_call(ctx, fid, argc, &stack[sp - argc], &res, err);
            if (rc != R_OK) {
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

int rx_exec(rx_ctx *ctx, rx_stmt *stmts, size_t n)
{
    for (size_t i = 0; i < n && !ctx->abort; i++) {
        rx_stmt *s = &stmts[i];
        int rc = R_OK;
        switch (s->kind) {
        case ST_EMPTY:
            break;
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
        if (rc != R_OK) {
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

/* used by cmds_env.c to render values */
int rx_vm_display(rx_ctx *ctx, const rval *v, strbuf *sb)
{
    return rval_to_display(ctx, v, sb);
}
