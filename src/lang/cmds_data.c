/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_data.c
 * @brief Data commands (spec §7.1): USE, CLOSE, SELECT, LIST, DISPLAY,
 *        GO/GOTO, SKIP with FoxPro scope semantics (spec §8.7).
 */
/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "lang/funcs.h"
#include "lang/lang.h"
#include "reynard/dbf.h"
#include "util/util.h"

/* resolve a table name per SET DEFAULT / SET PATH (spec §10.3) */
static int resolve_table(rx_ctx* ctx, const char* name, char* out, size_t outsz) {
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
        char* save = NULL;
        for (char* dir = strtok_r(pathcopy, ",;", &save); dir; dir = strtok_r(NULL, ",;", &save)) {
            snprintf(cand, sizeof(cand), "%s/%s.dbf", dir, name);
            if (access(cand, F_OK) == 0) {
                snprintf(out, outsz, "%s", cand);
                return R_OK;
            }
        }
    }
    return R_ERR_IO_NOTFOUND;
}

int rx_cmd_use(rx_ctx* ctx, rx_stmt* s) {
    int area = 0; /* 0 = current */
    const char* alias = NULL;
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
                    const char* a;
                    size_t n;
                    if (rv_to_str(&v, &a, &n)) {
                        for (int k = 1; k <= 255; k++) {
                            if (rx_wa_used(k)) {
                                const char* ka;
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
                const char* w = s->clauses[i].word;
                int numeric = w[0] >= '0' && w[0] <= '9';
                for (const char* q = w; *q; q++) {
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
                            const char* ka;
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
    const char* nm;
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
    int mode = ctx->set.exclusive ? RD_EXCLUSIVE : RD_SHARED;
    for (size_t i = 0; i < s->nstrs; i++) {
        if (rx_strcasecmp(s->strs[i], "EXCLUSIVE") == 0) {
            mode = RD_EXCLUSIVE;
        } else if (rx_strcasecmp(s->strs[i], "SHARED") == 0) {
            mode = RD_SHARED;
        } else if (rx_strcasecmp(s->strs[i], "NOUPDATE") == 0) {
            mode = RD_READONLY;
        }
    }
    int rc2 = rx_wa_open(area, path, mode, alias);
    if (rc2 != R_OK) {
        return rc2;
    }
    /* M6 §8.4: USE ... INDEX f1 [, f2] [ORDER n|TAG t [OF cdx]] */
    rx_dbf* dbf;
    if (rx_wa_dbf(area, &dbf) == R_OK) {
        rx_index_registry_purge(ctx, path);
        int nidx = 0;
        for (size_t i = 0; i < s->nclause; i++) {
            if (strcmp(s->clauses[i].key, "IDXFILE") != 0) {
                continue;
            }
            nidx++;
            char ipath[600];
            int rc3 = resolve_index_m6(ctx, s->clauses[i].word, ipath, sizeof(ipath));
            if (rc3 != R_OK) {
                return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "File %s not found",
                                s->clauses[i].word);
            }
            rc3 = rx_index_attach(dbf, ipath, NULL);
            if (rc3 != R_OK) {
                return rx_raise(ctx, rc3, s->line, s->col, "Cannot open index %s", ipath);
            }
        }
        int has_order = 0;
        const char* ofpath = NULL;
        char ofbuf[600];
        for (size_t i = 0; i < s->nclause; i++) {
            if (strcmp(s->clauses[i].key, "OF") == 0) {
                if (resolve_index_m6(ctx, s->clauses[i].word, ofbuf, sizeof(ofbuf)) == R_OK) {
                    ofpath = ofbuf;
                } else {
                    ofpath = s->clauses[i].word;
                }
            }
        }
        for (size_t i = 0; i < s->nclause; i++) {
            if (strcmp(s->clauses[i].key, "ORDER") == 0) {
                has_order = 1;
                rx_clause* oc = &s->clauses[i];
                const char* w = oc->word;
                int numeric = w[0] != '\0';
                for (const char* q = w; *q; q++) {
                    if (*q < '0' || *q > '9') {
                        numeric = 0;
                        break;
                    }
                }
                if (oc->e) {
                    rval v;
                    rx_rval_undef(&v);
                    int rc3 = rx_eval(ctx, oc->e, &v, &ctx->err);
                    if (rc3 != R_OK) {
                        return rc3;
                    }
                    double d = 0;
                    rv_to_num(&v, &d);
                    rx_rval_free(&v);
                    long n = (long)d;
                    if (n <= 0) {
                        rx_index_order_set(dbf, -1, -1);
                    } else {
                        int fi, ti;
                        if (rx_index_order_nth(dbf, (uint32_t)n, &fi, &ti) != R_OK) {
                            return rx_raise(ctx, R_ERR_RANGE, s->line, s->col,
                                            "Invalid index number %ld", n);
                        }
                        rx_index_order_set(dbf, fi, ti);
                    }
                } else if (numeric) {
                    long n = strtol(w, NULL, 10);
                    if (n <= 0) {
                        rx_index_order_set(dbf, -1, -1);
                    } else {
                        int fi, ti;
                        if (rx_index_order_nth(dbf, (uint32_t)n, &fi, &ti) != R_OK) {
                            return rx_raise(ctx, R_ERR_RANGE, s->line, s->col,
                                            "Invalid index number %ld", n);
                        }
                        rx_index_order_set(dbf, fi, ti);
                    }
                } else {
                    int fi, ti;
                    if (rx_index_find_tag(dbf, ofpath, w, &fi, &ti) != R_OK) {
                        return rx_raise(ctx, R_ERR_CDX_TAG, s->line, s->col, "Tag %s not found", w);
                    }
                    rx_index_order_set(dbf, fi, ti);
                }
            }
        }
        if (!has_order && nidx > 0) {
            rx_index_default_order(dbf); /* first tag of the first file */
        } else if (!has_order) {
            rx_index_default_order(dbf); /* structural CDX tag, when present */
        }
    }
    return R_OK;
}

int rx_cmd_close(rx_ctx* ctx, rx_stmt* s) {
    int all = 0;
    for (size_t i = 0; i < s->nstrs; i++) {
        if (rx_strcasecmp(s->strs[i], "ALL") == 0 || rx_strcasecmp(s->strs[i], "DATABASES") == 0 ||
            rx_strcasecmp(s->strs[i], "DATABASE") == 0) {
            all = 1;
        }
    }
    if (all) {
        for (int i = 1; i <= 255; i++) {
            rx_dbf* d;
            if (rx_wa_used(i) && rx_wa_dbf(i, &d) == R_OK) {
                const char* p;
                if (rx_dbf_path(d, &p) == R_OK) {
                    rx_index_registry_purge(ctx, p); /* M6: drop key-expr cache */
                }
            }
            rx_wa_close(i);
        }
        return R_OK;
    }
    rx_dbf* d;
    if (rx_wa_dbf(0, &d) == R_OK) {
        const char* p;
        if (rx_dbf_path(d, &p) == R_OK) {
            rx_index_registry_purge(ctx, p);
        }
    }
    return rx_wa_close(0);
}

int rx_cmd_select(rx_ctx* ctx, rx_stmt* s) {
    if (s->nstrs > 0) {
        /* bare word: alias or numeric area */
        const char* w = s->strs[0];
        int numeric = w[0] >= '0' && w[0] <= '9';
        for (const char* q = w; *q; q++) {
            if (*q < '0' || *q > '9') {
                numeric = 0;
                break;
            }
        }
        int area;
        if (numeric) {
            area = atoi(w);
        } else {
            area = 0;
            for (int i = 1; i <= 255; i++) {
                if (rx_wa_used(i)) {
                    const char* ka;
                    rx_wa_alias(i, &ka);
                    if (rx_strcasecmp(ka, w) == 0) {
                        area = i;
                        break;
                    }
                }
            }
            if (area == 0) {
                return rx_raise(ctx, R_ERR_FIELD, s->line, s->col, "Alias not found");
            }
        }
        if (area < 0 || area > 255) {
            return rx_raise(ctx, R_ERR_RANGE, s->line, s->col, "SELECT area out of range");
        }
        rx_wa_select(area == 0 ? rx_wa_current() : area);
        return R_OK;
    }
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
    const char* a;
    size_t n;
    if (rv_to_str(&v, &a, &n)) {
        for (int i = 1; i <= 255; i++) {
            if (rx_wa_used(i)) {
                const char* ka;
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

/* scope evaluation (spec \u00a78.7): clauses SCOPE/FOR/WHILE; the positional
   half of the range lives in data/scope.c, filter evaluation here (VM). */
static int eval_scope(rx_ctx* ctx, rx_stmt* s, int default_kind, rx_scope* sc) {
    memset(sc, 0, sizeof(*sc));
    sc->kind = default_kind;
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

/* shared scope iteration with SET DELETED, SET FILTER, FOR and WHILE */

/* internal sentinel: callback asks iterate() to stop scanning without error
   (SCAN body hit EXIT/RETURN) */
#define RX_ITER_STOP 0x7ffffff1

static int eval_predicate(rx_ctx* ctx, const rx_code* code, int* pass) {
    rval v;
    rx_rval_undef(&v);
    int rc = rx_vm_run(ctx, code, &v, &ctx->err);
    if (rc != R_OK) {
        return rc;
    }
    rx_logic l;
    int ok = rv_to_logic(&v, &l);
    rx_rval_free(&v);
    if (!ok) {
        return R_ERR_TYPE;
    }
    *pass = (l == L_T);
    return R_OK;
}

static int compile_clause(rx_ctx* ctx, rx_stmt* s, const char* key, rx_code* code, int* has) {
    *has = 0;
    memset(code, 0, sizeof(*code));
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, key) == 0 && s->clauses[i].e) {
            *has = 1;
            return rx_compile_expr(s->clauses[i].e, code, &ctx->err);
        }
    }
    return R_OK;
}

/* iterate the scope, honoring SET DELETED / SET FILTER / FOR / WHILE; the
   record pointer ends per FoxPro rules (RECORD n restores, WHILE stops on
   the first false record, ALL/REST/NEXT run to the scope end) */
static int iterate(rx_ctx* ctx, rx_stmt* s, rx_dbf* dbf, int default_kind, rx_rec_cb cb, void* ud,
                   int* count, int keep_pos) {
    rx_scope sc;
    int rc = eval_scope(ctx, s, default_kind, &sc);
    if (rc != R_OK) {
        return rc;
    }
    rx_code for_code, while_code;
    int has_for = 0, has_while = 0;
    rc = compile_clause(ctx, s, "FOR", &for_code, &has_for);
    if (rc != R_OK) {
        return rc;
    }
    rc = compile_clause(ctx, s, "WHILE", &while_code, &has_while);
    if (rc != R_OK) {
        if (has_for) {
            rx_code_free(&for_code);
        }
        return rc;
    }
    uint32_t start, end;
    int restore;
    rc = rx_scope_range(dbf, &sc, &start, &end, &restore);
    if (rc != R_OK) {
        if (has_for) {
            rx_code_free(&for_code);
        }
        if (has_while) {
            rx_code_free(&while_code);
        }
        return rc;
    }
    uint32_t saved, nrec;
    rx_dbf_recno(dbf, &saved);
    rx_dbf_recount(dbf, &nrec);
    /* M6 §8.4: with a controlling index order the scope runs in tag order;
       build the record-number sequence up front (ordered positions for
       ALL/REST/NEXT, physical RECORD n), then walk it. */
    int ordered = rx_index_order_active(dbf);
    uint32_t* seq = NULL;
    size_t nseq = 0;
    int64_t endpos = -1; /* ordered: position one past the scope end */
    if (ordered) {
        int fi, ti;
        rx_index_order_get(dbf, &fi, &ti);
        uint32_t nk = 0;
        rx_index_key_count(dbf, fi, ti, &nk);
        int64_t from = 0, to = -1;
        if (sc.kind == 4) {
            restore = 1;
            if (sc.n >= 1 && sc.n <= nrec) {
                nseq = 1;
            }
            seq = malloc((nseq ? nseq : 1) * sizeof(uint32_t));
            if (!seq) {
                RX_FATAL("out of memory");
            }
            if (nseq) {
                seq[0] = sc.n;
            }
        } else {
            int64_t curpos;
            int eof;
            rx_dbf_eof(dbf, &eof);
            if (eof) {
                curpos = (int64_t)nk;
            } else {
                int64_t pp = -1;
                rx_index_pos_of(dbf, saved, &pp);
                curpos = pp >= 0 ? pp : (int64_t)(saved ? saved - 1 : 0);
            }
            switch (sc.kind) {
            case 1:
                from = 0;
                to = (int64_t)nk - 1;
                break;
            case 2:
                from = curpos;
                to = (int64_t)nk - 1;
                break;
            case 3:
                from = curpos;
                to = curpos + (int64_t)sc.n - 1;
                break;
            default:
                from = curpos;
                to = curpos;
                break;
            }
            if (to >= (int64_t)nk) {
                to = (int64_t)nk - 1;
            }
            if (from < 0) {
                from = 0;
            }
            nseq = to >= from ? (size_t)(to - from + 1) : 0;
            seq = malloc((nseq ? nseq : 1) * sizeof(uint32_t));
            if (!seq) {
                RX_FATAL("out of memory");
            }
            for (size_t i = 0; i < nseq; i++) {
                uint32_t r = 0;
                rx_index_recno_at(dbf, (uint32_t)(from + (int64_t)i), &r);
                seq[i] = r;
            }
            endpos = sc.kind == 3 ? from + (int64_t)sc.n : (int64_t)nk;
        }
    } else {
        nseq = end >= start ? (size_t)(end - start + 1) : 0;
        seq = malloc((nseq ? nseq : 1) * sizeof(uint32_t));
        if (!seq) {
            RX_FATAL("out of memory");
        }
        for (size_t i = 0; i < nseq; i++) {
            seq[i] = start + (uint32_t)i;
        }
    }
    int stop = 0;
    int cnt = 0;
    for (size_t si = 0; si < nseq; si++) {
        uint32_t r = seq[si];
        if (r > nrec) {
            break;
        }
        rc = rx_dbf_goto(dbf, r);
        if (rc != R_OK) {
            break;
        }
        int del;
        rx_dbf_deleted(dbf, &del);
        if (ctx->set.deleted && del) {
            continue;
        }
        if (ctx->filter_on) {
            int pass;
            rc = eval_predicate(ctx, &ctx->filter, &pass);
            if (rc != R_OK) {
                break;
            }
            if (!pass) {
                continue;
            }
        }
        if (has_for) {
            int pass;
            rc = eval_predicate(ctx, &for_code, &pass);
            if (rc != R_OK) {
                break;
            }
            if (!pass) {
                continue;
            }
        }
        if (has_while) {
            int pass;
            rc = eval_predicate(ctx, &while_code, &pass);
            if (rc != R_OK) {
                break;
            }
            if (!pass) {
                stop = 1;
                break;
            }
        }
        if (ctx->loop_ctl == RX_CTL_EXIT) {
            stop = 1;
            break;
        }
        rc = cb(ctx, dbf, r, ud);
        if (rc == RX_ITER_STOP) {
            rc = R_OK;
            stop = 1;
            break;
        }
        if (rc != R_OK) {
            break;
        }
        cnt++;
    }
    if (restore || (keep_pos && !stop)) {
        /* RECORD n restores; REPLACE/DELETE/RECALL leave the pointer put
           (FoxPro semantics); a past-end save is a no-op */
        rx_dbf_goto(dbf, saved);
    } else if (!stop && ordered) {
        /* M6: leave the pointer one past the scope in tag order (EOF at end) */
        int fi, ti;
        rx_index_order_get(dbf, &fi, &ti);
        uint32_t nk = 0;
        rx_index_key_count(dbf, fi, ti, &nk);
        if (endpos >= (int64_t)nk || nk == 0) {
            rx_goto_logical(dbf, nrec ? nrec + 1 : 1);
        } else {
            uint32_t r = 0;
            rx_index_recno_at(dbf, (uint32_t)endpos, &r);
            rx_dbf_goto(dbf, r);
        }
    } else if (!stop && end >= 1 && end <= nrec) {
        rx_dbf_goto(dbf, end);
        rx_dbf_skip(dbf, 1);
    } else if (!stop && nrec == 0) {
        rx_dbf_goto(dbf, 1);
    } else if (!stop) {
        rx_dbf_goto(dbf, saved);
    }
    free(seq);
    if (has_for) {
        rx_code_free(&for_code);
    }
    if (has_while) {
        rx_code_free(&while_code);
    }
    if (count) {
        *count = cnt;
    }
    return rc;
}

typedef struct {
    rx_ctx* ctx;
    const size_t* flds; /**< selected field indices */
    size_t nf;          /**< number of selected fields */
    rx_fielddef* fds;   /**< cached def of each selected field (M10) */
    size_t* foffs;      /**< byte offset of every field in a record (M10) */
    size_t* widths;
    int recw;
    int off;
    FILE* sink; /**< LIST/DISPLAY TO FILE: write the listing here (M10) */
    strbuf sb;  /**< reused across records (M10 perf gate) */
} list_ud;

/* Format a DBF numeric field exactly like rx_parse_num()+rx_format_num(d,dec,1)
   ("%.*f") when no rounding is involved (plain decimal, <= 15 significant
   digits, fractional digits <= dec); returns 0 when the exact strtod/snprintf
   path must be used instead. Hot on the LIST 100k gate (spec 11.3, M10). */
static int fast_num(const uint8_t* raw, size_t len, int dec, char* out, size_t outsz) {
    size_t i = 0;
    while (i < len && (raw[i] == ' ' || raw[i] == '\t')) {
        i++;
    }
    int neg = 0;
    if (i < len && (raw[i] == '-' || raw[i] == '+')) {
        neg = raw[i] == '-';
        i++;
    }
    size_t int_start = i;
    while (i < len && raw[i] >= '0' && raw[i] <= '9') {
        i++;
    }
    size_t int_n = i - int_start;
    size_t frac_start = 0, frac_n = 0;
    if (i < len && raw[i] == '.') {
        i++;
        frac_start = i;
        while (i < len && raw[i] >= '0' && raw[i] <= '9') {
            i++;
            frac_n++;
        }
    }
    while (i < len && (raw[i] == ' ' || raw[i] == '\t')) {
        i++;
    }
    if (i != len) {
        return 0; /* not a plain decimal (e.g. overflow ******): exact path */
    }
    if (int_n + frac_n > 15) {
        return 0; /* too many significant digits to round-trip through double */
    }
    if (dec > 15) {
        dec = 15; /* same clamp as rx_format_num */
    }
    if (frac_n > (size_t)dec) {
        return 0; /* rounding needed: exact path */
    }
    size_t o = 0;
    if (neg && int_n + frac_n > 0) {
        if (o + 1 >= outsz) {
            return 0;
        }
        out[o++] = '-';
    }
    if (int_n == 0) {
        if (o + 1 >= outsz) {
            return 0;
        }
        out[o++] = '0'; /* blank field -> 0, like rx_parse_num */
    } else {
        if (o + int_n >= outsz) {
            return 0;
        }
        memcpy(out + o, raw + int_start, int_n);
        o += int_n;
    }
    if (dec > 0) {
        if (o + 1 + (size_t)dec >= outsz) {
            return 0;
        }
        out[o++] = '.';
        if (frac_n) {
            memcpy(out + o, raw + frac_start, frac_n);
            o += frac_n;
        }
        for (size_t k = frac_n; k < (size_t)dec; k++) {
            out[o++] = '0';
        }
    }
    out[o] = '\0';
    return 1;
}

/* append n spaces in bulk (padding is hot on the LIST perf path, M10) */
static void sb_spaces(strbuf* sb, size_t n) {
    static const char sp[64] = "                                                                ";
    while (n > 0) {
        size_t c = n > sizeof(sp) ? sizeof(sp) : n;
        sb_put(sb, sp, c);
        n -= c;
    }
}

/* Emit one field of one record into sb, padded to width w. raw points at the
   field bytes inside the current record buffer when available (M10 fast
   path, no per-field pread/strdup); NULL forces the generic rval path. */
static void list_emit_field(rx_ctx* ctx, rx_dbf* dbf, size_t f, const rx_fielddef* fd, size_t w,
                            const uint8_t* raw, strbuf* sb) {
    switch (fd->type) {
    case RFT_C:
        if (raw) { /* caller guarantees codepage <= 0 when raw is set */
            sb_put(sb, (const char*)raw, fd->len);
            if ((size_t)fd->len < w) {
                sb_spaces(sb, w - fd->len);
            }
            return;
        }
        break;
    case RFT_N:
    case RFT_F:
        if (raw) {
            char num[64];
            if (!fast_num(raw, fd->len, fd->dec, num, sizeof(num))) {
                int blank;
                double d = rx_parse_num((const char*)raw, fd->len, &blank);
                rx_format_num(num, sizeof(num), d, fd->dec, 1);
            }
            size_t nl = strlen(num);
            if (nl < w) {
                sb_spaces(sb, w - nl);
            }
            sb_puts(sb, num);
            return;
        }
        break;
    case RFT_D:
        if (raw) {
            char d[48];
            int blank = 1;
            for (int i = 0; i < 8; i++) {
                if (raw[i] != ' ' && raw[i] != '0') {
                    blank = 0;
                    break;
                }
            }
            if (blank) {
                sb_puts(sb, ctx->set.century ? "  /  /    " : "  /  /  ");
                return;
            }
            int y =
                (raw[0] - '0') * 1000 + (raw[1] - '0') * 100 + (raw[2] - '0') * 10 + (raw[3] - '0');
            int m = (raw[4] - '0') * 10 + (raw[5] - '0');
            int dd = (raw[6] - '0') * 10 + (raw[7] - '0');
            if (y < 1 || m < 1 || m > 12 || dd < 1 || dd > 31) {
                sb_putc(sb, '?'); /* decode error: same as rx_dbf_get failure */
                return;
            }
            int alldig = 1;
            for (int q = 0; q < 8; q++) {
                if (raw[q] < '0' || raw[q] > '9') {
                    alldig = 0;
                    break;
                }
            }
            if (alldig) {
                /* MM/DD/YY(YY) is a pure digit copy of YYYYMMDD (M10 fast path) */
                char* p = d;
                *p++ = (char)raw[4];
                *p++ = (char)raw[5];
                *p++ = '/';
                *p++ = (char)raw[6];
                *p++ = (char)raw[7];
                *p++ = '/';
                if (ctx->set.century) {
                    memcpy(p, raw, 4);
                    p += 4;
                } else {
                    *p++ = (char)raw[2];
                    *p++ = (char)raw[3];
                }
                *p = '\0';
            } else if (ctx->set.century) {
                snprintf(d, sizeof(d), "%02d/%02d/%04d", m, dd, y);
            } else {
                snprintf(d, sizeof(d), "%02d/%02d/%02d", m, dd, y % 100);
            }
            sb_puts(sb, d);
            return;
        }
        break;
    case RFT_L:
        if (raw) {
            char c = (char)raw[0];
            char o = (c == 'Y' || c == 'y' || c == 'T' || c == 't')   ? 'T'
                     : (c == 'N' || c == 'n' || c == 'F' || c == 'f') ? 'F'
                                                                      : '?';
            sb_putc(sb, o);
            if (w > 1) {
                sb_spaces(sb, w - 1);
            }
            return;
        }
        break;
    case RFT_I:
        if (raw && fd->len >= 4) {
            char num[64];
            int32_t v;
            memcpy(&v, raw, 4);
            snprintf(num, sizeof(num), "%d", v);
            size_t nl = strlen(num);
            if (nl < w) {
                sb_spaces(sb, w - nl);
            }
            sb_puts(sb, num);
            return;
        }
        break;
    default:
        break;
    }
    /* generic path: decode through the rval API (memo/codepage/exotic types) */
    rval v;
    rx_rval_undef(&v);
    if (rx_dbf_get(dbf, f, &v) != R_OK) {
        sb_putc(sb, '?');
        return;
    }
    if (fd->type == RFT_C) {
        const char* str = v.t == RX_T_C ? (v.u.c.utf8 ? v.u.c.utf8 : "") : "";
        size_t n = v.t == RX_T_C ? v.u.c.n : 0;
        sb_put(sb, str, n);
        if (n < w) {
            sb_spaces(sb, w - n);
        }
    } else if (fd->type == RFT_M) {
        sb_puts(sb, "Memo");
        if (w > 4) {
            sb_spaces(sb, w - 4);
        }
    } else if (fd->type == RFT_N || fd->type == RFT_F) {
        char num[64];
        rx_format_num(num, sizeof(num), v.u.n.d, fd->dec, 1);
        size_t nl = strlen(num);
        if (nl < w) {
            sb_spaces(sb, w - nl);
        }
        sb_puts(sb, num);
    } else if (fd->type == RFT_D) {
        char d[48];
        if (v.u.d.y == 0) {
            sb_puts(sb, ctx->set.century ? "  /  /    " : "  /  /  ");
        } else if (ctx->set.century) {
            snprintf(d, sizeof(d), "%02d/%02d/%04d", v.u.d.m, v.u.d.d, v.u.d.y);
            sb_puts(sb, d);
        } else {
            snprintf(d, sizeof(d), "%02d/%02d/%02d", v.u.d.m, v.u.d.d, v.u.d.y % 100);
            sb_puts(sb, d);
        }
    } else if (fd->type == RFT_L) {
        sb_putc(sb, v.u.l == L_T ? 'T' : v.u.l == L_F ? 'F' : '?');
        if (w > 1) {
            sb_spaces(sb, w - 1);
        }
    } else if (fd->type == RFT_I) {
        char num[64];
        snprintf(num, sizeof(num), "%d", v.u.i);
        size_t nl = strlen(num);
        if (nl < w) {
            sb_spaces(sb, w - nl);
        }
        sb_puts(sb, num);
    } else {
        strbuf tmp2;
        sb_init(&tmp2);
        rx_vm_display(ctx, &v, &tmp2);
        size_t n = tmp2.len;
        if (n < w) {
            sb_spaces(sb, w - n);
        }
        sb_put(sb, tmp2.buf ? tmp2.buf : "", n);
        sb_free(&tmp2);
    }
    rx_rval_free(&v);
}

/* flush the accumulated listing buffer (M10: batched writes, not per-record) */
static void flush_list(list_ud* u) {
    strbuf* sb = &u->sb;
    if (sb->buf && sb->len) {
        if (u->sink) {
            fwrite(sb->buf, 1, sb->len, u->sink);
        } else {
            rx_out(u->ctx, sb->buf, sb->len);
        }
    }
    sb->len = 0;
}

/* right-aligned decimal into out; returns length (no snprintf on hot path) */
static size_t fmt_u32(char* out, uint32_t v) {
    char t[10];
    size_t n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (size_t i = 0; i < n; i++) {
        out[i] = t[n - 1 - i];
    }
    return n;
}

static void print_record_line(list_ud* u, rx_dbf* dbf, uint32_t recno, int deleted) {
    rx_ctx* ctx = u->ctx;
    strbuf* sb = &u->sb;
    char tmp[16];
    if (!u->off) {
        int rw = u->recw;
        if (deleted) {
            sb_putc(sb, '*');
            rw--;
        }
        size_t n = fmt_u32(tmp, recno);
        if (n < (size_t)rw) {
            sb_spaces(sb, (size_t)rw - n);
        }
        sb_put(sb, tmp, n);
    }
    size_t reclen = 0;
    const uint8_t* rb = rx_dbf_cur_recbuf(dbf, &reclen);
    int cp = 0;
    rx_dbf_codepage(dbf, &cp);
    if (cp > 0) {
        rb = NULL; /* char fields need code-page conversion: generic path */
    }
    for (size_t k = 0; k < u->nf; k++) {
        size_t f = u->flds[k];
        const uint8_t* raw = NULL;
        if (rb) {
            size_t off = u->foffs[f];
            if (off + u->fds[k].len <= reclen) {
                raw = rb + off;
            }
        }
        sb_putc(sb, ' ');
        list_emit_field(ctx, dbf, f, &u->fds[k], u->widths[k], raw, sb);
    }
    sb_putc(sb, '\n');
    if (sb->len >= (128u << 10)) {
        flush_list(u);
    }
}

static int cb_list(rx_ctx* ctx, rx_dbf* dbf, uint32_t recno, void* ud) {
    list_ud* u = ud;
    int del;
    rx_dbf_deleted(dbf, &del);
    (void)ctx;
    print_record_line(u, dbf, recno, del);
    return R_OK;
}

/* resolve the FIELDS clause into an index array; default = all fields */
static int select_fields(rx_ctx* ctx, rx_stmt* s, rx_dbf* dbf, size_t** flds_out, size_t* nf_out) {
    size_t nf;
    rx_dbf_field_count(dbf, &nf);
    size_t* flds = malloc((nf ? nf : 1) * sizeof(size_t));
    if (!flds) {
        RX_FATAL("out of memory");
    }
    size_t nsel = 0;
    int any = 0;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "FIELD") == 0) {
            any = 1;
            size_t idx;
            if (rx_dbf_field_index(dbf, s->clauses[i].word, &idx) != R_OK) {
                free(flds);
                return rx_raise(ctx, R_ERR_FIELD, s->line, s->col, "Unknown field %s",
                                s->clauses[i].word);
            }
            flds[nsel++] = idx;
        }
    }
    if (!any) {
        for (size_t i = 0; i < nf; i++) {
            flds[i] = i;
        }
        nsel = nf;
    }
    *flds_out = flds;
    *nf_out = nsel;
    return R_OK;
}

int rx_cmd_list(rx_ctx* ctx, rx_stmt* s, int display) {
    rx_dbf* dbf;
    int rc = rx_wa_dbf(0, &dbf);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
    }
    size_t nf;
    rx_dbf_field_count(dbf, &nf);
    uint32_t nrec;
    rx_dbf_recount(dbf, &nrec);

    size_t* flds;
    size_t nsel;
    rc = select_fields(ctx, s, dbf, &flds, &nsel);
    if (rc != R_OK) {
        return rc;
    }
    int off = 0;
    for (size_t i = 0; i < s->nstrs; i++) {
        if (rx_strcasecmp(s->strs[i], "OFF") == 0) {
            off = 1;
        }
    }
    /* M10 7.1: LIST/DISPLAY ... TO FILE f redirect the listing to f */
    FILE* sink = NULL;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "TOFILE") == 0 && s->clauses[i].word[0]) {
            sink = fopen(s->clauses[i].word, "w");
            if (!sink) {
                free(flds);
                return rx_raise(ctx, R_ERR_IO_OPEN, s->line, s->col, "Cannot open %s",
                                s->clauses[i].word);
            }
            setvbuf(sink, NULL, _IOFBF, 1 << 20); /* big buffer: 100k-record gate (M10) */
        }
    }

    /* M10 perf: cache field defs/widths and per-field record offsets once, so
       the per-record loop never calls rx_dbf_field_def or re-walks the defs */
    size_t* widths = calloc(nsel, sizeof(size_t));
    rx_fielddef* selfds = malloc((nsel ? nsel : 1) * sizeof(rx_fielddef));
    size_t* foffs = malloc((nf ? nf : 1) * sizeof(size_t));
    if (!widths || !selfds || !foffs) {
        free(widths);
        free(selfds);
        free(foffs);
        free(flds);
        if (sink) {
            fclose(sink);
        }
        RX_FATAL("out of memory");
    }
    {
        size_t o = 1; /* byte 0 is the delete flag */
        for (size_t i = 0; i < nf; i++) {
            rx_fielddef fd;
            rx_dbf_field_def(dbf, i, &fd);
            foffs[i] = o;
            o += fd.len;
        }
    }
    for (size_t k = 0; k < nsel; k++) {
        rx_fielddef fd;
        rx_dbf_field_def(dbf, flds[k], &fd);
        selfds[k] = fd;
        size_t w = strlen(fd.name);
        size_t tw = fd.len;
        if (fd.type == RFT_D) {
            tw = ctx->set.century ? 10 : 8;
        }
        if (fd.type == RFT_L) {
            tw = 1;
        }
        if (fd.type == RFT_M) {
            tw = 4; /* LIST shows the word Memo */
        }
        if (tw > w) {
            w = tw;
        }
        widths[k] = w;
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

    if (ctx->set.headings) {
        strbuf sb;
        sb_init(&sb);
        char tmp[32];
        if (!off) {
            snprintf(tmp, sizeof(tmp), "%*s", recw, "Record#");
            sb_puts(&sb, tmp);
        }
        for (size_t k = 0; k < nsel; k++) {
            sb_putc(&sb, ' ');
            sb_puts(&sb, selfds[k].name);
            for (size_t i = strlen(selfds[k].name); i < widths[k]; i++) {
                sb_putc(&sb, ' ');
            }
        }
        sb_putc(&sb, '\n');
        if (!off) {
            for (int i = 0; i < recw; i++) {
                sb_putc(&sb, '-');
            }
        }
        for (size_t k = 0; k < nsel; k++) {
            sb_putc(&sb, ' ');
            for (size_t i = 0; i < widths[k]; i++) {
                sb_putc(&sb, '-');
            }
        }
        sb_putc(&sb, '\n');
        if (sb.buf) {
            if (sink) {
                fwrite(sb.buf, 1, sb.len, sink);
            } else {
                rx_out(ctx, sb.buf, sb.len);
            }
        }
        sb_free(&sb);
    }

    list_ud ud;
    ud.ctx = ctx;
    ud.flds = flds;
    ud.nf = nsel;
    ud.fds = selfds;
    ud.foffs = foffs;
    ud.widths = widths;
    ud.recw = recw;
    ud.off = off;
    ud.sink = sink;
    sb_init(&ud.sb);
    rc = iterate(ctx, s, dbf, display ? 3 : 1, cb_list, &ud, NULL, 0);
    flush_list(&ud); /* M10: drain the batched listing buffer */
    sb_free(&ud.sb);
    if (sink) {
        fclose(sink);
    }
    free(widths);
    free(selfds);
    free(foffs);
    free(flds);
    return rc;
}

