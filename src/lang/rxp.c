/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file rxp.c
 * @brief .RXP compiled-program cache (spec §7.4) and rx_run_file.
 *
 * Format "RXP1": magic(4) | version u32 | sha256[32] of the raw .prg source |
 * u64 stmt count | serialized statement list. All integers little-endian,
 * fixed-width, so the same source always produces byte-identical .RXP files
 * (the M5 byte-reproducibility gate). The serialized AST is the
 * post-macro-expansion tree; macro expansion therefore happens at compile
 * time for cached programs (recorded in docs/DEVIATIONS.md).
 */
#define _POSIX_C_SOURCE 200809L

/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lang/lang.h"
#include "util/util.h"

#define RXP_MAGIC "RXP1"
#define RXP_VERSION 1u

/* ============================ writer ============================ */

static void w8(strbuf *b, uint8_t v)
{
    sb_putc(b, (char)v);
}

static void w16(strbuf *b, uint16_t v)
{
    w8(b, (uint8_t)(v & 0xff));
    w8(b, (uint8_t)(v >> 8));
}

static void w32(strbuf *b, uint32_t v)
{
    w16(b, (uint16_t)(v & 0xffff));
    w16(b, (uint16_t)(v >> 16));
}

static void w64(strbuf *b, uint64_t v)
{
    w32(b, (uint32_t)(v & 0xffffffffu));
    w32(b, (uint32_t)(v >> 32));
}

static void wi32(strbuf *b, int32_t v)
{
    w32(b, (uint32_t)v);
}

static void wd(strbuf *b, double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    w64(b, u);
}

static void wbytes(strbuf *b, const void *p, size_t n)
{
    w64(b, (uint64_t)n);
    if (n && p) {
        sb_put(b, p, n);
    }
}

static void wstr(strbuf *b, const char *s)
{
    if (!s) {
        w64(b, UINT64_MAX);
        return;
    }
    size_t n = strlen(s);
    w64(b, (uint64_t)n);
    if (n) {
        sb_put(b, s, n);
    }
}

static void w_rval(strbuf *b, const rval *v)
{
    w32(b, (uint32_t)v->t);
    switch (v->t) {
    case RX_T_U:
        break;
    case RX_T_C:
        wbytes(b, v->u.c.utf8 ? v->u.c.utf8 : "", v->u.c.utf8 ? v->u.c.n : 0);
        break;
    case RX_T_N:
        wd(b, v->u.n.d);
        wi32(b, v->u.n.dec);
        break;
    case RX_T_I:
        wi32(b, v->u.i);
        break;
    case RX_T_Y:
        w64(b, (uint64_t)v->u.y);
        break;
    case RX_T_B:
        wd(b, v->u.b);
        break;
    case RX_T_D:
        w16(b, (uint16_t)v->u.d.y);
        w16(b, (uint16_t)v->u.d.m);
        w16(b, (uint16_t)v->u.d.d);
        break;
    case RX_T_T:
        w16(b, (uint16_t)v->u.tm.d.y);
        w16(b, (uint16_t)v->u.tm.d.m);
        w16(b, (uint16_t)v->u.tm.d.d);
        wi32(b, v->u.tm.ms);
        break;
    case RX_T_L:
        w32(b, (uint32_t)v->u.l);
        break;
    case RX_T_M:
        w32(b, v->u.memo.blk);
        w8(b, 0); /* loaded memo text is runtime state, not serialized */
        break;
    case RX_T_G:
        wbytes(b, v->u.g.bytes, v->u.g.n);
        break;
    }
}

static void w_expr(strbuf *b, const rx_expr *e);

