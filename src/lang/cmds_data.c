/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_data.c
 * @brief Data commands (spec §7.1): USE, CLOSE, SELECT, LIST, DISPLAY,
 *        GO/GOTO, SKIP with FoxPro scope semantics (spec §8.7).
 */
/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "lang/lang.h"
#include "reynard/dbf.h"
#include "lang/funcs.h"
#include "util/util.h"

/* resolve a table name per SET DEFAULT / SET PATH (spec §10.3) */
static int resolve_table(rx_ctx *ctx, const char *name, char *out, size_t outsz)
{
    char cand[600];
    /* 1: as given (relative to cwd) */
    snprintf(cand, sizeof(cand), "%s", name);
    if (access(cand, F_OK) == 0) {
        snprintf(out, outsz, "%s", cand);
        return R_OK;
    }
    /* 2: with .dbf appended */
    snprintf(cand, sizeof(cand), "%s.dbf", name);
    if (access(cand, F_OK) == 0) {
        snprintf(out, outsz, "%s", cand);
        return R_OK;
    }
    /* 3: default dir */
    if (ctx->set.defdir[0]) {
        snprintf(cand, sizeof(cand), "%s/%s.dbf", ctx->set.defdir, name);
        if (access(cand, F_OK) == 0) {
            snprintf(out, outsz, "%s", cand);
            return R_OK;
        }
    }
    /* 4: SET PATH list, comma separated */
    if (ctx->set.path[0]) {
        char pathcopy[1024];
        snprintf(pathcopy, sizeof(pathcopy), "%s", ctx->set.path);
        char *save = NULL;
        for (char *dir = strtok_r(pathcopy, ",;", &save); dir; dir = strtok_r(NULL, ",;", &save)) {
            snprintf(cand, sizeof(cand), "%s/%s.dbf", dir, name);
            if (access(cand, F_OK) == 0) {
                snprintf(out, outsz, "%s", cand);
                return R_OK;
            }
        }
    }
    return R_ERR_IO_NOTFOUND;
}

int rx_cmd_use(rx_ctx *ctx, rx_stmt *s)
{
    int area = 0; /* 0 = current */
    const char *alias = NULL;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "IN") == 0) {
            if (s->clauses[i].e) {
                rval v;
                rx_rval_undef(&v);
                int rc = rx_eval(ctx, s->clauses[i].e, &v, &ctx->err);
                if (rc != R_OK) {
                    return rc;
                }
                double d;
                if (rv_to_num(&v, &d)) {
                    area = (int)d;
                } else {
                    const char *a;
                    size_t n;
                    if (rv_to_str(&v, &a, &n)) {
                        for (int k = 1; k <= 255; k++) {
                            if (rx_wa_used(k)) {
                                const char *ka;
                                rx_wa_alias(k, &ka);
                                if (strlen(ka) == n && memcmp(ka, a, n) == 0) {
                                    area = k;
                                    break;
                                }
                            }
                        }
                    }
                }
                rx_rval_free(&v);
            } else {
                area = -2; /* alias by word: find or allocate */
                /* store word temporarily in alias handling below */
            }
            if (s->clauses[i].word[0]) {
                const char *w = s->clauses[i].word;
                int numeric = w[0] >= '0' && w[0] <= '9';
                for (const char *q = w; *q; q++) {
                    if (*q < '0' || *q > '9') {
                        numeric = 0;
                        break;
                    }
                }
                if (numeric) {
                    area = atoi(w);
                } else {
                int found = 0;
                for (int k = 1; k <= 255; k++) {
                    if (rx_wa_used(k)) {
                        const char *ka;
                        rx_wa_alias(k, &ka);
                        if (rx_strcasecmp(ka, w) == 0) {
                            area = k;
                            found = 1;
                            break;
                        }
                    }
                }
                if (!found) {
                    for (int k = 1; k <= 255; k++) {
                        if (!rx_wa_used(k)) {
                            area = k;
                            break;
                        }
                    }
                }
                }
            }
        } else if (strcmp(s->clauses[i].key, "ALIAS") == 0) {
            alias = s->clauses[i].word;
        }
    }
    if (s->nexpr == 0) {
        /* USE with no argument: close current work area */
        return rx_wa_close(0);
    }
    rval v;
    rx_rval_undef(&v);
    int rc = rx_eval(ctx, s->exprs[0], &v, &ctx->err);
    if (rc != R_OK) {
        return rc;
    }
    const char *nm;
    size_t nn;
    if (!rv_to_str(&v, &nm, &nn)) {
        rx_rval_free(&v);
        return rx_raise(ctx, R_ERR_TYPE, s->line, s->col, "USE expects a table name");
    }
    char nmc[260];
    size_t k = nn < 259 ? nn : 259;
    memcpy(nmc, nm, k);
    nmc[k] = '\0';
    rx_rval_free(&v);
    char path[600];
    rc = resolve_table(ctx, nmc, path, sizeof(path));
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "File %s not found", nmc);
    }
    if (area == 0 || area == -2) {
        /* pick lowest free area unless current is free */
        if (!rx_wa_used(0)) {
            area = rx_wa_current();
        } else {
            for (int i = 1; i <= 255; i++) {
                if (!rx_wa_used(i)) {
                    area = i;
                    break;
                }
            }
        }
    }
    return rx_wa_open(area, path, RD_SHARED, alias);
}