/* M4: navigation helpers that reposition related parent work areas
   (SET RELATION) after every record-pointer move */
int rx_nav_goto(rx_ctx* ctx, uint32_t recno) {
    rx_dbf* dbf;
    int rc = rx_wa_dbf(0, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    rc = rx_dbf_goto(dbf, recno);
    if (rc != R_OK) {
        return rc;
    }
    rx_rel_apply(ctx, rx_wa_current(), 0);
    return R_OK;
}

int rx_nav_skip(rx_ctx* ctx, int32_t delta) {
    rx_dbf* dbf;
    int rc = rx_wa_dbf(0, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    if (rx_index_order_active(dbf)) {
        return rx_nav_skip_ordered(ctx, delta); /* M6: move in tag order */
    }
    rc = rx_dbf_skip(dbf, delta);
    if (rc != R_OK) {
        return rc;
    }
    rx_rel_apply(ctx, rx_wa_current(), 1);
    return R_OK;
}

int rx_cmd_go(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = rx_wa_dbf(0, &dbf);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
    }
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "TARGET") == 0) {
            if (strcmp(s->clauses[i].word, "TOP") == 0) {
                if (rx_index_order_active(dbf)) {
                    return rx_nav_top(ctx); /* M6: first in tag order */
                }
                return rx_nav_goto(ctx, 1);
            }
            if (strcmp(s->clauses[i].word, "BOTTOM") == 0) {
                if (rx_index_order_active(dbf)) {
                    return rx_nav_bottom(ctx); /* M6: last in tag order */
                }
                uint32_t n;
                rx_dbf_recount(dbf, &n);
                return rx_nav_goto(ctx, n ? n : 1);
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
            return rx_nav_goto(ctx, (uint32_t)target);
        }
    }
    return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "GO requires TOP, BOTTOM or a record");
}

