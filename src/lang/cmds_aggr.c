/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_aggr.c
 * @brief Aggregate and restructuring commands (spec §7.1, M7): COUNT, SUM,
 *        AVERAGE, CALCULATE, SORT TO, TOTAL TO, SCATTER, GATHER.
 *
 * All of them share the scope/FOR/WHILE iteration (rx_iterate from
 * cmds_data.c). SORT/TOTAL encode keys with the public rx_key_encode so their
 * ordering matches the index engine exactly.
 */
/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "lang/funcs.h"
#include "lang/lang.h"
#include "reynard/dbf.h"
#include "util/util.h"

/* ---------------- shared helpers ---------------- */

/* append an expr to a stmt (st_add_expr lives in parser.c) */
static void aggr_add_expr(rx_stmt* s, rx_expr* e) {
    if (s->nexpr == s->capexpr) {
        size_t nc = s->capexpr ? s->capexpr * 2 : 8;
        rx_expr** ne = realloc(s->exprs, nc * sizeof(rx_expr*));
        if (!ne) {
            RX_FATAL("out of memory");
        }
        s->exprs = ne;
        s->capexpr = nc;
    }
    s->exprs[s->nexpr++] = e;
}

static int cur_table(rx_ctx* ctx, rx_stmt* s, rx_dbf** out) {
    int rc = rx_wa_dbf(0, out);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
    }
    return R_OK;
}

static int aggr_raise(rx_ctx* ctx, rx_stmt* s, int rc, const char* cmd) {
    return rx_raise(ctx, rc, s->line, s->col, "%s", cmd);
}

/* resolve the FIELDS clause list to field indices (no clause = all fields) */
static int field_list(rx_ctx* ctx, rx_dbf* dbf, rx_stmt* s, size_t** out, size_t* nout) {
    size_t nf = 0;
    rx_dbf_field_count(dbf, &nf);
    size_t* f = NULL;
    size_t n = 0;
    int have = 0;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "FIELD") == 0) {
            have = 1;
            size_t idx;
            if (rx_dbf_field_index(dbf, s->clauses[i].word, &idx) != R_OK) {
                free(f);
                return rx_raise(ctx, R_ERR_FIELD, 0, 0, "Unknown field %s", s->clauses[i].word);
            }
            f = realloc(f, (n + 1) * sizeof(size_t));
            if (!f) {
                RX_FATAL("out of memory");
            }
            f[n++] = idx;
        }
    }
    if (!have) {
        f = malloc(nf * sizeof(size_t));
        if (!f) {
            RX_FATAL("out of memory");
        }
        for (size_t i = 0; i < nf; i++) {
            f[i] = i;
        }
        n = nf;
    }
    *out = f;
    *nout = n;
    return R_OK;
}

/* clause lookup helpers */
static const char* clause_word(rx_stmt* s, const char* key, size_t idx) {
    size_t k = 0;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, key) == 0) {
            if (k == idx) {
                return s->clauses[i].word;
            }
            k++;
        }
    }
    return NULL;
}

static int has_clause(rx_stmt* s, const char* key) {
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, key) == 0) {
            return 1;
        }
    }
    return 0;
}

/* display an rval like ? does */
static void out_value(rx_ctx* ctx, const rval* v) {
    strbuf sb;
    sb_init(&sb);
    rx_vm_display(ctx, v, &sb);
    if (sb.buf) {
        rx_out(ctx, sb.buf, sb.len);
    }
    sb_free(&sb);
}

static void set_var(rx_ctx* ctx, const char* name, const rval* v) {
    rx_memvar_set(ctx, name, v); /* creates or overwrites */
}

static void set_array_el(rx_ctx* ctx, const char* name, size_t i, const rval* v) {
    rx_array_set(ctx, name, i, 1, v);
}

/* ---------------- COUNT ---------------- */

typedef struct {
    rx_ctx* ctx;
    uint32_t n;
} count_ud;