static void w_stmt(strbuf *b, const rx_stmt *s)
{
    w32(b, (uint32_t)s->kind);
    wi32(b, s->line);
    wi32(b, s->col);
    wstr(b, s->target);
    w64(b, (uint64_t)s->nstrs);
    for (size_t i = 0; i < s->nstrs; i++) {
        wstr(b, s->strs[i]);
    }
    w64(b, (uint64_t)s->nclause);
    for (size_t i = 0; i < s->nclause; i++) {
        wstr(b, s->clauses[i].key);
        wstr(b, s->clauses[i].word);
        w_expr(b, s->clauses[i].e);
    }
    w64(b, (uint64_t)s->nexpr);
    for (size_t i = 0; i < s->nexpr; i++) {
        w_expr(b, s->exprs[i]);
    }
    w64(b, (uint64_t)s->ncond);
    w64(b, (uint64_t)s->nidx);
    wstr(b, s->raw);
    w64(b, (uint64_t)s->nbcols);
    for (size_t i = 0; i < s->nbcols; i++) {
        const rx_bcol *c = &s->bcols[i];
        wstr(b, c->name);
        w_expr(b, c->e);
        wstr(b, c->header);
        wi32(b, c->width);
        w_expr(b, c->valid);
        wstr(b, c->verr);
        wi32(b, c->rflag);
        wi32(b, c->fflag);
        wi32(b, c->frozen);
        wi32(b, c->is_field);
        w64(b, (uint64_t)c->fldidx);
    }
    w64(b, (uint64_t)s->nkids);
    for (size_t i = 0; i < s->nkids; i++) {
        w_stmt(b, s->kids[i]);
    }
}

static void w_expr(strbuf *b, const rx_expr *e)
{
    if (!e) {
        w8(b, 0);
        return;
    }
    w8(b, 1);
    w32(b, (uint32_t)e->kind);
    wi32(b, e->line);
    wi32(b, e->col);
    wi32(b, e->op);
    wi32(b, e->fid);
    wstr(b, e->name);
    w_rval(b, &e->lit);
    w_expr(b, e->l);
    w_expr(b, e->r);
    w64(b, (uint64_t)e->nargs);
    for (size_t i = 0; i < e->nargs; i++) {
        w_expr(b, e->args[i]);
    }
}

/* ============================ reader ============================ */

typedef struct {
    const uint8_t *p, *end;
    int bad;
} rd;

static uint8_t r8(rd *r)
{
    if (r->bad || r->end - r->p < 1) {
        r->bad = 1;
        return 0;
    }
    return *r->p++;
}

static uint16_t r16(rd *r)
{
    uint16_t lo = r8(r), hi = r8(r);
    return (uint16_t)(lo | (hi << 8));
}

static uint32_t r32(rd *r)
{
    uint32_t lo = r16(r), hi = r16(r);
    return lo | (hi << 16);
}

static uint64_t r64(rd *r)
{
    uint64_t lo = r32(r), hi = r32(r);
    return lo | (hi << 32);
}