int rx_cmd_skip(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
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
    return rx_nav_skip(ctx, delta);
}

/* ============================ M2 write commands ============================ */

/* map a data-layer code to a user-facing raise */
static int data_raise(rx_ctx* ctx, rx_stmt* s, int rc, const char* what) {
    switch (rc) {
    case R_ERR_IO_NOTFOUND:
        return rx_raise(ctx, rc, s->line, s->col, "File %s not found", what);
    case R_ERR_IO_PERM:
        return rx_raise(ctx, rc, s->line, s->col, "%s: file is read only or already exists", what);
    case R_ERR_LOCK_CONFLICT:
        return rx_raise(ctx, rc, s->line, s->col, "Record is in use by another process");
    case R_ERR_FILE_IN_USE:
        return rx_raise(ctx, rc, s->line, s->col, "File must be opened exclusively");
    case R_ERR_DBF_NOTOPEN:
        return rx_raise(ctx, rc, s->line, s->col, "No table in use");
    case R_ERR_FIELD:
        return rx_raise(ctx, rc, s->line, s->col, "Unknown field in %s", what);
    default: {
        char msg[512];
        rx_status_message(rc, msg, sizeof(msg));
        return rx_raise(ctx, rc, s->line, s->col, "%s (%s)", msg, what);
    }
    }
}