static int cb_count(rx_ctx* ctx, rx_dbf* dbf, uint32_t recno, void* ud) {
    (void)dbf;
    (void)recno;
    (void)ctx;
    (void)0;
    ((count_ud*)ud)->n++;
    return R_OK;
}

int rx_cmd_count(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    count_ud ud = {ctx, 0};
    int n = 0;
    rc = rx_iterate(ctx, s, dbf, 1 /* ALL (FoxPro COUNT default) */, cb_count, &ud, &n);
    if (rc != R_OK) {
        return aggr_raise(ctx, s, rc, "COUNT");
    }
    rval v;
    rx_rval_undef(&v);
    v.t = RX_T_N;
    v.u.n.d = (double)ud.n;
    v.u.n.dec = 0;
    const char* to = clause_word(s, "TOVAR", 0);
    if (to) {
        set_var(ctx, to, &v);
    } else if (ctx->set.talk) {
        out_value(ctx, &v);
        rx_out(ctx, " records\n", 9);
    }
    return R_OK;
}

/* ---------------- SUM / AVERAGE ---------------- */

typedef struct {
    rx_ctx* ctx;
    rx_stmt* s;
    int is_avg;
    double* sum; /* per expr */
    uint32_t n;
} sum_ud;

static int cb_sum(rx_ctx* ctx, rx_dbf* dbf, uint32_t recno, void* ud) {
    (void)dbf;
    (void)recno;
    sum_ud* u = ud;
    for (size_t i = 0; i < u->s->nexpr; i++) {
        rval v;
        rx_rval_undef(&v);
        int rc = rx_eval(ctx, u->s->exprs[i], &v, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        double d = 0;
        int ok = rv_to_num(&v, &d);
        rx_rval_free(&v);
        if (ok) {
            u->sum[i] += d;
        }
    }
    u->n++;
    return R_OK;
}

static int sum_avg(rx_ctx* ctx, rx_stmt* s, int is_avg) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    size_t ne = s->nexpr;
    if (ne == 0) {
        /* SUM with no expr list: sum every numeric field */
        size_t nf = 0;
        rx_dbf_field_count(dbf, &nf);
        for (size_t i = 0; i < nf; i++) {
            rx_fielddef fd;
            rx_dbf_field_def(dbf, i, &fd);
            if (fd.type == RFT_N || fd.type == RFT_F || fd.type == RFT_I) {
                /* build a field expr */
                rx_expr* e = NULL;
                r_err err;
                memset(&err, 0, sizeof(err));
                char src[16];
                snprintf(src, sizeof(src), "%s", fd.name);
                if (rx_parse_expr_str(src, &e, &err) == R_OK) {
                    aggr_add_expr(s, e);
                    ne++;
                }
            }
        }
    }
    if (ne == 0) {
        return aggr_raise(ctx, s, R_ERR_SYNTAX, is_avg ? "AVERAGE" : "SUM");
    }
    double* sum = calloc(ne, sizeof(double));
    if (!sum) {
        RX_FATAL("out of memory");
    }
    sum_ud ud = {ctx, s, is_avg, sum, 0};
    int n = 0;
    rc = rx_iterate(ctx, s, dbf, 1 /* ALL */, cb_sum, &ud, &n);
    if (rc != R_OK) {
        free(sum);
        return aggr_raise(ctx, s, rc, is_avg ? "AVERAGE" : "SUM");
    }
    const char* to = clause_word(s, "TOVAR", 0);
    const char* toarr = clause_word(s, "TOARRAY", 0);
    if (toarr) {
        rx_array_def(ctx, toarr, ne, 1); /* once: re-def would clear elements */
    }
    for (size_t i = 0; i < ne; i++) {
        rval v;
        rx_rval_undef(&v);
        v.t = RX_T_N;
        v.u.n.d = sum[i];
        v.u.n.dec = 2;
        if (is_avg && ud.n > 0) {
            v.u.n.d = sum[i] / (double)ud.n;
        }
        if (toarr) {
            set_array_el(ctx, toarr, i + 1, &v);
        } else if (to) {
            set_var(ctx, to, &v);
        } else if (ctx->set.talk) {
            out_value(ctx, &v);
            rx_out(ctx, "\n", 1);
        }
    }
    free(sum);
    return R_OK;
}