static double rdbl(rd *r)
{
    uint64_t u = r64(r);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static void rbytes(rd *r, void *dst, size_t n)
{
    if (r->bad) {
        return;
    }
    if ((size_t)(r->end - r->p) < n) {
        r->bad = 1;
        return;
    }
    if (n && dst) {
        memcpy(dst, r->p, n);
    }
    r->p += n;
}

static char *rstr(rd *r)
{
    uint64_t n = r64(r);
    if (r->bad) {
        return NULL;
    }
    if (n == UINT64_MAX) {
        return NULL;
    }
    if (n > (1u << 24) || (size_t)(r->end - r->p) < n) {
        r->bad = 1;
        return NULL;
    }
    char *s = malloc((size_t)n + 1);
    if (!s) {
        RX_FATAL("out of memory");
    }
    rbytes(r, s, (size_t)n);
    s[n] = '\0';
    return s;
}

static int r_rval(rd *r, rval *v)
{
    rx_rval_undef(v);
    v->t = (rx_type)r32(r);
    switch (v->t) {
    case RX_T_U:
        break;
    case RX_T_C: {
        uint64_t n = r64(r);
        if (r->bad || n > (1u << 24) || (size_t)(r->end - r->p) < n) {
            r->bad = 1;
            return R_ERR_DBF_BADRXP;
        }
        v->u.c.n = (size_t)n;
        v->u.c.utf8 = malloc((size_t)n + 1);
        if (!v->u.c.utf8) {
            RX_FATAL("out of memory");
        }
        rbytes(r, v->u.c.utf8, (size_t)n);
        v->u.c.utf8[n] = '\0';
        break;
    }
    case RX_T_N:
        v->u.n.d = rdbl(r);
        v->u.n.dec = (int32_t)r32(r);
        break;
    case RX_T_I:
        v->u.i = (int32_t)r32(r);
        break;
    case RX_T_Y:
        v->u.y = (int64_t)r64(r);
        break;
    case RX_T_B:
        v->u.b = rdbl(r);
        break;
    case RX_T_D:
        v->u.d.y = (int16_t)r16(r);
        v->u.d.m = (int16_t)r16(r);
        v->u.d.d = (int16_t)r16(r);
        break;
    case RX_T_T:
        v->u.tm.d.y = (int16_t)r16(r);
        v->u.tm.d.m = (int16_t)r16(r);
        v->u.tm.d.d = (int16_t)r16(r);
        v->u.tm.ms = (int32_t)r32(r);
        break;
    case RX_T_L:
        v->u.l = (rx_logic)r32(r);
        break;
    case RX_T_M:
        v->u.memo.blk = r32(r);
        (void)r8(r);
        v->u.memo.loaded = NULL;
        break;
    case RX_T_G: {
        uint64_t n = r64(r);
        if (r->bad || n > (1u << 24) || (size_t)(r->end - r->p) < n) {
            r->bad = 1;
            return R_ERR_DBF_BADRXP;
        }
        v->u.g.n = (size_t)n;
        v->u.g.bytes = malloc((size_t)n ? (size_t)n : 1);
        if (!v->u.g.bytes) {
            RX_FATAL("out of memory");
        }
        rbytes(r, v->u.g.bytes, (size_t)n);
        break;
    }
    default:
        r->bad = 1;
        return R_ERR_DBF_BADRXP;
    }
    return r->bad ? R_ERR_DBF_BADRXP : R_OK;
}

static int r_expr(rd *r, rx_expr **out);

static int r_stmt(rd *r, rx_stmt *s)
{
    memset(s, 0, sizeof(*s));
    s->kind = (st_kind)r32(r);
    s->line = (int32_t)r32(r);
    s->col = (int32_t)r32(r);
    char *t = rstr(r);
    if (r->bad) {
        free(t);
        return R_ERR_DBF_BADRXP;
    }
    if (t) {
        snprintf(s->target, sizeof(s->target), "%s", t);
        free(t);
    }
    uint64_t n = r64(r);
    if (r->bad || n > (1u << 20)) {
        return R_ERR_DBF_BADRXP;
    }
    s->nstrs = (size_t)n;
    if (n) {
        s->strs = calloc(n, sizeof(char *));
        if (!s->strs) {
            RX_FATAL("out of memory");
        }
        for (size_t i = 0; i < s->nstrs; i++) {
            s->strs[i] = rstr(r);
        }
    }
    n = r64(r);
    if (r->bad || n > (1u << 20)) {
        return R_ERR_DBF_BADRXP;
    }
    s->nclause = (size_t)n;
    if (n) {
        s->clauses = calloc(n, sizeof(rx_clause));
        if (!s->clauses) {
            RX_FATAL("out of memory");
        }
        for (size_t i = 0; i < s->nclause; i++) {
            char *k = rstr(r);
            char *w = rstr(r);
            if (k) {
                snprintf(s->clauses[i].key, sizeof(s->clauses[i].key), "%s", k);
                free(k);
            }
            if (w) {
                snprintf(s->clauses[i].word, sizeof(s->clauses[i].word), "%s", w);
                free(w);
            }
            if (r_expr(r, &s->clauses[i].e) != R_OK) {
                return R_ERR_DBF_BADRXP;
            }
        }
    }
    n = r64(r);
    if (r->bad || n > (1u << 20)) {
        return R_ERR_DBF_BADRXP;
    }
    s->nexpr = (size_t)n;
    if (n) {
        s->exprs = calloc(n, sizeof(rx_expr *));
        if (!s->exprs) {
            RX_FATAL("out of memory");
        }
        for (size_t i = 0; i < s->nexpr; i++) {
            if (r_expr(r, &s->exprs[i]) != R_OK) {
                return R_ERR_DBF_BADRXP;
            }
        }
    }
    s->ncond = (size_t)r64(r);
    s->nidx = (size_t)r64(r);
    s->raw = rstr(r);
    n = r64(r);
    if (r->bad || n > (1u << 20)) {
        return R_ERR_DBF_BADRXP;
    }
    s->nbcols = (size_t)n;
    if (n) {
        s->bcols = calloc(n, sizeof(rx_bcol));
        if (!s->bcols) {
            RX_FATAL("out of memory");
        }
        for (size_t i = 0; i < s->nbcols; i++) {
            rx_bcol *c = &s->bcols[i];
            char *a = rstr(r);
            if (a) {
                snprintf(c->name, sizeof(c->name), "%s", a);
                free(a);
            }
            if (r_expr(r, &c->e) != R_OK) {
                return R_ERR_DBF_BADRXP;
            }
            a = rstr(r);
            if (a) {
                snprintf(c->header, sizeof(c->header), "%s", a);
                free(a);
            }
            c->width = (int32_t)r32(r);
            if (r_expr(r, &c->valid) != R_OK) {
                return R_ERR_DBF_BADRXP;
            }
            a = rstr(r);
            if (a) {
                snprintf(c->verr, sizeof(c->verr), "%s", a);
                free(a);
            }
            c->rflag = (int32_t)r32(r);
            c->fflag = (int32_t)r32(r);
            c->frozen = (int32_t)r32(r);
            c->is_field = (int32_t)r32(r);
            c->fldidx = (size_t)r64(r);
        }
    }
    n = r64(r);
    if (r->bad || n > (1u << 20)) {
        return R_ERR_DBF_BADRXP;
    }
    s->nkids = (size_t)n;
    if (n) {
        s->kids = calloc(n, sizeof(rx_stmt *));
        if (!s->kids) {
            RX_FATAL("out of memory");
        }
        for (size_t i = 0; i < s->nkids; i++) {
            s->kids[i] = calloc(1, sizeof(rx_stmt));
            if (!s->kids[i]) {
                RX_FATAL("out of memory");
            }
            if (r_stmt(r, s->kids[i]) != R_OK) {
                return R_ERR_DBF_BADRXP;
            }
        }
    }
    return r->bad ? R_ERR_DBF_BADRXP : R_OK;
}

static int r_expr(rd *r, rx_expr **out)
{
    *out = NULL;
    if (r8(r) == 0) {
        return r->bad ? R_ERR_DBF_BADRXP : R_OK;
    }
    rx_expr *e = calloc(1, sizeof(rx_expr));
    if (!e) {
        RX_FATAL("out of memory");
    }
    e->kind = (ex_kind)r32(r);
    e->line = (int32_t)r32(r);
    e->col = (int32_t)r32(r);
    e->op = (int32_t)r32(r);
    e->fid = (int32_t)r32(r);
    char *nm = rstr(r);
    if (r->bad) {
        free(nm);
        free(e);
        return R_ERR_DBF_BADRXP;
    }
    if (nm) {
        snprintf(e->name, sizeof(e->name), "%s", nm);
        free(nm);
    }
    if (r_rval(r, &e->lit) != R_OK) {
        free(e);
        return R_ERR_DBF_BADRXP;
    }
    if (r_expr(r, &e->l) != R_OK || r_expr(r, &e->r) != R_OK) {
        rx_expr_free(e);
        return R_ERR_DBF_BADRXP;
    }
    uint64_t n = r64(r);
    if (r->bad || n > (1u << 20)) {
        rx_expr_free(e);
        return R_ERR_DBF_BADRXP;
    }
    e->nargs = (size_t)n;
    if (n) {
        e->args = calloc(n, sizeof(rx_expr *));
        if (!e->args) {
            RX_FATAL("out of memory");
        }
        for (size_t i = 0; i < e->nargs; i++) {
            if (r_expr(r, &e->args[i]) != R_OK) {
                rx_expr_free(e);
                return R_ERR_DBF_BADRXP;
            }
        }
    }
    *out = e;
    return r->bad ? R_ERR_DBF_BADRXP : R_OK;
}

/* ============================ cache file I/O ============================ */

static int rxp_save(const char *path, const uint8_t hash[32], const rx_stmt *stmts, size_t n)
{
    strbuf b;
    sb_init(&b);
    sb_put(&b, RXP_MAGIC, 4);
    w32(&b, RXP_VERSION);
    sb_put(&b, (const char *)hash, 32);
    w64(&b, (uint64_t)n);
    for (size_t i = 0; i < n; i++) {
        w_stmt(&b, &stmts[i]);
    }
    if (!b.buf) {
        sb_putc(&b, '\0');
        b.len = 0;
    }
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        sb_free(&b);
        return R_ERR_IO_OPEN;
    }
    int rc = R_OK;
    if (fwrite(b.buf, 1, b.len, f) != b.len) {
        rc = R_ERR_IO_WRITE;
    }
    if (fclose(f) != 0 && rc == R_OK) {
        rc = R_ERR_IO_CLOSE;
    }
    sb_free(&b);
    if (rc != R_OK) {
        unlink(tmp);
        return rc;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return R_ERR_IO_WRITE;
    }
    return R_OK;
}