static int cur_table(rx_ctx* ctx, rx_stmt* s, rx_dbf** out) {
    int rc = rx_wa_dbf(0, out);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
    }
    return R_OK;
}

/* FoxPro rule: DELETE/RECALL/REPLACE default to NEXT 1, but when FOR or
   WHILE is present without an explicit scope the default becomes ALL. */
static int default_scope_m2(rx_stmt* s, int dflt) {
    int has_scope = 0, has_fw = 0;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "SCOPE") == 0) {
            has_scope = 1;
        }
        if (strcmp(s->clauses[i].key, "FOR") == 0 || strcmp(s->clauses[i].key, "WHILE") == 0) {
            has_fw = 1;
        }
    }
    if (!has_scope && has_fw) {
        return 1; /* ALL */
    }
    return dflt;
}

/* ---- DELETE / RECALL ---- */

static int cb_delete(rx_ctx* ctx, rx_dbf* dbf, uint32_t recno, void* ud) {
    (void)ctx;
    (void)recno;
    int recall = *(int*)ud;
    return rx_dbf_set_deleted(dbf, recall ? 0 : 1);
}

int rx_cmd_delete(rx_ctx* ctx, rx_stmt* s, int recall) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    int n = 0;
    rc = iterate(ctx, s, dbf, default_scope_m2(s, 3), cb_delete, &recall, &n, 1);
    if (rc != R_OK) {
        return data_raise(ctx, s, rc, "DELETE");
    }
    if (ctx->set.talk) {
        rx_outf(ctx, "%d record%s %s.\n", n, n == 1 ? "" : "s", recall ? "recalled" : "deleted");
    }
    return R_OK;
}

/* ---- PACK / ZAP ---- */

int rx_cmd_pack(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    rc = rx_dbf_pack(dbf);
    if (rc != R_OK) {
        return data_raise(ctx, s, rc, "PACK");
    }
    /* M6: PACK renumbers records — rebuild every tag */
    rc = rx_index_rebuild_all(ctx, dbf);
    if (rc != R_OK) {
        return data_raise(ctx, s, rc, "PACK");
    }
    rx_index_flush(dbf);
    return R_OK;
}

int rx_cmd_zap(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    rc = rx_dbf_zap(dbf);
    if (rc != R_OK) {
        return data_raise(ctx, s, rc, "ZAP");
    }
    /* M6: ZAP empties the table — clear every tag */
    {
        int nf = rx_index_file_count(dbf);
        for (int fi = 0; fi < nf; fi++) {
            int nt = rx_tag_count(dbf, fi);
            for (int ti = 0; ti < nt; ti++) {
                rx_index_clear(dbf, fi, ti);
            }
        }
        rx_index_flush(dbf);
    }
    return R_OK;
}

/* ---- REPLACE ---- */

typedef struct {
    rx_ctx* ctx;
    rx_stmt* s;
    rx_dbf* main_dbf;
    int exclusive;
    int nrec_done;
    int alias_area; /**< nonzero when an alias->field target was written (M6) */
} repl_ud;

static int find_area_by_alias(const char* alias) {
    for (int i = 1; i <= 255; i++) {
        if (rx_wa_used(i)) {
            const char* a;
            rx_wa_alias(i, &a);
            if (rx_strcasecmp(a, alias) == 0) {
                return i;
            }
        }
    }
    return 0;
}