int rx_cmd_sum(rx_ctx* ctx, rx_stmt* s) {
    return sum_avg(ctx, s, 0);
}
int rx_cmd_average(rx_ctx* ctx, rx_stmt* s) {
    return sum_avg(ctx, s, 1);
}

/* ---------------- CALCULATE ---------------- */

typedef struct {
    rx_ctx* ctx;
    rx_stmt* s;
    double* acc;   /* per calc clause: running sum / min / max */
    uint32_t n;    /* records seen (for AVG) */
    uint32_t nnum; /* numeric values seen (for MIN/MAX) */
} calc_ud;

static int cb_calc(rx_ctx* ctx, rx_dbf* dbf, uint32_t recno, void* ud) {
    (void)dbf;
    (void)recno;
    calc_ud* u = ud;
    for (size_t i = 0; i < u->s->nclause; i++) {
        rx_clause* c = &u->s->clauses[i];
        if (strcmp(c->key, "CALC") != 0) {
            continue;
        }
        const char* fn = c->word;
        if (rx_strcasecmp(fn, "CNT") == 0) {
            u->acc[i] += 1;
            continue;
        }
        rval v;
        rx_rval_undef(&v);
        int rc = rx_eval(ctx, c->e, &v, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        double d = 0;
        int ok = rv_to_num(&v, &d);
        rx_rval_free(&v);
        if (!ok) {
            continue;
        }
        if (rx_strcasecmp(fn, "SUM") == 0) {
            u->acc[i] += d;
        } else if (rx_strcasecmp(fn, "AVG") == 0) {
            u->acc[i] += d;
            u->n++;
        } else if (rx_strcasecmp(fn, "MIN") == 0) {
            if (u->nnum == 0 || d < u->acc[i]) {
                u->acc[i] = d;
            }
            u->nnum++;
        } else if (rx_strcasecmp(fn, "MAX") == 0) {
            if (u->nnum == 0 || d > u->acc[i]) {
                u->acc[i] = d;
            }
            u->nnum++;
        }
    }
    return R_OK;
}

int rx_cmd_calculate(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    size_t nc = 0;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "CALC") == 0) {
            nc++;
        }
    }
    if (nc == 0) {
        return aggr_raise(ctx, s, R_ERR_SYNTAX, "CALCULATE");
    }
    double* acc = calloc(s->nclause ? s->nclause : 1, sizeof(double));
    if (!acc) {
        RX_FATAL("out of memory");
    }
    calc_ud ud = {ctx, s, acc, 0, 0};
    int n = 0;
    rc = rx_iterate(ctx, s, dbf, 1, cb_calc, &ud, &n);
    if (rc != R_OK) {
        free(acc);
        return aggr_raise(ctx, s, rc, "CALCULATE");
    }
    const char* to = clause_word(s, "TOVAR", 0);
    const char* toarr = clause_word(s, "TOARRAY", 0);
    if (toarr) {
        rx_array_def(ctx, toarr, nc, 1); /* once: re-def would clear elements */
    }
    size_t w = 0;
    for (size_t i = 0; i < s->nclause; i++) {
        rx_clause* c = &s->clauses[i];
        if (strcmp(c->key, "CALC") != 0) {
            continue;
        }
        rval v;
        rx_rval_undef(&v);
        v.t = RX_T_N;
        v.u.n.d = acc[i];
        v.u.n.dec = 2;
        if (rx_strcasecmp(c->word, "AVG") == 0 && ud.n > 0) {
            v.u.n.d = acc[i] / (double)ud.n;
        }
        if (toarr) {
            set_array_el(ctx, toarr, w + 1, &v);
        } else if (to) {
            set_var(ctx, to, &v);
        } else if (ctx->set.talk) {
            out_value(ctx, &v);
            rx_out(ctx, "\n", 1);
        }
        w++;
    }
    free(acc);
    return R_OK;
}