static int rxp_load(const char *path, const uint8_t hash[32], rx_stmt **out, size_t *nout)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return R_ERR_IO_NOTFOUND;
    }
    strbuf b;
    sb_init(&b);
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        sb_put(&b, buf, got);
    }
    fclose(f);
    if (!b.buf) {
        return R_ERR_IO_READ;
    }
    rd r = { (const uint8_t *)b.buf, (const uint8_t *)b.buf + b.len, 0 };
    if (b.len < 4 + 4 + 32 || memcmp(b.buf, RXP_MAGIC, 4) != 0 || r32(&r) != RXP_VERSION ||
        memcmp(r.p, hash, 32) != 0) {
        sb_free(&b);
        return R_ERR_DBF_BADRXP;
    }
    r.p += 32;
    uint64_t n = r64(&r);
    if (r.bad || n > (1u << 20)) {
        sb_free(&b);
        return R_ERR_DBF_BADRXP;
    }
    rx_stmt *stmts = calloc(n ? n : 1, sizeof(rx_stmt));
    if (!stmts) {
        RX_FATAL("out of memory");
    }
    for (size_t i = 0; i < (size_t)n; i++) {
        if (r_stmt(&r, &stmts[i]) != R_OK) {
            for (size_t j = 0; j <= i; j++) {
                rx_stmt_free(&stmts[j]);
            }
            free(stmts);
            sb_free(&b);
            return R_ERR_DBF_BADRXP;
        }
    }
    sb_free(&b);
    if (r.bad || r.p != r.end) {
        for (size_t i = 0; i < (size_t)n; i++) {
            rx_stmt_free(&stmts[i]);
        }
        free(stmts);
        return R_ERR_DBF_BADRXP;
    }
    *out = stmts;
    *nout = (size_t)n;
    return R_OK;
}