static int cb_replace(rx_ctx* ctx, rx_dbf* dbf, uint32_t recno, void* ud) {
    repl_ud* u = ud;
    int rc;
    if (!u->exclusive) {
        if (rx_dbf_is_readonly(dbf)) {
            return R_ERR_IO_PERM; /* NOUPDATE: "File is read only" */
        }
        rc = rx_rlock(dbf, recno); /* spec \u00a78.5: RLOCK before REPLACE */
        if (rc != R_OK) {
            return rc;
        }
    }
    for (size_t i = 0; i < u->s->nclause; i++) {
        rx_clause* c = &u->s->clauses[i];
        if (strcmp(c->key, "REPL") != 0) {
            continue;
        }
        char fname[140];
        snprintf(fname, sizeof(fname), "%.139s", c->word);
        rx_dbf* target = dbf;
        char* arrow = strstr(fname, "->");
        if (arrow) {
            *arrow = '\0';
            int area = find_area_by_alias(fname);
            if (!area) {
                return rx_raise(ctx, R_ERR_FIELD, u->s->line, u->s->col, "Alias not found: %s",
                                fname);
            }
            rc = rx_wa_dbf(area, &target);
            if (rc != R_OK) {
                return rc;
            }
            /* alias->field writes at the alias work area's CURRENT record */
            memmove(fname, arrow + 2, strlen(arrow + 2) + 1);
            u->alias_area = area;
        }
        size_t fidx;
        if (rx_dbf_field_index(target, fname, &fidx) != R_OK) {
            return rx_raise(ctx, R_ERR_FIELD, u->s->line, u->s->col, "Unknown field %s", fname);
        }
        rval v;
        rx_rval_undef(&v);
        rc = rx_eval(ctx, c->e, &v, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        rc = rx_dbf_put(target, fidx, &v);
        rx_rval_free(&v);
        if (rc != R_OK) {
            return rc;
        }
        if (target != dbf) {
            rc = rx_flush(target); /* alias work area: flush before reposition */
            if (rc != R_OK) {
                return rc;
            }
        }
    }
    /* flush before iterate() repositions the pointer (goto clears dirty) */
    rc = rx_flush(dbf);
    if (rc != R_OK) {
        return rc;
    }
    /* M6 §8.4: re-insert the record into every attached index tag */
    rc = rx_index_maintain_rec(ctx, dbf, recno);
    if (rc != R_OK) {
        return rc;
    }
    if (u->alias_area) {
        /* index maintenance for the alias target at its current record */
        int saved = rx_wa_current();
        rx_wa_select(u->alias_area);
        rx_dbf* tdbf;
        if (rx_wa_dbf(u->alias_area, &tdbf) == R_OK) {
            uint32_t tr;
            rx_dbf_recno(tdbf, &tr);
            rc = rx_index_maintain_rec(ctx, tdbf, tr);
        }
        rx_wa_select(saved);
        if (rc != R_OK) {
            return rc;
        }
    }
    u->nrec_done++;
    return R_OK;
}

int rx_cmd_replace(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    int exclusive = rx_dbf_is_exclusive(dbf);
    repl_ud ud = {ctx, s, dbf, exclusive, 0, 0};
    rc = iterate(ctx, s, dbf, default_scope_m2(s, 3), cb_replace, &ud, NULL, 1);
    if (rc != R_OK) {
        return data_raise(ctx, s, rc, "REPLACE");
    }
    rx_flush(dbf);
    if (ctx->set.talk && ud.nrec_done) {
        rx_outf(ctx, "%d replacement%s.\n", ud.nrec_done, ud.nrec_done == 1 ? "" : "s");
    }
    return R_OK;
}

/* ---- LOCATE / CONTINUE ---- */

int rx_cmd_locate(rx_ctx* ctx, rx_stmt* s, int cont) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    uint32_t nrec;
    rx_dbf_recount(dbf, &nrec);
    rx_code for_code, while_code;
    int has_while = 0;
    memset(&for_code, 0, sizeof(for_code));
    memset(&while_code, 0, sizeof(while_code));
    uint32_t start, end;
    if (!cont) {
        rx_expr* for_e = NULL;
        for (size_t i = 0; i < s->nclause; i++) {
            if (strcmp(s->clauses[i].key, "FOR") == 0) {
                for_e = s->clauses[i].e;
            }
        }
        if (!for_e) {
            return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "LOCATE requires FOR");
        }
        ctx->locate_expr = for_e; /* borrowed from the stmt list */
        rx_scope sc;
        rc = eval_scope(ctx, s, 2 /* REST */, &sc);
        if (rc != R_OK) {
            return rc;
        }
        ctx->locate_kind = sc.kind;
        ctx->locate_n = sc.n;
        ctx->locate_active = 1;
        int restore;
        rc = rx_scope_range(dbf, &sc, &start, &end, &restore);
        if (rc != R_OK) {
            return rc;
        }
        rc = rx_compile_expr(for_e, &for_code, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        rc = compile_clause(ctx, s, "WHILE", &while_code, &has_while);
        if (rc != R_OK) {
            rx_code_free(&for_code);
            return rc;
        }
    } else {
        if (!ctx->locate_active || !ctx->locate_expr) {
            return rx_raise(ctx, R_ERR_NO_CONTINUE, s->line, s->col, "CONTINUE without LOCATE");
        }
        uint32_t cur;
        rx_dbf_recno(dbf, &cur);
        int eof;
        rx_dbf_eof(dbf, &eof);
        start = (eof ? nrec + 1 : cur) + 1;
        if (start < 1) {
            start = 1;
        }
        end = nrec;
        rc = rx_compile_expr(ctx->locate_expr, &for_code, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
    }
    int found = 0;
    uint32_t found_rec = 0;
    /* M6 §8.4: with a controlling order, LOCATE/CONTINUE scan in tag order */
    int ordered = rx_index_order_active(dbf);
    uint32_t* oseq = NULL;
    size_t onseq = 0;
    if (ordered) {
        int fi, ti;
        rx_index_order_get(dbf, &fi, &ti);
        uint32_t nk = 0;
        rx_index_key_count(dbf, fi, ti, &nk);
        int64_t from = 0, to = (int64_t)nk - 1;
        if (!cont) {
            int64_t curpos;
            int eof;
            rx_dbf_eof(dbf, &eof);
            uint32_t cur0;
            rx_dbf_recno(dbf, &cur0);
            if (eof) {
                curpos = (int64_t)nk;
            } else {
                int64_t pp = -1;
                rx_index_pos_of(dbf, cur0, &pp);
                curpos = pp >= 0 ? pp : (int64_t)(cur0 ? cur0 - 1 : 0);
            }
            if (ctx->locate_kind == 1) {
                from = 0;
            } else if (ctx->locate_kind == 3) {
                from = curpos;
                to = curpos + (int64_t)ctx->locate_n - 1;
            } else if (ctx->locate_kind == 4) {
                from = -2; /* RECORD n: physical single */
                to = -2;
            } else {
                from = curpos; /* REST */
            }
            if (from == -2) {
                onseq = ctx->locate_n >= 1 && ctx->locate_n <= nrec ? 1 : 0;
                oseq = malloc((onseq ? onseq : 1) * sizeof(uint32_t));
                if (!oseq) {
                    RX_FATAL("out of memory");
                }
                if (onseq) {
                    oseq[0] = ctx->locate_n;
                }
            }
        }
        if (!oseq) {
            if (cont) {
                uint32_t cur0;
                rx_dbf_recno(dbf, &cur0);
                int eof;
                rx_dbf_eof(dbf, &eof);
                int64_t pp = -1;
                if (!eof) {
                    rx_index_pos_of(dbf, cur0, &pp);
                }
                from = eof ? (int64_t)nk : (pp >= 0 ? pp + 1 : (int64_t)cur0);
            }
            if (to >= (int64_t)nk) {
                to = (int64_t)nk - 1;
            }
            if (from < 0) {
                from = 0;
            }
            onseq = to >= from ? (size_t)(to - from + 1) : 0;
            oseq = malloc((onseq ? onseq : 1) * sizeof(uint32_t));
            if (!oseq) {
                RX_FATAL("out of memory");
            }
            for (size_t i = 0; i < onseq; i++) {
                uint32_t r = 0;
                rx_index_recno_at(dbf, (uint32_t)(from + (int64_t)i), &r);
                oseq[i] = r;
            }
        }
    }
    size_t nscan = ordered ? onseq : (end >= start ? (size_t)(end - start + 1) : 0);
    for (size_t si = 0; si < nscan; si++) {
        uint32_t r = ordered ? oseq[si] : start + (uint32_t)si;
        if (r > nrec) {
            break;
        }
        rc = rx_dbf_goto(dbf, r);
        if (rc != R_OK) {
            break;
        }
        int del;
        rx_dbf_deleted(dbf, &del);
        if (ctx->set.deleted && del) {
            continue;
        }
        int pass;
        rc = eval_predicate(ctx, &for_code, &pass);
        if (rc != R_OK) {
            break;
        }
        if (!pass) {
            continue;
        }
        if (has_while) {
            rc = eval_predicate(ctx, &while_code, &pass);
            if (rc != R_OK) {
                break;
            }
            if (!pass) {
                break;
            }
        }
        found = 1;
        found_rec = r;
        break;
    }
    free(oseq);
    rx_code_free(&for_code);
    if (has_while) {
        rx_code_free(&while_code);
    }
    if (found) {
        rx_nav_goto(ctx, found_rec);
        rx_dbf_set_found(dbf, 1);
        if (ctx->set.talk) {
            rx_outf(ctx, "Record = %u\n", found_rec);
        }
    } else {
        if (nrec) {
            rx_dbf_goto(dbf, nrec);
            rx_dbf_skip(dbf, 1);
        } else {
            rx_dbf_goto(dbf, 1);
        }
        rx_dbf_set_found(dbf, 0);
    }
    return rc == R_OK ? R_OK : data_raise(ctx, s, rc, cont ? "CONTINUE" : "LOCATE");
}

/* ---- SCAN ... ENDSCAN (spec §7.1, M5) ---- */

typedef struct {
    rx_stmt* body; /* container stmt: kids/nkids hold the real body */
} scan_ud;

static int scan_cb(rx_ctx* ctx, rx_dbf* dbf, uint32_t recno, void* ud) {
    scan_ud* u = ud;
    (void)dbf;
    (void)recno;
    int rc = rx_exec_block(ctx, u->body->kids, u->body->nkids);
    if (rc != R_OK) {
        return rc;
    }
    if (ctx->returning || ctx->abort) {
        return RX_ITER_STOP; /* unwind: stop scanning, propagate control */
    }
    if (ctx->loop_ctl == RX_CTL_LOOP) {
        ctx->loop_ctl = RX_CTL_NONE;
        return R_OK;
    }
    if (ctx->loop_ctl == RX_CTL_EXIT) {
        return RX_ITER_STOP; /* leave EXIT set so enclosing loops unwind */
    }
    return R_OK;
}

int rx_cmd_scan(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    if (s->nkids < 1 || !s->kids[0]) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "SCAN without body");
    }
    scan_ud ud = {s->kids[0]};
    rc = iterate(ctx, s, dbf, 1 /* ALL */, scan_cb, &ud, NULL, 0);
    if (ctx->loop_ctl == RX_CTL_EXIT) {
        ctx->loop_ctl = RX_CTL_NONE; /* EXIT leaves the SCAN, not the program */
    }
    return rc;
}