/* ---------------- SORT TO ---------------- */

typedef struct {
    uint8_t* keys; /* concat of (u16 len + key bytes) per sort field */
    size_t klen;
    uint32_t recno;
} sortrec;

typedef struct {
    rx_dbf* dbf;
    rx_stmt* s;
    size_t nkeys;
    int* desc; /* per key */
    sortrec* recs;
    size_t n, cap;
} sort_ud;

static void put_u16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static int sort_cmp(const void* a, const void* b, void* ud) {
    sort_ud* u = ud;
    const sortrec *ra = a, *rb = b;
    size_t oa = 0, ob = 0;
    for (size_t k = 0; k < u->nkeys; k++) {
        uint16_t la = ra->keys[oa] | (ra->keys[oa + 1] << 8);
        uint16_t lb = rb->keys[ob] | (rb->keys[ob + 1] << 8);
        size_t m = la < lb ? la : lb;
        int c = memcmp(ra->keys + oa + 2, rb->keys + ob + 2, m);
        if (c == 0) {
            c = la < lb ? -1 : la > lb ? 1 : 0;
        }
        if (u->desc[k]) {
            c = -c;
        }
        if (c != 0) {
            return c;
        }
        oa += 2 + la;
        ob += 2 + lb;
    }
    return ra->recno < rb->recno ? -1 : ra->recno > rb->recno ? 1 : 0;
}

/* qsort_r is GNU-only; the engine is single-threaded, use a file-scope ctx */
static sort_ud* g_sort_ud;
static int sort_cmp_thunk(const void* a, const void* b) {
    return sort_cmp(a, b, g_sort_ud);
}

static int cb_sort(rx_ctx* ctx, rx_dbf* dbf, uint32_t recno, void* ud) {
    (void)ctx;
    (void)dbf;
    sort_ud* u = ud;
    strbuf kb;
    sb_init(&kb);
    for (size_t k = 0; k < u->nkeys; k++) {
        const char* fname = NULL;
        size_t ci = 0;
        for (size_t i = 0; i < u->s->nclause; i++) {
            if (strcmp(u->s->clauses[i].key, "SORTFLD") == 0) {
                if (ci == k) {
                    fname = u->s->clauses[i].word;
                }
                ci++;
            }
        }
        size_t fi;
        if (!fname || rx_dbf_field_index(dbf, fname, &fi) != R_OK) {
            sb_free(&kb);
            return R_ERR_FIELD;
        }
        rval v;
        rx_rval_undef(&v);
        int rc = rx_dbf_get(dbf, fi, &v);
        if (rc != R_OK) {
            sb_free(&kb);
            return rc;
        }
        strbuf kb2;
        sb_init(&kb2);
        rx_key_encode(&v, &kb2);
        rx_rval_free(&v);
        uint8_t hdr[2];
        put_u16le(hdr, (uint16_t)kb2.len);
        sb_put(&kb, (const char*)hdr, 2);
        if (kb2.buf) {
            sb_put(&kb, kb2.buf, kb2.len);
        }
        sb_free(&kb2);
    }
    if (u->n == u->cap) {
        u->cap = u->cap ? u->cap * 2 : 256;
        u->recs = realloc(u->recs, u->cap * sizeof(sortrec));
        if (!u->recs) {
            RX_FATAL("out of memory");
        }
    }
    sortrec* r = &u->recs[u->n++];
    r->klen = kb.len;
    r->keys = malloc(kb.len ? kb.len : 1);
    if (!r->keys) {
        RX_FATAL("out of memory");
    }
    if (kb.buf) {
        memcpy(r->keys, kb.buf, kb.len);
    }
    r->recno = recno;
    sb_free(&kb);
    return R_OK;
}