/* ============================ paths ============================ */

static void cache_path_for(const char *path, char *out, size_t outsz)
{
    size_t l = strlen(path);
    if (l >= 4 && rx_strcasecmp(path + l - 4, ".prg") == 0) {
        snprintf(out, outsz, "%.*s.rxp", (int)(l - 4), path);
    } else {
        snprintf(out, outsz, "%s.rxp", path);
    }
}

static void fallback_cache_path(const uint8_t hash[32], char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    char base[512];
    if (xdg && *xdg) {
        snprintf(base, sizeof(base), "%s/reynard/rxp", xdg);
    } else {
        const char *home = getenv("HOME");
        if (home && *home) {
            snprintf(base, sizeof(base), "%s/.cache/reynard/rxp", home);
        } else {
            base[0] = '\0';
        }
    }
    if (!base[0]) {
        out[0] = '\0';
        return;
    }
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    snprintf(out, outsz, "%s/%s.rxp", base, hex);
}

static int ensure_dir(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return mkdir(tmp, 0777) == 0 || errno == EEXIST ? R_OK : R_ERR_IO_OPEN;
}

static char *read_file(const char *path, size_t *outn)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    strbuf b;
    sb_init(&b);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        sb_put(&b, buf, n);
    }
    fclose(f);
    if (!b.buf) {
        sb_putc(&b, '\0');
        b.len = 0;
    }
    *outn = b.len;
    return b.buf;
}