/* ---- APPEND ---- */

static int resolve_read_path(rx_ctx* ctx, const char* name, const char* ext, char* out,
                             size_t outsz) {
    char cand[600];
    snprintf(cand, sizeof(cand), "%s", name);
    if (access(cand, F_OK) == 0) {
        snprintf(out, outsz, "%s", cand);
        return R_OK;
    }
    if (ext) {
        snprintf(cand, sizeof(cand), "%s.%s", name, ext);
        if (access(cand, F_OK) == 0) {
            snprintf(out, outsz, "%s", cand);
            return R_OK;
        }
    }
    if (ctx->set.defdir[0]) {
        snprintf(cand, sizeof(cand), "%s/%s", ctx->set.defdir, name);
        if (access(cand, F_OK) == 0) {
            snprintf(out, outsz, "%s", cand);
            return R_OK;
        }
        if (ext) {
            snprintf(cand, sizeof(cand), "%s/%s.%s", ctx->set.defdir, name, ext);
            if (access(cand, F_OK) == 0) {
                snprintf(out, outsz, "%s", cand);
                return R_OK;
            }
        }
    }
    if (ctx->set.path[0]) {
        char pathcopy[1024];
        snprintf(pathcopy, sizeof(pathcopy), "%s", ctx->set.path);
        char* save = NULL;
        for (char* dir = strtok_r(pathcopy, ",;", &save); dir; dir = strtok_r(NULL, ",;", &save)) {
            snprintf(cand, sizeof(cand), "%s/%s", dir, name);
            if (access(cand, F_OK) == 0) {
                snprintf(out, outsz, "%s", cand);
                return R_OK;
            }
            if (ext) {
                snprintf(cand, sizeof(cand), "%s/%s.%s", dir, name, ext);
                if (access(cand, F_OK) == 0) {
                    snprintf(out, outsz, "%s", cand);
                    return R_OK;
                }
            }
        }
    }
    return R_ERR_IO_NOTFOUND;
}

static int append_from_text(rx_ctx* ctx, rx_stmt* s, rx_dbf* dbf, const char* path, int is_sdf,
                            char delim, int quote) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "File %s not found", path);
    }
    strbuf sb;
    sb_init(&sb);
    char buf[8192];
    ssize_t k;
    while ((k = read(fd, buf, sizeof(buf))) > 0) {
        sb_put(&sb, buf, (size_t)k);
    }
    close(fd);
    if (k < 0) {
        sb_free(&sb);
        return rx_raise(ctx, R_ERR_IO_READ, s->line, s->col, "Cannot read %s", path);
    }
    int exclusive = rx_dbf_is_exclusive(dbf);
    if (!exclusive) {
        int rc = rx_flock(dbf);
        if (rc != R_OK) {
            sb_free(&sb);
            return data_raise(ctx, s, rc, "APPEND FROM");
        }
    }
    size_t nf;
    rx_dbf_field_count(dbf, &nf);
    size_t* flds = malloc(nf * sizeof(size_t));
    if (!flds) {
        sb_free(&sb);
        RX_FATAL("out of memory");
    }
    for (size_t i = 0; i < nf; i++) {
        flds[i] = i;
    }
    uint16_t rsz = 0;
    rx_dbf_recsize(dbf, &rsz);
    uint8_t* rec = malloc(rsz);
    if (!rec) {
        free(flds);
        sb_free(&sb);
        RX_FATAL("out of memory");
    }
    int n = 0;
    size_t pos = 0, n_all = sb.len;
    char* data = sb.buf ? sb.buf : (char*)"";
    while (pos < n_all) {
        size_t eol = pos;
        while (eol < n_all && data[eol] != '\n') {
            eol++;
        }
        size_t len = eol - pos;
        if (len > 0 && data[pos + len - 1] == '\r') {
            len--;
        }
        int last = (eol >= n_all);
        if (last && len == 0) {
            break; /* trailing newline: no extra record */
        }
        uint32_t recno;
        int rc = rx_dbf_append(dbf, &recno);
        if (rc != R_OK) {
            free(rec);
            free(flds);
            sb_free(&sb);
            return data_raise(ctx, s, rc, "APPEND FROM");
        }
        memset(rec, ' ', rsz);
        rec[0] = 0x20;
        if (is_sdf) {
            rx_sdf_parse_line(dbf, flds, nf, data + pos, len, rec);
        } else {
            rx_delim_parse_line(dbf, flds, nf, data + pos, len, delim, quote, rec);
        }
        rc = rx_dbf_put_raw(dbf, rec);
        if (rc == R_OK) {
            rc = rx_flush(dbf);
        }
        if (rc == R_OK) {
            rc = rx_index_maintain_rec(ctx, dbf, recno); /* M6 index maintenance */
        }
        if (rc != R_OK) {
            free(rec);
            free(flds);
            sb_free(&sb);
            return data_raise(ctx, s, rc, "APPEND FROM");
        }
        n++;
        pos = eol + 1;
    }
    free(rec);
    free(flds);
    sb_free(&sb);
    if (!exclusive) {
        rx_funlock(dbf);
    }
    if (ctx->set.talk) {
        rx_outf(ctx, "%d record%s added.\n", n, n == 1 ? "" : "s");
    }
    return R_OK;
}

static int append_from_dbf(rx_ctx* ctx, rx_stmt* s, rx_dbf* dbf, const char* path) {
    rx_dbf* src = NULL;
    int rc = rx_dbf_open(path, RD_READONLY, &src);
    if (rc != R_OK) {
        return data_raise(ctx, s, rc, path);
    }
    int exclusive = rx_dbf_is_exclusive(dbf);
    if (!exclusive) {
        rc = rx_flock(dbf);
        if (rc != R_OK) {
            rx_dbf_close(src);
            return data_raise(ctx, s, rc, "APPEND FROM");
        }
    }
    size_t nfdst, nfsrc;
    rx_dbf_field_count(dbf, &nfdst);
    rx_dbf_field_count(src, &nfsrc);
    uint32_t nrec;
    rx_dbf_recount(src, &nrec);
    int n = 0;
    for (uint32_t r = 1; r <= nrec; r++) {
        rc = rx_dbf_goto(src, r);
        if (rc != R_OK) {
            break;
        }
        int del;
        rx_dbf_deleted(src, &del);
        if (ctx->set.deleted && del) {
            continue;
        }
        uint32_t recno;
        rc = rx_dbf_append(dbf, &recno);
        if (rc != R_OK) {
            break;
        }
        for (size_t fi = 0; fi < nfdst; fi++) {
            rx_fielddef fd;
            rx_dbf_field_def(dbf, fi, &fd);
            size_t sidx;
            if (rx_dbf_field_index(src, fd.name, &sidx) != R_OK) {
                continue; /* field not in source: stays blank */
            }
            rval v;
            rx_rval_undef(&v);
            rc = rx_dbf_get(src, sidx, &v);
            if (rc != R_OK) {
                rx_rval_free(&v);
                break;
            }
            rc = rx_dbf_put(dbf, fi, &v);
            rx_rval_free(&v);
            if (rc != R_OK) {
                break;
            }
        }
        if (rc == R_OK) {
            rc = rx_flush(dbf);
        }
        if (rc == R_OK) {
            rc = rx_index_maintain_rec(ctx, dbf, recno); /* M6 index maintenance */
        }
        if (rc != R_OK) {
            break;
        }
        n++;
    }
    if (!exclusive) {
        rx_funlock(dbf);
    }
    rx_dbf_close(src);
    if (rc != R_OK) {
        return data_raise(ctx, s, rc, "APPEND FROM");
    }
    if (ctx->set.talk) {
        rx_outf(ctx, "%d record%s added.\n", n, n == 1 ? "" : "s");
    }
    return R_OK;
}