int rx_cmd_sort(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    const char* to = clause_word(s, "TO", 0);
    if (!to) {
        return aggr_raise(ctx, s, R_ERR_SYNTAX, "SORT TO");
    }
    size_t nkeys = 0;
    int* desc = NULL;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "SORTFLD") == 0) {
            desc = realloc(desc, (nkeys + 1) * sizeof(int));
            desc[nkeys] = (s->clauses[i].text && strchr(s->clauses[i].text, 'D')) ||
                                  (s->clauses[i].text && strchr(s->clauses[i].text, 'd')) ||
                                  has_clause(s, "DESCENDING")
                              ? 1
                              : 0;
            nkeys++;
        }
    }
    if (nkeys == 0) {
        free(desc);
        return aggr_raise(ctx, s, R_ERR_SYNTAX, "SORT ON");
    }
    sort_ud ud = {dbf, s, nkeys, desc, NULL, 0, 0};
    int n = 0;
    rc = rx_iterate(ctx, s, dbf, 1, cb_sort, &ud, &n);
    if (rc != R_OK) {
        for (size_t i = 0; i < ud.n; i++) {
            free(ud.recs[i].keys);
        }
        free(ud.recs);
        free(desc);
        return aggr_raise(ctx, s, rc, "SORT TO");
    }
    g_sort_ud = &ud;
    qsort(ud.recs, ud.n, sizeof(sortrec), sort_cmp_thunk);
    /* target structure: FIELDS subset or full copy */
    size_t *flds = NULL, nf = 0;
    rc = field_list(ctx, dbf, s, &flds, &nf);
    if (rc != R_OK) {
        for (size_t i = 0; i < ud.n; i++) {
            free(ud.recs[i].keys);
        }
        free(ud.recs);
        free(desc);
        return rc;
    }
    char path[600];
    snprintf(path, sizeof(path), "%.599s", to);
    if (!strchr(strrchr(path, '/') ? strrchr(path, '/') : path, '.') && !strchr(to, '.')) {
        strncat(path, ".dbf", sizeof(path) - strlen(path) - 1);
    }
    rx_fielddef* defs = malloc(nf * sizeof(rx_fielddef));
    if (!defs) {
        RX_FATAL("out of memory");
    }
    for (size_t i = 0; i < nf; i++) {
        rx_dbf_field_def(dbf, flds[i], &defs[i]);
    }
    rx_dbf* dst = NULL;
    rc = rx_dbf_create(path, defs, nf, &dst);
    free(defs);
    if (rc != R_OK) {
        free(flds);
        for (size_t i = 0; i < ud.n; i++) {
            free(ud.recs[i].keys);
        }
        free(ud.recs);
        free(desc);
        return aggr_raise(ctx, s, rc, "SORT TO");
    }
    for (size_t i = 0; i < ud.n && rc == R_OK; i++) {
        rc = rx_dbf_goto(dbf, ud.recs[i].recno);
        if (rc != R_OK) {
            break;
        }
        uint32_t nr;
        rc = rx_dbf_append(dst, &nr);
        if (rc != R_OK) {
            break;
        }
        for (size_t j = 0; j < nf && rc == R_OK; j++) {
            rval v;
            rx_rval_undef(&v);
            rc = rx_dbf_get(dbf, flds[j], &v);
            if (rc == R_OK) {
                rc = rx_dbf_put(dst, j, &v);
            }
            rx_rval_free(&v);
        }
        if (rc == R_OK) {
            rc = rx_flush(dst);
        }
    }
    rx_dbf_close(dst);
    free(flds);
    for (size_t i = 0; i < ud.n; i++) {
        free(ud.recs[i].keys);
    }
    free(ud.recs);
    free(desc);
    if (rc != R_OK) {
        return aggr_raise(ctx, s, rc, "SORT TO");
    }
    if (ctx->set.talk) {
        rx_outf(ctx, "%u records sorted.\n", (unsigned)ud.n);
    }
    return R_OK;
}

/* ---------------- TOTAL TO ---------------- */