/* ============================ rx_run_file ============================ */

int rx_run_file(rx_ctx *ctx, const char *path)
{
    size_t n;
    char *src = read_file(path, &n);
    if (!src) {
        snprintf(ctx->prog, sizeof(ctx->prog), "%s", path);
        return rx_raise(ctx, R_ERR_IO_NOTFOUND, 0, 0, "Cannot open %s", path);
    }
    uint8_t hash[32];
    rx_sha256(src, n, hash);
    char cache[600];
    cache_path_for(path, cache, sizeof(cache));
    char fb[600];
    fallback_cache_path(hash, fb, sizeof(fb));

    snprintf(ctx->prog, sizeof(ctx->prog), "%s", path);
    rx_stmt *stmts = NULL;
    size_t nstmt = 0;
    int rc;
    if (rxp_load(cache, hash, &stmts, &nstmt) == R_OK ||
        (fb[0] && rxp_load(fb, hash, &stmts, &nstmt) == R_OK)) {
        ctx->src = src;
        rc = rx_exec(ctx, stmts, nstmt);
        if (rc != R_OK && !ctx->err_printed) {
            char msg[512];
            snprintf(msg, sizeof(msg), "%s", ctx->err.msg[0] ? ctx->err.msg : rx_status_str(rc));
            rx_raise(ctx, rc, ctx->err.line, ctx->err.col, "%s", msg);
        }
        for (size_t i = 0; i < nstmt; i++) {
            rx_stmt_free(&stmts[i]);
        }
        free(stmts);
        free(src);
        ctx->src = NULL;
        return rc;
    }

    /* cache miss: lex, parse, save, execute. & macros expand at execution
       time per statement (raw spans), not at compile time */
    r_err err;
    memset(&err, 0, sizeof(err));
    ctx->src = src;
    rx_tok *toks;
    size_t ntok;
    if (rx_lex(src, path, &toks, &ntok, &err) != R_OK) {
        rx_raise(ctx, err.code, err.line, err.col, "%s", err.msg);
        free(src);
        return err.code;
    }
    if (rx_parse_src(toks, ntok, src, &stmts, &nstmt, &err) != R_OK) {
        rx_raise(ctx, err.code, err.line, err.col, "%s", err.msg);
        rx_lex_free(toks, ntok);
        free(src);
        return err.code;
    }
    if (rxp_save(cache, hash, stmts, nstmt) != R_OK && fb[0]) {
        char dir[600];
        snprintf(dir, sizeof(dir), "%s", fb);
        char *sl = strrchr(dir, '/');
        if (sl) {
            *sl = '\0';
            if (ensure_dir(dir) == R_OK) {
                rxp_save(fb, hash, stmts, nstmt);
            }
        }
    }
    rc = rx_exec(ctx, stmts, nstmt);
    if (rc != R_OK && !ctx->err_printed) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s", ctx->err.msg[0] ? ctx->err.msg : rx_status_str(rc));
        rx_raise(ctx, rc, ctx->err.line, ctx->err.col, "%s", msg);
    }
    for (size_t i = 0; i < nstmt; i++) {
        rx_stmt_free(&stmts[i]);
    }
    free(stmts);
    rx_lex_free(toks, ntok);
    free(src);
    ctx->src = NULL;
    return rc;
}