int rx_cmd_close(rx_ctx *ctx, rx_stmt *s)
{
    (void)ctx;
    int all = 0;
    for (size_t i = 0; i < s->nstrs; i++) {
        if (rx_strcasecmp(s->strs[i], "ALL") == 0 || rx_strcasecmp(s->strs[i], "DATABASES") == 0 ||
            rx_strcasecmp(s->strs[i], "DATABASE") == 0) {
            all = 1;
        }
    }
    if (all) {
        for (int i = 1; i <= 255; i++) {
            rx_wa_close(i);
        }
        return R_OK;
    }
    return rx_wa_close(0);
}

int rx_cmd_select(rx_ctx *ctx, rx_stmt *s)
{
    rval v;
    rx_rval_undef(&v);
    int rc = rx_eval(ctx, s->exprs[0], &v, &ctx->err);
    if (rc != R_OK) {
        return rc;
    }
    double d;
    if (rv_to_num(&v, &d)) {
        int area = (int)d;
        rx_rval_free(&v);
        if (area < 0 || area > 255) {
            return rx_raise(ctx, R_ERR_RANGE, s->line, s->col, "SELECT area out of range");
        }
        rx_wa_select(area == 0 ? rx_wa_current() : area);
        return R_OK;
    }
    const char *a;
    size_t n;
    if (rv_to_str(&v, &a, &n)) {
        for (int i = 1; i <= 255; i++) {
            if (rx_wa_used(i)) {
                const char *ka;
                rx_wa_alias(i, &ka);
                if (strlen(ka) == n && memcmp(ka, a, n) == 0) {
                    rx_rval_free(&v);
                    rx_wa_select(i);
                    return R_OK;
                }
            }
        }
        rx_rval_free(&v);
        return rx_raise(ctx, R_ERR_FIELD, s->line, s->col, "Alias not found");
    }
    rx_rval_free(&v);
    return rx_raise(ctx, R_ERR_TYPE, s->line, s->col, "SELECT expects area number or alias");
}

/* scope evaluation */
typedef struct {
    int kind; /* 0 none, 1 ALL, 2 REST, 3 NEXT, 4 RECORD */
    uint32_t n;
} scope_t;

static int eval_scope(rx_ctx *ctx, rx_stmt *s, int is_display, scope_t *sc)
{
    memset(sc, 0, sizeof(*sc));
    sc->kind = is_display ? 3 : 1; /* DISPLAY defaults NEXT 1, LIST defaults ALL */
    sc->n = 1;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "SCOPE") == 0) {
            if (strcmp(s->clauses[i].word, "ALL") == 0) {
                sc->kind = 1;
            } else if (strcmp(s->clauses[i].word, "REST") == 0) {
                sc->kind = 2;
            } else if (strcmp(s->clauses[i].word, "NEXT") == 0) {
                sc->kind = 3;
                rval v;
                rx_rval_undef(&v);
                int rc = rx_eval(ctx, s->clauses[i].e, &v, &ctx->err);
                if (rc != R_OK) {
                    return rc;
                }
                double d = 0;
                rv_to_num(&v, &d);
                rx_rval_free(&v);
                sc->n = (uint32_t)(d < 0 ? 0 : d);
            } else if (strcmp(s->clauses[i].word, "RECORD") == 0) {
                sc->kind = 4;
                rval v;
                rx_rval_undef(&v);
                int rc = rx_eval(ctx, s->clauses[i].e, &v, &ctx->err);
                if (rc != R_OK) {
                    return rc;
                }
                double d = 0;
                rv_to_num(&v, &d);
                rx_rval_free(&v);
                sc->n = (uint32_t)(d < 1 ? 1 : d);
            }
        }
    }
    return R_OK;
}