typedef struct {
    rx_ctx* ctx;
    rx_stmt* s;
    sortrec* recs;
    size_t n, cap;
} total_ud;

static int cb_total(rx_ctx* ctx, rx_dbf* dbf, uint32_t recno, void* ud) {
    (void)dbf;
    total_ud* u = ud;
    rval v;
    rx_rval_undef(&v);
    int rc = rx_eval(ctx, u->s->exprs[0], &v, &ctx->err);
    if (rc != R_OK) {
        return rc;
    }
    strbuf kb;
    sb_init(&kb);
    rx_key_encode(&v, &kb);
    rx_rval_free(&v);
    if (u->n == u->cap) {
        u->cap = u->cap ? u->cap * 2 : 256;
        u->recs = realloc(u->recs, u->cap * sizeof(sortrec));
        if (!u->recs) {
            RX_FATAL("out of memory");
        }
    }
    sortrec* r = &u->recs[u->n++];
    r->klen = kb.len;
    r->keys = malloc(kb.len ? kb.len : 1);
    if (!r->keys) {
        RX_FATAL("out of memory");
    }
    if (kb.buf) {
        memcpy(r->keys, kb.buf, kb.len);
    }
    r->recno = recno;
    sb_free(&kb);
    return R_OK;
}

static int total_keycmp(const void* a, const void* b) {
    const sortrec *ra = a, *rb = b;
    size_t m = ra->klen < rb->klen ? ra->klen : rb->klen;
    int c = memcmp(ra->keys, rb->keys, m);
    if (c == 0) {
        c = ra->klen < rb->klen ? -1 : ra->klen > rb->klen ? 1 : 0;
    }
    return c;
}