int rx_cmd_append(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    if (s->nstrs == 0) {
        /* bare APPEND: interactive opens the append-mode Browse grid (M4) */
        if (!ctx->browse_fn) {
            return rx_raise(ctx, R_ERR_NOUI, s->line, s->col, "APPEND requires the interactive UI");
        }
        rx_stmt bs;
        memset(&bs, 0, sizeof(bs));
        bs.kind = ST_BROWSE;
        bs.line = s->line;
        bs.col = s->col;
        rx_stmt_add_clause(&bs, "APPENDMODE", "1", NULL);
        int brc = ctx->browse_fn(ctx->browse_ud, &bs);
        rx_stmt_free(&bs);
        return brc;
    }
    if (rx_strcasecmp(s->strs[0], "BLANK") == 0) {
        int exclusive = rx_dbf_is_exclusive(dbf);
        if (!exclusive) {
            rc = rx_flock(dbf);
            if (rc != R_OK) {
                return data_raise(ctx, s, rc, "APPEND BLANK");
            }
        }
        uint32_t recno;
        rc = rx_dbf_append(dbf, &recno);
        if (!exclusive) {
            rx_funlock(dbf);
        }
        if (rc != R_OK) {
            return data_raise(ctx, s, rc, "APPEND BLANK");
        }
        rc = rx_index_maintain_rec(ctx, dbf, recno); /* M6: blank keys inserted */
        if (rc != R_OK) {
            return data_raise(ctx, s, rc, "APPEND BLANK");
        }
        return R_OK;
    }
    if (rx_strcasecmp(s->strs[0], "APPEND") == 0) {
        /* bare APPEND (parser echoed the keyword): append-mode Browse (M4) */
        if (!ctx->browse_fn) {
            return rx_raise(ctx, R_ERR_NOUI, s->line, s->col, "APPEND requires the interactive UI");
        }
        rx_stmt bs;
        memset(&bs, 0, sizeof(bs));
        bs.kind = ST_BROWSE;
        bs.line = s->line;
        bs.col = s->col;
        rx_stmt_add_clause(&bs, "APPENDMODE", "1", NULL);
        int brc = ctx->browse_fn(ctx->browse_ud, &bs);
        rx_stmt_free(&bs);
        return brc;
    }
    /* APPEND FROM */
    const char* type = NULL;
    const char* with = NULL;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "TYPE") == 0) {
            type = s->clauses[i].word;
        } else if (strcmp(s->clauses[i].key, "WITH") == 0) {
            with = s->clauses[i].word;
        }
    }
    char path[600];
    int is_sdf = 0;
    const char* ext = NULL;
    char delim = ',';
    int quote = 1;
    if (type && rx_strcasecmp(type, "SDF") == 0) {
        is_sdf = 1;
        ext = "txt";
    } else if (type && rx_strcasecmp(type, "DELIMITED") == 0) {
        ext = "txt";
    } else if (!type) {
        ext = "dbf"; /* APPEND FROM with no TYPE reads a DBF */
    } else {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col,
                        "APPEND FROM type must be SDF, DELIMITED or a table");
    }
    if (with) {
        if (rx_strcasecmp(with, "BLANK") == 0) {
            delim = ' ';
            quote = 0;
        } else if (rx_strcasecmp(with, "TAB") == 0) {
            delim = '\t';
            quote = 1;
        } else if (with[0]) {
            delim = with[0];
            quote = 1;
        }
    }
    rc = resolve_read_path(ctx, s->strs[0], ext, path, sizeof(path));
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "File %s not found", s->strs[0]);
    }
    if (ext && strcmp(ext, "dbf") == 0) {
        return append_from_dbf(ctx, s, dbf, path);
    }
    return append_from_text(ctx, s, dbf, path, is_sdf, delim, quote);
}

/* ---- COPY TO / COPY STRUCTURE ---- */

typedef struct {
    rx_ctx* ctx;
    rx_stmt* s;
    rx_dbf* src;
    int fd;      /* text file, -1 when DBF mode */
    rx_dbf* dst; /* DBF mode target */
    const size_t* flds;
    size_t nf;
    int is_sdf;
    char delim;
    int quote;
    int n;
} copy_ud;

static int cb_copy(rx_ctx* ctx, rx_dbf* dbf, uint32_t recno, void* ud) {
    (void)recno;
    copy_ud* u = ud;
    if (u->fd >= 0) {
        uint16_t rsz = 0;
        rx_dbf_recsize(dbf, &rsz);
        char* line = malloc((size_t)rsz + 16);
        if (!line) {
            RX_FATAL("out of memory");
        }
        size_t ln = 0;
        int rc;
        const uint8_t* raw = rx_dbf_recbuf(dbf);
        if (u->is_sdf) {
            rc = rx_sdf_write_line(dbf, u->flds, u->nf, raw, line, (size_t)rsz + 16, &ln);
        } else {
            rc = rx_delim_write_line(dbf, u->flds, u->nf, raw, u->delim, u->quote, line,
                                     (size_t)rsz + 16, &ln);
        }
        if (rc == R_OK) {
            ssize_t w = write(u->fd, line, ln);
            if (w != (ssize_t)ln) {
                rc = R_ERR_IO_WRITE;
            }
        }
        free(line);
        if (rc != R_OK) {
            return rc;
        }
    } else {
        uint32_t nr;
        int rc = rx_dbf_append(u->dst, &nr);
        if (rc != R_OK) {
            return rc;
        }
        for (size_t k = 0; k < u->nf; k++) {
            size_t fi = u->flds[k];
            rval v;
            rx_rval_undef(&v);
            rc = rx_dbf_get(dbf, fi, &v);
            if (rc != R_OK) {
                rx_rval_free(&v);
                return rc;
            }
            /* map to the target's field with the same name */
            rx_fielddef fd;
            rx_dbf_field_def(dbf, fi, &fd);
            size_t tidx;
            if (rx_dbf_field_index(u->dst, fd.name, &tidx) != R_OK) {
                rx_rval_free(&v);
                continue;
            }
            rc = rx_dbf_put(u->dst, tidx, &v);
            rx_rval_free(&v);
            if (rc != R_OK) {
                return rc;
            }
        }
        rc = rx_flush(u->dst);
        if (rc != R_OK) {
            return rc;
        }
        rc = rx_index_maintain_rec(u->ctx, u->dst, nr); /* M6 */
        if (rc != R_OK) {
            return rc;
        }
    }
    u->n++;
    (void)ctx;
    return R_OK;
}

int rx_cmd_copy(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    int structure = 0, extended = 0;
    const char* target_str = NULL;
    for (size_t i = 0; i < s->nstrs; i++) {
        if (rx_strcasecmp(s->strs[i], "STRUCTURE") == 0) {
            structure = 1;
        } else if (rx_strcasecmp(s->strs[i], "EXTENDED") == 0) {
            extended = 1;
        } else if (!target_str) {
            target_str = s->strs[i]; /* first non-keyword word is the path */
        }
    }
    const char* type = NULL;
    const char* with = NULL;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "TYPE") == 0) {
            type = s->clauses[i].word;
        } else if (strcmp(s->clauses[i].key, "WITH") == 0) {
            with = s->clauses[i].word;
        }
    }
    if (!target_str) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "COPY requires TO <file>");
    }
    char target[600];
    snprintf(target, sizeof(target), "%s", target_str);
    int is_sdf = 0, is_delim = 0;
    char delim = ',', quote = 1;
    if (type && rx_strcasecmp(type, "SDF") == 0) {
        is_sdf = 1;
    } else if (type && rx_strcasecmp(type, "DELIMITED") == 0) {
        is_delim = 1;
    } else if (type) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col,
                        "COPY TO type must be SDF or DELIMITED");
    }
    if (with) {
        if (rx_strcasecmp(with, "BLANK") == 0) {
            delim = ' ';
            quote = 0;
        } else if (rx_strcasecmp(with, "TAB") == 0) {
            delim = '\t';
        } else if (with[0]) {
            delim = with[0];
        }
    }
    if (!strchr(target, '.')) {
        /* FoxPro default extensions: .dbf for tables, .txt for SDF/DELIMITED */
        strncat(target, (is_sdf || is_delim) ? ".txt" : ".dbf",
                sizeof(target) - strlen(target) - 1);
    }
    /* SET SAFETY: refuse to overwrite an existing file */
    if (access(target, F_OK) == 0 && ctx->set.safety) {
        return rx_raise(ctx, R_ERR_IO_PERM, s->line, s->col, "File %s already exists", target);
    }

    size_t* flds;
    size_t nsel;
    rc = select_fields(ctx, s, dbf, &flds, &nsel);
    if (rc != R_OK) {
        return rc;
    }

    if (structure) {
        /* COPY STRUCTURE TO: create the target with the selected layout
           (EXTENDED: the standard FIELD_NAME/FIELD_TYPE/FIELD_LEN/FIELD_DEC) */
        rx_fielddef* defs;
        size_t ndef;
        rx_fielddef ext[4] = {
            {"FIELD_NAME", RFT_C, 10, 0, 0},
            {"FIELD_TYPE", RFT_C, 1, 0, 0},
            {"FIELD_LEN", RFT_N, 3, 0, 0},
            {"FIELD_DEC", RFT_N, 3, 0, 0},
        };
        if (extended) {
            defs = ext;
            ndef = 4;
        } else {
            defs = malloc(nsel * sizeof(rx_fielddef));
            if (!defs) {
                free(flds);
                RX_FATAL("out of memory");
            }
            for (size_t k = 0; k < nsel; k++) {
                rx_dbf_field_def(dbf, flds[k], &defs[k]);
            }
            ndef = nsel;
        }
        uint8_t version;
        int y, m, d;
        rx_dbf_lupdate(dbf, &y, &m, &d);
        rx_dbf* nd = NULL;
        /* preserve the source version/mark so round trips stay identical */
        uint8_t ver = 0x03, mark = 0x01;
        rx_dbf_version_mark(dbf, &ver, &mark);
        version = ver;
        rc = rx_dbf_create_full(target, defs, ndef, version, mark, y, m, d, &nd);
        if (rc != R_OK) {
            if (!extended) {
                free(defs);
            }
            free(flds);
            return data_raise(ctx, s, rc, target);
        }
        if (extended) {
            /* one record per source field, written into the 4-field layout */
            for (size_t k = 0; k < nsel; k++) {
                rx_fielddef sf;
                rx_dbf_field_def(dbf, flds[k], &sf);
                uint32_t nr;
                rc = rx_dbf_append(nd, &nr);
                if (rc != R_OK) {
                    break;
                }
                char nm[11];
                snprintf(nm, sizeof(nm), "%-10s", sf.name);
                rval v;
                rx_rval_undef(&v);
                rval_set_str(&v, rx_strdup(nm), 10);
                rc = rx_dbf_put(nd, 0, &v);
                rx_rval_free(&v);
                if (rc != R_OK) {
                    break;
                }
                char ty[2] = {(char)sf.type, '\0'};
                rval_set_str(&v, rx_strdup(ty), 1);
                rc = rx_dbf_put(nd, 1, &v);
                rx_rval_free(&v);
                if (rc != R_OK) {
                    break;
                }
                char num[8];
                snprintf(num, sizeof(num), "%3u", sf.len);
                rval_set_str(&v, rx_strdup(num), 3);
                rc = rx_dbf_put(nd, 2, &v);
                rx_rval_free(&v);
                if (rc != R_OK) {
                    break;
                }
                snprintf(num, sizeof(num), "%3u", sf.dec);
                rval_set_str(&v, rx_strdup(num), 3);
                rc = rx_dbf_put(nd, 3, &v);
                rx_rval_free(&v);
                if (rc != R_OK) {
                    break;
                }
                rc = rx_flush(nd);
                if (rc != R_OK) {
                    break;
                }
            }
        }
        rx_dbf_close(nd);
        if (!extended) {
            free(defs);
        }
        free(flds);
        if (rc != R_OK) {
            return data_raise(ctx, s, rc, target);
        }
        return R_OK;
    }

    if (is_sdf || is_delim) {
        int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            free(flds);
            return rx_raise(ctx, R_ERR_IO_OPEN, s->line, s->col, "Cannot create %s", target);
        }
        copy_ud ud = {ctx, s, dbf, fd, NULL, flds, nsel, is_sdf, delim, quote, 0};
        rc = iterate(ctx, s, dbf, 1 /* ALL */, cb_copy, &ud, NULL, 0);
        close(fd);
        free(flds);
        if (rc != R_OK) {
            return data_raise(ctx, s, rc, "COPY TO");
        }
        if (ctx->set.talk) {
            rx_outf(ctx, "%d record%s copied.\n", ud.n, ud.n == 1 ? "" : "s");
        }
        return R_OK;
    }

    /* COPY TO <dbf>: create the target and copy matching records */
    rx_fielddef* defs = malloc(nsel * sizeof(rx_fielddef));
    if (!defs) {
        free(flds);
        RX_FATAL("out of memory");
    }
    for (size_t k = 0; k < nsel; k++) {
        rx_dbf_field_def(dbf, flds[k], &defs[k]);
    }
    uint8_t ver = 0x03, mark = 0x01;
    rx_dbf_version_mark(dbf, &ver, &mark);
    int y, m, d;
    rx_dbf_lupdate(dbf, &y, &m, &d);
    rx_dbf* nd = NULL;
    rc = rx_dbf_create_full(target, defs, nsel, ver, mark, y, m, d, &nd);
    free(defs);
    if (rc != R_OK) {
        free(flds);
        return data_raise(ctx, s, rc, target);
    }
    copy_ud ud = {ctx, s, dbf, -1, nd, flds, nsel, 0, ',', 1, 0};
    rc = iterate(ctx, s, dbf, 1 /* ALL */, cb_copy, &ud, NULL, 0);
    rx_dbf_close(nd);
    free(flds);
    if (rc != R_OK) {
        return data_raise(ctx, s, rc, "COPY TO");
    }
    if (ctx->set.talk) {
        rx_outf(ctx, "%d record%s copied.\n", ud.n, ud.n == 1 ? "" : "s");
    }
    return R_OK;
}