static void print_record_line(rx_ctx *ctx, rx_dbf *dbf, uint32_t recno, size_t *widths,
                              size_t nf, int recw)
{
    strbuf sb;
    sb_init(&sb);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%*u", recw, recno);
    sb_puts(&sb, tmp);
    for (size_t f = 0; f < nf; f++) {
        sb_putc(&sb, ' ');
        rval v;
        rx_rval_undef(&v);
        if (rx_dbf_get(dbf, f, &v) != R_OK) {
            sb_putc(&sb, '?');
            continue;
        }
        rx_fielddef fd;
        rx_dbf_field_def(dbf, f, &fd);
        size_t w = widths[f];
        if (fd.type == RFT_C || fd.type == RFT_M) {
            const char *s = v.t == RX_T_C ? (v.u.c.utf8 ? v.u.c.utf8 : "") : "";
            size_t n = v.t == RX_T_C ? v.u.c.n : 0;
            sb_put(&sb, s, n);
            for (size_t i = n; i < w; i++) {
                sb_putc(&sb, ' ');
            }
        } else if (fd.type == RFT_N || fd.type == RFT_F) {
            char num[64];
            rx_format_num(num, sizeof(num), v.u.n.d, fd.dec, 1);
            size_t nl = strlen(num);
            for (size_t i = nl; i < w; i++) {
                sb_putc(&sb, ' ');
            }
            sb_puts(&sb, num);
        } else if (fd.type == RFT_D) {
            char d[48];
            if (v.u.d.y == 0) {
                snprintf(d, sizeof(d), "%s", ctx->set.century ? "  /  /    " : "  /  /  ");
            } else if (ctx->set.century) {
                snprintf(d, sizeof(d), "%02d/%02d/%04d", v.u.d.m, v.u.d.d, v.u.d.y);
            } else {
                snprintf(d, sizeof(d), "%02d/%02d/%02d", v.u.d.m, v.u.d.d, v.u.d.y % 100);
            }
            sb_puts(&sb, d);
        } else if (fd.type == RFT_L) {
            sb_putc(&sb, v.u.l == L_T ? 'T' : v.u.l == L_F ? 'F' : '?');
            for (size_t i = 1; i < w; i++) {
                sb_putc(&sb, ' ');
            }
        } else if (fd.type == RFT_I) {
            char num[64];
            snprintf(num, sizeof(num), "%d", v.u.i);
            size_t nl = strlen(num);
            for (size_t i = nl; i < w; i++) {
                sb_putc(&sb, ' ');
            }
            sb_puts(&sb, num);
        } else {
            strbuf tmp2;
            sb_init(&tmp2);
            rx_vm_display(ctx, &v, &tmp2);
            size_t n = tmp2.len;
            for (size_t i = n; i < w; i++) {
                sb_putc(&sb, ' ');
            }
            sb_put(&sb, tmp2.buf ? tmp2.buf : "", n);
            sb_free(&tmp2);
        }
        rx_rval_free(&v);
    }
    sb_putc(&sb, '\n');
    if (sb.buf) {
        rx_out(ctx, sb.buf, sb.len);
    }
    sb_free(&sb);
}