int rx_cmd_total(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    const char* to = clause_word(s, "TO", 0);
    if (!to || s->nexpr < 1) {
        return aggr_raise(ctx, s, R_ERR_SYNTAX, "TOTAL TO");
    }
    total_ud ud = {ctx, s, NULL, 0, 0};
    int n = 0;
    rc = rx_iterate(ctx, s, dbf, 1, cb_total, &ud, &n);
    if (rc != R_OK) {
        for (size_t i = 0; i < ud.n; i++) {
            free(ud.recs[i].keys);
        }
        free(ud.recs);
        return aggr_raise(ctx, s, rc, "TOTAL TO");
    }
    /* FoxPro requires the table to be indexed on the ON expression; we
       accept any scope and sort by the encoded key (DEVIATIONS #46).
       FIELDS (when given) selects which numeric fields are summed; the
       target keeps the full source structure (FoxPro semantics). */
    qsort(ud.recs, ud.n, sizeof(sortrec), total_keycmp);
    size_t *sumf = NULL, nsum = 0;
    int have_fields = 0;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "FIELD") == 0) {
            have_fields = 1;
            size_t idx;
            if (rx_dbf_field_index(dbf, s->clauses[i].word, &idx) != R_OK) {
                for (size_t k = 0; k < ud.n; k++) {
                    free(ud.recs[k].keys);
                }
                free(ud.recs);
                return rx_raise(ctx, R_ERR_FIELD, s->line, s->col, "Unknown field %s",
                                s->clauses[i].word);
            }
            sumf = realloc(sumf, (nsum + 1) * sizeof(size_t));
            if (!sumf) {
                RX_FATAL("out of memory");
            }
            sumf[nsum++] = idx;
        }
    }
    size_t nf = 0;
    rx_dbf_field_count(dbf, &nf);
    size_t* flds = malloc(nf * sizeof(size_t));
    if (!flds) {
        RX_FATAL("out of memory");
    }
    for (size_t i = 0; i < nf; i++) {
        flds[i] = i;
    }
    char path[600];
    snprintf(path, sizeof(path), "%.599s", to);
    if (!strchr(to, '.')) {
        strncat(path, ".dbf", sizeof(path) - strlen(path) - 1);
    }
    rx_fielddef* defs = malloc(nf * sizeof(rx_fielddef));
    if (!defs) {
        RX_FATAL("out of memory");
    }
    for (size_t i = 0; i < nf; i++) {
        rx_dbf_field_def(dbf, flds[i], &defs[i]);
    }
    rx_dbf* dst = NULL;
    rc = rx_dbf_create(path, defs, nf, &dst);
    free(defs);
    if (rc != R_OK) {
        free(flds);
        for (size_t i = 0; i < ud.n; i++) {
            free(ud.recs[i].keys);
        }
        free(ud.recs);
        return aggr_raise(ctx, s, rc, "TOTAL TO");
    }
    /* walk groups of equal keys */
    size_t i = 0;
    while (i < ud.n && rc == R_OK) {
        size_t j = i;
        while (j + 1 < ud.n && total_keycmp(&ud.recs[i], &ud.recs[j + 1]) == 0) {
            j++;
        }
        /* group = [i..j]; emit one record: first record's fields, numeric
           fields summed across the group */
        rc = rx_dbf_goto(dbf, ud.recs[i].recno);
        uint32_t nr = 0;
        if (rc == R_OK) {
            rc = rx_dbf_append(dst, &nr);
        }
        for (size_t fj = 0; fj < nf && rc == R_OK; fj++) {
            rx_fielddef fd;
            rx_dbf_field_def(dbf, flds[fj], &fd);
            int numeric = fd.type == RFT_N || fd.type == RFT_F || fd.type == RFT_I;
            if (have_fields && numeric) {
                int listed = 0;
                for (size_t q = 0; q < nsum; q++) {
                    if (sumf[q] == flds[fj]) {
                        listed = 1;
                    }
                }
                numeric = listed; /* FIELDS restricts which numerics are summed */
            }
            if (numeric && j > i) {
                double acc = 0;
                int any = 0;
                for (size_t g = i; g <= j; g++) {
                    if (rx_dbf_goto(dbf, ud.recs[g].recno) != R_OK) {
                        continue;
                    }
                    rval v;
                    rx_rval_undef(&v);
                    if (rx_dbf_get(dbf, flds[fj], &v) == R_OK) {
                        double d;
                        if (rv_to_num(&v, &d)) {
                            acc += d;
                            any = 1;
                        }
                    }
                    rx_rval_free(&v);
                }
                rval v;
                rx_rval_undef(&v);
                v.t = RX_T_N;
                v.u.n.d = acc;
                v.u.n.dec = fd.dec;
                if (any) {
                    rc = rx_dbf_put(dst, fj, &v);
                }
                (void)rc;
            } else {
                if (rx_dbf_goto(dbf, ud.recs[i].recno) != R_OK) {
                    rc = R_ERR_DBF_RECNO;
                    break;
                }
                rval v;
                rx_rval_undef(&v);
                if (rx_dbf_get(dbf, flds[fj], &v) == R_OK) {
                    rc = rx_dbf_put(dst, fj, &v);
                }
                rx_rval_free(&v);
            }
        }
        if (rc == R_OK) {
            rc = rx_flush(dst);
        }
        i = j + 1;
    }
    rx_dbf_close(dst);
    free(flds);
    free(sumf);
    for (size_t k = 0; k < ud.n; k++) {
        free(ud.recs[k].keys);
    }
    free(ud.recs);
    if (rc != R_OK) {
        return aggr_raise(ctx, s, rc, "TOTAL TO");
    }
    return R_OK;
}

/* ---------------- SCATTER / GATHER ---------------- */