/* ---- CREATE ---- */

int rx_cmd_create(rx_ctx* ctx, rx_stmt* s) {
    if (s->nstrs == 0) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "CREATE requires a table name");
    }
    char target[600];
    snprintf(target, sizeof(target), "%s", s->strs[0]);
    if (!strchr(target, '.')) {
        strncat(target, ".dbf", sizeof(target) - strlen(target) - 1);
    }
    if (access(target, F_OK) == 0) {
        return rx_raise(ctx, R_ERR_IO_PERM, s->line, s->col, "File %s already exists", target);
    }
    if (s->nstrs >= 2) {
        /* CREATE <new> FROM <structure-dbf> (COPY STRUCTURE EXTENDED output) */
        char spath[600];
        int rc = resolve_table(ctx, s->strs[1], spath, sizeof(spath));
        if (rc != R_OK) {
            return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "File %s not found",
                            s->strs[1]);
        }
        rx_dbf* sd = NULL;
        rc = rx_dbf_open(spath, RD_READONLY, &sd);
        if (rc != R_OK) {
            return data_raise(ctx, s, rc, spath);
        }
        size_t nf;
        rx_dbf_field_count(sd, &nf);
        size_t iname, itype, ilen, idec;
        int ok = rx_dbf_field_index(sd, "FIELD_NAME", &iname) == R_OK &&
                 rx_dbf_field_index(sd, "FIELD_TYPE", &itype) == R_OK &&
                 rx_dbf_field_index(sd, "FIELD_LEN", &ilen) == R_OK &&
                 rx_dbf_field_index(sd, "FIELD_DEC", &idec) == R_OK;
        if (!ok) {
            rx_dbf_close(sd);
            return rx_raise(ctx, R_ERR_DBF_FIELDS, s->line, s->col,
                            "%s is not a COPY STRUCTURE EXTENDED file", spath);
        }
        uint32_t nrec;
        rx_dbf_recount(sd, &nrec);
        rx_fielddef* defs = calloc(nrec ? nrec : 1, sizeof(rx_fielddef));
        if (!defs) {
            rx_dbf_close(sd);
            RX_FATAL("out of memory");
        }
        size_t nd = 0;
        int rc2 = R_OK;
        for (uint32_t r = 1; r <= nrec && rc2 == R_OK; r++) {
            rc2 = rx_dbf_goto(sd, r);
            if (rc2 != R_OK) {
                break;
            }
            rval vn, vt, vl, vd;
            rx_rval_undef(&vn);
            rx_rval_undef(&vt);
            rx_rval_undef(&vl);
            rx_rval_undef(&vd);
            rc2 = rx_dbf_get(sd, iname, &vn);
            if (rc2 == R_OK) {
                rc2 = rx_dbf_get(sd, itype, &vt);
            }
            if (rc2 == R_OK) {
                rc2 = rx_dbf_get(sd, ilen, &vl);
            }
            if (rc2 == R_OK) {
                rc2 = rx_dbf_get(sd, idec, &vd);
            }
            if (rc2 == R_OK) {
                const char* s;
                size_t sn;
                if (rv_to_str(&vn, &s, &sn)) {
                    while (sn > 0 && s[sn - 1] == ' ') {
                        sn--;
                    }
                    if (sn > 0 && sn < 11) {
                        rx_fielddef* f = &defs[nd++];
                        memset(f, 0, sizeof(*f));
                        memcpy(f->name, s, sn);
                        f->name[sn] = '\0';
                        rx_strtoupper(f->name);
                        double td;
                        const char* ts;
                        size_t tn;
                        if (rv_to_str(&vt, &ts, &tn) && tn >= 1) {
                            f->type = (uint8_t)ts[0];
                        }
                        if (rv_to_num(&vl, &td)) {
                            f->len = (uint8_t)td;
                        }
                        if (rv_to_num(&vd, &td)) {
                            f->dec = (uint8_t)td;
                        }
                    }
                }
            }
            rx_rval_free(&vn);
            rx_rval_free(&vt);
            rx_rval_free(&vl);
            rx_rval_free(&vd);
        }
        rx_dbf_close(sd);
        if (rc2 != R_OK) {
            free(defs);
            return data_raise(ctx, s, rc2, spath);
        }
        if (nd == 0) {
            free(defs);
            return rx_raise(ctx, R_ERR_DBF_FIELDS, s->line, s->col, "Structure file has no fields");
        }
        rx_dbf* nd2 = NULL;
        rc = rx_dbf_create(target, defs, nd, &nd2);
        free(defs);
        if (rc != R_OK) {
            return data_raise(ctx, s, rc, target);
        }
        rx_dbf_close(nd2);
        return R_OK;
    }
    return rx_raise(ctx, R_ERR_NOUI, s->line, s->col,
                    "CREATE's interactive table builder needs the UI (M3+); "
                    "in batch use CREATE ... FROM or APPEND FROM");
}

/* ---- FLUSH / TYPE / UNLOCK ---- */

int rx_cmd_flush(rx_ctx* ctx, rx_stmt* s) {
    (void)s;
    for (int a = 1; a <= 255; a++) {
        if (rx_wa_used(a)) {
            rx_dbf* d;
            if (rx_wa_dbf(a, &d) == R_OK) {
                int rc = rx_flush(d);
                if (rc != R_OK) {
                    return data_raise(ctx, s, rc, "FLUSH");
                }
                rc = rx_index_flush(d); /* M6: write dirty index files */
                if (rc != R_OK) {
                    return data_raise(ctx, s, rc, "FLUSH");
                }
            }
        }
    }
    return R_OK;
}

int rx_cmd_type(rx_ctx* ctx, rx_stmt* s) {
    if (s->nstrs == 0) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "TYPE requires a file name");
    }
    char path[600];
    int rc = resolve_read_path(ctx, s->strs[0], NULL, path, sizeof(path));
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "File %s not found", s->strs[0]);
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return rx_raise(ctx, R_ERR_IO_OPEN, s->line, s->col, "Cannot open %s", path);
    }
    char buf[8192];
    ssize_t k;
    while ((k = read(fd, buf, sizeof(buf))) > 0) {
        rx_out(ctx, buf, (size_t)k);
    }
    close(fd);
    if (k < 0) {
        return rx_raise(ctx, R_ERR_IO_READ, s->line, s->col, "Cannot read %s", path);
    }
    return R_OK;
}

int rx_cmd_unlock(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    int all = 0;
    for (size_t i = 0; i < s->nstrs; i++) {
        if (rx_strcasecmp(s->strs[i], "ALL") == 0) {
            all = 1;
        }
    }
    if (all) {
        for (int a = 1; a <= 255; a++) {
            if (rx_wa_used(a)) {
                rx_dbf* d;
                if (rx_wa_dbf(a, &d) == R_OK) {
                    rx_unlock(d, 0);
                }
            }
        }
        return R_OK;
    }
    rx_unlock(dbf, 0);
    return R_OK;
}

/* ---- M7 exports (cmds_aggr.c shares scope iteration) ---- */
int rx_iterate(rx_ctx* ctx, rx_stmt* s, rx_dbf* dbf, int default_kind, rx_rec_cb cb, void* ud,
               int* count) {
    return iterate(ctx, s, dbf, default_kind, cb, ud, count, 0);
}

int rx_default_scope(rx_stmt* s, int dflt) {
    return default_scope_m2(s, dflt);
}