int rx_cmd_list(rx_ctx *ctx, rx_stmt *s, int display)
{
    rx_dbf *dbf;
    int rc = rx_wa_dbf(0, &dbf);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
    }
    size_t nf;
    rx_dbf_field_count(dbf, &nf);
    uint32_t nrec;
    rx_dbf_recount(dbf, &nrec);

    size_t *widths = calloc(nf, sizeof(size_t));
    if (!widths) {
        RX_FATAL("out of memory");
    }
    for (size_t f = 0; f < nf; f++) {
        rx_fielddef fd;
        rx_dbf_field_def(dbf, f, &fd);
        size_t w = strlen(fd.name);
        size_t tw = fd.len;
        if (fd.type == RFT_D) {
            tw = ctx->set.century ? 10 : 8;
        }
        if (fd.type == RFT_L) {
            tw = 1;
        }
        if (tw > w) {
            w = tw;
        }
        widths[f] = w;
    }
    int recw = 7; /* "Record#" */
    uint32_t t = nrec;
    int digits = 1;
    while (t >= 10) {
        t /= 10;
        digits++;
    }
    if (digits > recw) {
        recw = digits;
    }

    scope_t sc;
    rc = eval_scope(ctx, s, display, &sc);
    if (rc != R_OK) {
        free(widths);
        return rc;
    }

    uint32_t saved;
    rx_dbf_recno(dbf, &saved);

    if (ctx->set.headings) {
        strbuf sb;
        sb_init(&sb);
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%*s", recw, "Record#");
        sb_puts(&sb, tmp);
        for (size_t f = 0; f < nf; f++) {
            rx_fielddef fd;
            rx_dbf_field_def(dbf, f, &fd);
            sb_putc(&sb, ' ');
            sb_puts(&sb, fd.name);
            for (size_t i = strlen(fd.name); i < widths[f]; i++) {
                sb_putc(&sb, ' ');
            }
        }
        sb_putc(&sb, '\n');
        for (int i = 0; i < recw; i++) {
            sb_putc(&sb, '-');
        }
        for (size_t f = 0; f < nf; f++) {
            sb_putc(&sb, ' ');
            for (size_t i = 0; i < widths[f]; i++) {
                sb_putc(&sb, '-');
            }
        }
        sb_putc(&sb, '\n');
        if (sb.buf) {
            rx_out(ctx, sb.buf, sb.len);
        }
        sb_free(&sb);
    }

    int count = 0;
    if (sc.kind == 1) { /* ALL */
        rx_dbf_goto(dbf, 1);
        while (!ctx->abort) {
            int eof;
            rx_dbf_eof(dbf, &eof);
            if (eof) {
                break;
            }
            int del;
            rx_dbf_deleted(dbf, &del);
            if (!(ctx->set.deleted && del)) {
                uint32_t r;
                rx_dbf_recno(dbf, &r);
                print_record_line(ctx, dbf, r, widths, nf, recw);
            }
            rx_dbf_skip(dbf, 1);
        }
    } else if (sc.kind == 2) { /* REST */
        while (1) {
            int eof;
            rx_dbf_eof(dbf, &eof);
            if (eof) {
                break;
            }
            int del;
            rx_dbf_deleted(dbf, &del);
            if (!(ctx->set.deleted && del)) {
                uint32_t r;
                rx_dbf_recno(dbf, &r);
                print_record_line(ctx, dbf, r, widths, nf, recw);
            }
            rx_dbf_skip(dbf, 1);
        }
    } else if (sc.kind == 3) { /* NEXT n */
        for (uint32_t i = 0; i < sc.n; i++) {
            int eof;
            rx_dbf_eof(dbf, &eof);
            if (eof) {
                break;
            }
            int del;
            rx_dbf_deleted(dbf, &del);
            if (!(ctx->set.deleted && del)) {
                uint32_t r;
                rx_dbf_recno(dbf, &r);
                print_record_line(ctx, dbf, r, widths, nf, recw);
            }
            rx_dbf_skip(dbf, 1);
            count++;
        }
    } else { /* RECORD n */
        uint32_t cur;
        rx_dbf_recno(dbf, &cur);
        if (sc.n >= 1 && sc.n <= nrec) {
            rx_dbf_goto(dbf, sc.n);
            int del;
            rx_dbf_deleted(dbf, &del);
            if (!(ctx->set.deleted && del)) {
                print_record_line(ctx, dbf, sc.n, widths, nf, recw);
            }
            rx_dbf_goto(dbf, cur);
        }
    }
    (void)count;
    free(widths);
    /* RECORD n leaves the pointer where it was; ALL/REST/NEXT leave it at
       the position the iteration ended (FoxPro semantics, §8.7). */
    if (sc.kind == 4) {
        rx_dbf_goto(dbf, saved);
    }
    return R_OK;
}

int rx_cmd_go(rx_ctx *ctx, rx_stmt *s)
{
    rx_dbf *dbf;
    int rc = rx_wa_dbf(0, &dbf);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
    }
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "TARGET") == 0) {
            if (strcmp(s->clauses[i].word, "TOP") == 0) {
                uint32_t n;
                rx_dbf_recount(dbf, &n);
                return n ? rx_dbf_goto(dbf, 1) : rx_dbf_goto(dbf, 1);
            }
            if (strcmp(s->clauses[i].word, "BOTTOM") == 0) {
                uint32_t n;
                rx_dbf_recount(dbf, &n);
                return rx_dbf_goto(dbf, n ? n : 1);
            }
            rval v;
            rx_rval_undef(&v);
            rc = rx_eval(ctx, s->clauses[i].e, &v, &ctx->err);
            if (rc != R_OK) {
                return rc;
            }
            double d = 0;
            rv_to_num(&v, &d);
            rx_rval_free(&v);
            uint32_t n;
            rx_dbf_recount(dbf, &n);
            long target = (long)d;
            if (target < 1) {
                target = 1;
            }
            if (n && target > (long)n) {
                return rx_raise(ctx, R_ERR_DBF_RECNO, s->line, s->col,
                                "Record number out of range");
            }
            return rx_dbf_goto(dbf, (uint32_t)target);
        }
    }
    return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "GO requires TOP, BOTTOM or a record");
}

int rx_cmd_skip(rx_ctx *ctx, rx_stmt *s)
{
    rx_dbf *dbf;
    int rc = rx_wa_dbf(0, &dbf);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
    }
    int32_t delta = 1;
    if (s->nexpr > 0) {
        rval v;
        rx_rval_undef(&v);
        rc = rx_eval(ctx, s->exprs[0], &v, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        double d = 0;
        rv_to_num(&v, &d);
        rx_rval_free(&v);
        delta = (int32_t)d;
    }
    return rx_dbf_skip(dbf, delta);
}