int rx_cmd_scatter(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    int memo = has_clause(s, "MEMO");
    int blank = has_clause(s, "BLANK");
    const char* toarr = clause_word(s, "TOARRAY", 0);
    int to_memvar = has_clause(s, "MEMVAR");
    if (!toarr && !to_memvar) {
        return aggr_raise(ctx, s, R_ERR_SYNTAX, "SCATTER TO");
    }
    size_t *flds = NULL, nf = 0;
    rc = field_list(ctx, dbf, s, &flds, &nf);
    if (rc != R_OK) {
        return rc;
    }
    if (toarr && rx_array_def(ctx, toarr, nf, 1) != R_OK) {
        free(flds);
        return aggr_raise(ctx, s, R_ERR_STACK, "SCATTER TO ARRAY");
    }
    uint32_t recno;
    rx_dbf_recno(dbf, &recno);
    if (recno >= 1) {
        rx_dbf_goto(dbf, recno); /* ensure loaded at current */
    }
    for (size_t i = 0; i < nf; i++) {
        rx_fielddef fd;
        rx_dbf_field_def(dbf, flds[i], &fd);
        if (fd.type == RFT_M && !memo) {
            continue; /* without MEMO, memo fields scatter blank */
        }
        rval v;
        rx_rval_undef(&v);
        if (blank || recno == 0) {
            /* blank value for the type */
            switch (fd.type) {
            case RFT_N:
            case RFT_F:
            case RFT_I:
                v.t = RX_T_N;
                v.u.n.d = 0;
                v.u.n.dec = fd.dec;
                break;
            case RFT_D:
                v.t = RX_T_D;
                v.u.d.y = 0;
                v.u.d.m = 0;
                v.u.d.d = 0;
                break;
            case RFT_L:
                v.t = RX_T_L;
                v.u.l = L_F;
                break;
            default:
                v.t = RX_T_C;
                v.u.c.utf8 = rx_strdup("");
                v.u.c.n = 0;
                break;
            }
        } else if (rx_dbf_get(dbf, flds[i], &v) != R_OK) {
            rx_rval_undef(&v);
        }
        if (toarr) {
            set_array_el(ctx, toarr, i + 1, &v);
        } else {
            set_var(ctx, fd.name, &v);
        }
        rx_rval_free(&v);
    }
    free(flds);
    return R_OK;
}

int rx_cmd_gather(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    int memo = has_clause(s, "MEMO");
    const char* fromarr = clause_word(s, "FROMARRAY", 0);
    int from_memvar = has_clause(s, "MEMVAR");
    if (!fromarr && !from_memvar) {
        return aggr_raise(ctx, s, R_ERR_SYNTAX, "GATHER FROM");
    }
    size_t *flds = NULL, nf = 0;
    rc = field_list(ctx, dbf, s, &flds, &nf);
    if (rc != R_OK) {
        return rc;
    }
    uint32_t recno;
    rx_dbf_recno(dbf, &recno);
    for (size_t i = 0; i < nf; i++) {
        rx_fielddef fd;
        rx_dbf_field_def(dbf, flds[i], &fd);
        if (fd.type == RFT_M && !memo) {
            continue;
        }
        rval v;
        rx_rval_undef(&v);
        int got = 0;
        if (fromarr) {
            if (rx_array_get(ctx, fromarr, i + 1, 1, &v) == R_OK) {
                got = 1;
            }
        } else {
            /* MEMVAR: m.<field> then bare field name */
            char mn[16];
            snprintf(mn, sizeof(mn), "m.%s", fd.name);
            rx_memvar* mv = rx_memvar_find(ctx, mn);
            if (!mv) {
                mv = rx_memvar_find(ctx, fd.name);
            }
            if (mv) {
                rx_rval_deep(&v, &mv->val); /* deep copy: v is owned below */
                got = 1;
            }
        }
        if (got) {
            rc = rx_dbf_put(dbf, flds[i], &v);
            rx_rval_free(&v);
            if (rc != R_OK) {
                free(flds);
                return aggr_raise(ctx, s, rc, "GATHER");
            }
        }
    }
    free(flds);
    if (rc == R_OK) {
        rc = rx_flush(dbf);
    }
    if (rc == R_OK && recno >= 1) {
        rc = rx_index_maintain_rec(ctx, dbf, recno); /* M6: keep tags current */
    }
    if (rc != R_OK) {
        return aggr_raise(ctx, s, rc, "GATHER");
    }
    return R_OK;
}
