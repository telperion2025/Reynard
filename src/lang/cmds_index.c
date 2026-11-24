/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_index.c
 * @brief Index commands (spec §7.1 + §8.4, M6): INDEX ON ... TO/TAG,
 *        SET INDEX TO, SET ORDER TO, SEEK, REINDEX, plus the maintenance
 *        hooks (REPLACE/APPEND/PACK/ZAP keep tags current) and ordered
 *        navigation (GO TOP/BOTTOM, SKIP follow the controlling order).
 *
 * The data layer owns the sorted entry arrays and the on-disk trees; this
 * file owns expression evaluation: tag key/FOR expressions are stored as
 * text in the tag headers and re-compiled once per session into the ctx
 * registry (rx_idxexpr), then evaluated per record with the VM.
 */
/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "lang/lang.h"
#include "lang/funcs.h"
#include "reynard/dbf.h"
#include "util/util.h"

/* ============================ path resolution ============================ */

static int has_any_ext(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strchr(base, '.') != NULL;
}

static void ensure_ext(const char *in, const char *ext, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s", in);
    if (!has_any_ext(in)) {
        strncat(out, ext, outsz - strlen(out) - 1);
    }
}

/* resolve an index file name per SET DEFAULT / SET PATH (spec §10.3),
   trying the as-given path and the .idx/.cdx/.ntx/.ndx extensions */
int resolve_index_m6(rx_ctx *ctx, const char *name, char *out, size_t outsz)
{
    static const char *exts[] = { "", ".idx", ".cdx", ".ntx", ".ndx", NULL };
    char cand[600];
    for (int e = 0; exts[e]; e++) {
        snprintf(cand, sizeof(cand), "%s%s", name, exts[e]);
        if (access(cand, F_OK) == 0) {
            snprintf(out, outsz, "%s", cand);
            return R_OK;
        }
    }
    if (ctx->set.defdir[0]) {
        for (int e = 0; exts[e]; e++) {
            snprintf(cand, sizeof(cand), "%s/%s%s", ctx->set.defdir, name, exts[e]);
            if (access(cand, F_OK) == 0) {
                snprintf(out, outsz, "%s", cand);
                return R_OK;
            }
        }
    }
    if (ctx->set.path[0]) {
        char pathcopy[1024];
        snprintf(pathcopy, sizeof(pathcopy), "%s", ctx->set.path);
        char *save = NULL;
        for (char *dir = strtok_r(pathcopy, ",;", &save); dir;
             dir = strtok_r(NULL, ",;", &save)) {
            for (int e = 0; exts[e]; e++) {
                snprintf(cand, sizeof(cand), "%s/%s%s", dir, name, exts[e]);
                if (access(cand, F_OK) == 0) {
                    snprintf(out, outsz, "%s", cand);
                    return R_OK;
                }
            }
        }
    }
    /* not found on disk: allow create paths (INDEX ON ... TO newfile) */
    snprintf(cand, sizeof(cand), "%s", name);
    if (!has_any_ext(cand) && ctx->set.defdir[0]) {
        snprintf(out, outsz, "%s/%s", ctx->set.defdir, cand);
    } else {
        snprintf(out, outsz, "%s", cand);
    }
    return R_ERR_IO_NOTFOUND;
}

/* ============================ registry (key expr AST cache) ============================ */

static rx_idxexpr *reg_find(rx_ctx *ctx, const char *dbfpath, const char *path, const char *tag)
{
    for (size_t i = 0; i < ctx->nidxexprs; i++) {
        rx_idxexpr *x = &ctx->idxexprs[i];
        if (strcmp(x->dbf, dbfpath) == 0 && rx_strcasecmp(x->path, path) == 0 &&
            rx_strcasecmp(x->tag, tag) == 0) {
            return x;
        }
    }
    return NULL;
}

static rx_idxexpr *reg_add(rx_ctx *ctx, const char *dbfpath, const char *path, const char *tag,
                           const char *keytext, const char *fortext)
{
    if (ctx->nidxexprs == ctx->capidxexprs) {
        size_t nc = ctx->capidxexprs ? ctx->capidxexprs * 2 : 8;
        rx_idxexpr *nx = realloc(ctx->idxexprs, nc * sizeof(rx_idxexpr));
        if (!nx) {
            RX_FATAL("out of memory");
        }
        ctx->idxexprs = nx;
        ctx->capidxexprs = nc;
    }
    rx_idxexpr *x = &ctx->idxexprs[ctx->nidxexprs++];
    memset(x, 0, sizeof(*x));
    snprintf(x->dbf, sizeof(x->dbf), "%s", dbfpath);
    snprintf(x->path, sizeof(x->path), "%s", path);
    snprintf(x->tag, sizeof(x->tag), "%s", tag);
    for (char *c = x->tag; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    x->keytext = rx_strdup(keytext ? keytext : "");
    x->fortext = (fortext && fortext[0]) ? rx_strdup(fortext) : NULL;
    return x;
}

void rx_index_registry_purge(rx_ctx *ctx, const char *dbf_path)
{
    size_t w = 0;
    for (size_t i = 0; i < ctx->nidxexprs; i++) {
        if (strcmp(ctx->idxexprs[i].dbf, dbf_path) == 0) {
            rx_expr_free(ctx->idxexprs[i].key_e);
            rx_expr_free(ctx->idxexprs[i].for_e);
            free(ctx->idxexprs[i].keytext);
            free(ctx->idxexprs[i].fortext);
        } else {
            ctx->idxexprs[w++] = ctx->idxexprs[i];
        }
    }
    ctx->nidxexprs = w;
}

/* find (or create from the tag header text) the registry entry for (fi, ti)
   and make sure its ASTs are parsed */
static rx_idxexpr *reg_ensure(rx_ctx *ctx, rx_dbf *dbf, int fi, int ti)
{
    const char *name = "", *key = "", *fore = "", *fpath = "", *dpath = "";
    rx_tag_info(dbf, fi, ti, &name, &key, NULL);
    rx_tag_for_expr(dbf, fi, ti, &fore);
    rx_index_file_path(dbf, fi, &fpath);
    rx_dbf_path(dbf, &dpath);
    rx_idxexpr *x = reg_find(ctx, dpath, fpath, name);
    if (!x) {
        x = reg_add(ctx, dpath, fpath, name, key, fore);
    }
    if (!x->parsed) {
        r_err err;
        memset(&err, 0, sizeof(err));
        if (x->keytext[0]) {
            int rc = rx_parse_expr_str(x->keytext, &x->key_e, &err);
            if (rc != R_OK) {
                ctx->err.code = err.code;
                snprintf(ctx->err.msg, sizeof(ctx->err.msg), "%s", err.msg);
                return NULL;
            }
        }
        if (x->fortext) {
            int rc = rx_parse_expr_str(x->fortext, &x->for_e, &err);
            if (rc != R_OK) {
                ctx->err.code = err.code;
                snprintf(ctx->err.msg, sizeof(ctx->err.msg), "%s", err.msg);
                return NULL;
            }
        }
        x->parsed = 1;
    }
    return x;
}

/* ============================ tag lookup helpers ============================ */

int rx_index_find_tag(rx_dbf *dbf, const char *path, const char *tag, int *fi_out, int *ti_out)
{
    int nf = rx_index_file_count(dbf);
    for (int fi = 0; fi < nf; fi++) {
        const char *fp = NULL;
        rx_index_file_path(dbf, fi, &fp);
        if (path && rx_strcasecmp(fp, path) != 0) {
            continue;
        }
        int nt = rx_tag_count(dbf, fi);
        for (int ti = 0; ti < nt; ti++) {
            const char *name = NULL;
            rx_tag_info(dbf, fi, ti, &name, NULL, NULL);
            if (rx_strcasecmp(name, tag) == 0) {
                *fi_out = fi;
                *ti_out = ti;
                return R_OK;
            }
        }
        if (path) {
            break; /* named file searched, tag not in it */
        }
    }
    return R_ERR_CDX_TAG;
}

int rx_index_order_active(const rx_dbf *dbf)
{
    int fi, ti;
    rx_index_order_get(dbf, &fi, &ti);
    return fi >= 0;
}

static uint32_t order_key_count(rx_dbf *dbf)
{
    int fi, ti;
    rx_index_order_get(dbf, &fi, &ti);
    uint32_t n = 0;
    if (fi >= 0) {
        rx_index_key_count(dbf, fi, ti, &n);
    }
    return n;
}

/* first tag overall (structural CDX is slot 0), or (-1,-1) when none */
void rx_index_default_order(rx_dbf *dbf)
{
    int nf = rx_index_file_count(dbf);
    for (int fi = 0; fi < nf; fi++) {
        int nt = rx_tag_count(dbf, fi);
        if (nt > 0) {
            rx_index_order_set(dbf, fi, 0);
            return;
        }
    }
    rx_index_order_set(dbf, -1, -1);
}

/* 1-based n across all attached files (structural CDX first) -> (fi, ti) */
int rx_index_order_nth(rx_dbf *dbf, uint32_t n, int *fi_out, int *ti_out)
{
    uint32_t k = 0;
    int nf = rx_index_file_count(dbf);
    for (int fi = 0; fi < nf; fi++) {
        int nt = rx_tag_count(dbf, fi);
        for (int ti = 0; ti < nt; ti++) {
            k++;
            if (k == n) {
                *fi_out = fi;
                *ti_out = ti;
                return R_OK;
            }
        }
    }
    return R_ERR_RANGE;
}

/* position the pointer at any logical recno (nrec+1 = EOF) without error */
void rx_goto_logical(rx_dbf *dbf, uint32_t recno)
{
    uint32_t n;
    rx_dbf_recount(dbf, &n);
    if (n == 0) {
        rx_dbf_goto(dbf, 1); /* sets BOF+EOF on empty tables */
        return;
    }
    if (recno < 1) {
        recno = 1;
    }
    if (recno > n) {
        rx_dbf_goto(dbf, n);
        rx_dbf_skip(dbf, 1);
        return;
    }
    rx_dbf_goto(dbf, recno);
}

/* ============================ build / maintain ============================ */

/* evaluate a tag's FOR filter at the current record; 1 = include */
static int for_passes(rx_ctx *ctx, rx_idxexpr *x)
{
    if (!x->for_e) {
        return 1;
    }
    rval v;
    rx_rval_undef(&v);
    int rc = rx_eval(ctx, x->for_e, &v, &ctx->err);
    if (rc != R_OK) {
        return -1;
    }
    rx_logic l;
    int ok = rv_to_logic(&v, &l);
    rx_rval_free(&v);
    if (!ok) {
        return -1;
    }
    return l == L_T;
}

int rx_index_rebuild_tag(rx_ctx *ctx, rx_dbf *dbf, int fi, int ti)
{
    rx_idxexpr *x = reg_ensure(ctx, dbf, fi, ti);
    if (!x || !x->key_e) {
        return ctx->err.code ? ctx->err.code : R_ERR_CDX_TAG;
    }
    rx_index_clear(dbf, fi, ti);
    uint32_t n, saved;
    rx_dbf_recount(dbf, &n);
    rx_dbf_recno(dbf, &saved);
    for (uint32_t r = 1; r <= n; r++) {
        int rc = rx_dbf_goto(dbf, r);
        if (rc != R_OK) {
            return rc;
        }
        int p = for_passes(ctx, x);
        if (p < 0) {
            return R_ERR_TYPE;
        }
        if (!p) {
            continue;
        }
        rval v;
        rx_rval_undef(&v);
        rc = rx_eval(ctx, x->key_e, &v, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        rc = rx_index_insert(dbf, fi, ti, &v, r);
        rx_rval_free(&v);
        if (rc != R_OK) {
            return rc;
        }
    }
    rx_goto_logical(dbf, saved);
    return R_OK;
}

int rx_index_rebuild_all(rx_ctx *ctx, rx_dbf *dbf)
{
    int nf = rx_index_file_count(dbf);
    for (int fi = 0; fi < nf; fi++) {
        int nt = rx_tag_count(dbf, fi);
        for (int ti = 0; ti < nt; ti++) {
            int rc = rx_index_rebuild_tag(ctx, dbf, fi, ti);
            if (rc != R_OK) {
                return rc;
            }
        }
    }
    return R_OK;
}

int rx_index_maintain_rec(rx_ctx *ctx, rx_dbf *dbf, uint32_t recno)
{
    int rc = rx_index_delete_rec(dbf, recno);
    if (rc != R_OK) {
        return rc;
    }
    int nf = rx_index_file_count(dbf);
    for (int fi = 0; fi < nf; fi++) {
        int nt = rx_tag_count(dbf, fi);
        for (int ti = 0; ti < nt; ti++) {
            rx_idxexpr *x = reg_ensure(ctx, dbf, fi, ti);
            if (!x || !x->key_e) {
                return ctx->err.code ? ctx->err.code : R_ERR_CDX_TAG;
            }
            int p = for_passes(ctx, x);
            if (p < 0) {
                return R_ERR_TYPE;
            }
            if (!p) {
                continue;
            }
            rval v;
            rx_rval_undef(&v);
            rc = rx_eval(ctx, x->key_e, &v, &ctx->err);
            if (rc != R_OK) {
                return rc;
            }
            rc = rx_index_insert(dbf, fi, ti, &v, recno);
            rx_rval_free(&v);
            if (rc != R_OK) {
                return rc;
            }
        }
    }
    return R_OK;
}

/* ============================ INDEX ON ============================ */

static int cur_table(rx_ctx *ctx, rx_stmt *s, rx_dbf **out)
{
    int rc = rx_wa_dbf(0, out);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
    }
    return R_OK;
}

int rx_cmd_index_on(rx_ctx *ctx, rx_stmt *s)
{
    rx_dbf *dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    if (s->nexpr < 1) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "INDEX ON requires an expression");
    }
    if (rx_dbf_is_readonly(dbf)) {
        return rx_raise(ctx, R_ERR_IO_PERM, s->line, s->col, "File is read only");
    }
    const char *mode = s->nstrs > 0 ? s->strs[0] : "";
    char tagname[12];
    char cdxpath[600];
    const char *cdx = NULL; /* NULL = structural CDX */
    int flags = 0;
    if (strcmp(mode, "TO") == 0 && s->nstrs >= 2) {
        char ipath[600];
        ensure_ext(s->strs[1], ".idx", ipath, sizeof(ipath));
        if (resolve_index_m6(ctx, ipath, cdxpath, sizeof(cdxpath)) != R_OK) {
            /* not on disk: create at the as-given path (relative to cwd, so
               diagnostics and NDX() stay machine-independent) */
            snprintf(cdxpath, sizeof(cdxpath), "%.599s", ipath);
        }
        /* implicit tag name = file stem */
        const char *base = strrchr(cdxpath, '/');
        base = base ? base + 1 : cdxpath;
        size_t l = 0;
        while (base[l] && base[l] != '.' && l < 11) {
            tagname[l] = (char)toupper((unsigned char)base[l]);
            l++;
        }
        tagname[l] = '\0';
        cdx = cdxpath;
    } else if (strcmp(mode, "TAG") == 0 && s->nstrs >= 2) {
        snprintf(tagname, sizeof(tagname), "%s", s->strs[1]);
        for (char *c = tagname; *c; c++) {
            *c = (char)toupper((unsigned char)*c);
        }
        if (s->nstrs >= 4 && strcmp(s->strs[2], "OF") == 0) {
            char cpath[600];
            ensure_ext(s->strs[3], ".cdx", cpath, sizeof(cpath));
            if (resolve_index_m6(ctx, cpath, cdxpath, sizeof(cdxpath)) == R_OK) {
                cdx = cdxpath;
            } else {
                snprintf(cdxpath, sizeof(cdxpath), "%.599s", cpath); /* create path */
                cdx = cdxpath;
            }
        } else {
            cdx = NULL; /* structural CDX */
            if (!rx_dbf_is_exclusive(dbf)) {
                return rx_raise(ctx, R_ERR_FILE_IN_USE, s->line, s->col,
                                "File must be opened exclusively");
            }
        }
    } else {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "INDEX ON requires TO or TAG");
    }
    const char *fortext = NULL;
    rx_expr *fore = NULL;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "FOR") == 0) {
            fore = s->clauses[i].e;
            fortext = s->auxtext;
        }
    }
    (void)fore;
    for (size_t i = 0; i < s->nstrs; i++) {
        if (rx_strcasecmp(s->strs[i], "UNIQUE") == 0) {
            flags |= RX_TAG_UNIQUE;
        } else if (rx_strcasecmp(s->strs[i], "DESCENDING") == 0) {
            flags |= RX_TAG_DESC;
        }
    }
    if (ctx->set.unique) {
        flags |= RX_TAG_UNIQUE; /* SET UNIQUE ON applies to new tags */
    }
    const char *keytext = s->keytext ? s->keytext : "";
    rc = rx_index_create(dbf, tagname, cdx, keytext, fortext, flags, 0);
    if (rc != R_OK) {
        return rx_raise(ctx, rc, s->line, s->col, "Cannot create index tag %s", tagname);
    }
    int fi, ti;
    const char *dpath;
    rx_dbf_path(dbf, &dpath);
    char spath[264];
    if (cdx) {
        snprintf(spath, sizeof(spath), "%.263s", cdx);
    } else {
        snprintf(spath, sizeof(spath), "%.263s", dpath);
        size_t l = strlen(spath);
        if (l >= 4 && rx_strcasecmp(spath + l - 4, ".dbf") == 0) {
            snprintf(spath + l - 4, sizeof(spath) - l + 4, ".cdx");
        } else {
            strncat(spath, ".cdx", sizeof(spath) - strlen(spath) - 1);
        }
    }
    rc = rx_index_find_tag(dbf, spath, tagname, &fi, &ti);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_CDX_TAG, s->line, s->col, "Index tag %s not found", tagname);
    }
    rc = rx_index_rebuild_tag(ctx, dbf, fi, ti);
    if (rc != R_OK) {
        return rx_raise(ctx, rc, s->line, s->col, "INDEX ON failed");
    }
    rx_index_order_set(dbf, fi, ti); /* the new tag becomes controlling */
    rx_index_flush(dbf);
    if (ctx->set.talk) {
        uint32_t k = 0;
        rx_index_key_count(dbf, fi, ti, &k);
        rx_outf(ctx, "%u records indexed.\n", k);
    }
    return R_OK;
}

/* ============================ SEEK ============================ */

int rx_cmd_seek(rx_ctx *ctx, rx_stmt *s)
{
    rx_dbf *dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    if (!rx_index_order_active(dbf)) {
        return rx_raise(ctx, R_ERR_CDX_TAG, s->line, s->col, "No index order is set");
    }
    rval v;
    rx_rval_undef(&v);
    rc = rx_eval(ctx, s->exprs[0], &v, &ctx->err);
    if (rc != R_OK) {
        return rc;
    }
    uint32_t recno = 0;
    int found = 0;
    rc = rx_index_seek(dbf, &v, ctx->set.exact, &recno, &found);
    rx_rval_free(&v);
    if (rc != R_OK) {
        return rx_raise(ctx, rc, s->line, s->col, "SEEK failed");
    }
    if (found && ctx->set.deleted) {
        /* SET DELETED ON: SEEK skips deleted entries in the order */
        int fi, ti;
        rx_index_order_get(dbf, &fi, &ti);
        uint32_t nk = 0;
        rx_index_key_count(dbf, fi, ti, &nk);
        int64_t pos = 0;
        rx_index_pos_of(dbf, recno, &pos);
        while (found && pos >= 0 && (uint32_t)pos < nk) {
            rx_dbf_goto(dbf, recno);
            int del = 0;
            rx_dbf_deleted(dbf, &del);
            if (!del) {
                break;
            }
            pos++;
            if ((uint32_t)pos >= nk) {
                found = 0;
                break;
            }
            rx_index_recno_at(dbf, (uint32_t)pos, &recno);
        }
    }
    if (found) {
        rx_nav_goto(ctx, recno);
        rx_dbf_set_found(dbf, 1);
    } else {
        uint32_t n;
        rx_dbf_recount(dbf, &n);
        rx_goto_logical(dbf, n ? n + 1 : 1); /* miss lands at EOF */
        rx_dbf_set_found(dbf, 0);
    }
    return R_OK;
}

/* ============================ REINDEX ============================ */

int rx_cmd_reindex(rx_ctx *ctx, rx_stmt *s)
{
    rx_dbf *dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    rc = rx_index_rebuild_all(ctx, dbf);
    if (rc != R_OK) {
        return rx_raise(ctx, rc, s->line, s->col, "REINDEX failed");
    }
    rx_index_flush(dbf);
    return R_OK;
}

/* ============================ SET INDEX TO ============================ */

int rx_cmd_set_index(rx_ctx *ctx, rx_stmt *s)
{
    rx_dbf *dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    int additive = 0, clear = 0, nfiles = 0;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "IDX_ADD") == 0) {
            additive = 1;
        } else if (strcmp(s->clauses[i].key, "IDX_CLEAR") == 0) {
            clear = 1;
        } else if (strcmp(s->clauses[i].key, "IDX") == 0) {
            nfiles++;
        }
    }
    if (clear && !nfiles) {
        rx_index_close_nonsstructural(dbf);
        rx_index_default_order(dbf);
        return R_OK;
    }
    if (!additive) {
        rx_index_close_nonsstructural(dbf);
    }
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "IDX") != 0) {
            continue;
        }
        char path[600];
        rc = resolve_index_m6(ctx, s->clauses[i].word, path, sizeof(path));
        if (rc != R_OK) {
            return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "File %s not found",
                            s->clauses[i].word);
        }
        rc = rx_index_attach(dbf, path, NULL);
        if (rc != R_OK) {
            return rx_raise(ctx, rc, s->line, s->col, "Cannot open index %s", path);
        }
    }
    int fi, ti;
    rx_index_order_get(dbf, &fi, &ti);
    if (fi < 0) {
        rx_index_default_order(dbf); /* first tag of the first file */
    }
    return R_OK;
}

/* ============================ SET ORDER TO ============================ */

/* count tags across all attached files; return the (fi, ti) of 1-based n
   (public wrapper rx_index_order_nth above) */

int rx_cmd_set_order(rx_ctx *ctx, rx_stmt *s)
{
    rx_dbf *dbf;
    int rc = cur_table(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    for (size_t i = 0; i < s->nclause; i++) {
        rx_clause *c = &s->clauses[i];
        if (strcmp(c->key, "ORD_CLEAR") == 0) {
            return rx_index_order_set(dbf, -1, -1);
        }
        if (strcmp(c->key, "ORD_TAG") == 0) {
            const char *of = NULL;
            for (size_t j = 0; j < s->nclause; j++) {
                if (strcmp(s->clauses[j].key, "OF") == 0) {
                    of = s->clauses[j].word;
                }
            }
            char ofpath[600];
            if (of) {
                if (resolve_index_m6(ctx, of, ofpath, sizeof(ofpath)) != R_OK) {
                    return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "File %s not found", of);
                }
                of = ofpath;
            }
            int fi, ti;
            if (rx_index_find_tag(dbf, of, c->word, &fi, &ti) != R_OK) {
                return rx_raise(ctx, R_ERR_CDX_TAG, s->line, s->col, "Tag %s not found", c->word);
            }
            return rx_index_order_set(dbf, fi, ti);
        }
        if (strcmp(c->key, "ORD_NUM") == 0) {
            const char *w = c->word;
            int numeric = w[0] != '\0';
            for (const char *q = w; *q; q++) {
                if (!isdigit((unsigned char)*q)) {
                    numeric = 0;
                    break;
                }
            }
            if (numeric) {
                long n = strtol(w, NULL, 10);
                if (n <= 0) {
                    return rx_index_order_set(dbf, -1, -1); /* ORDER 0 = natural */
                }
                int fi, ti;
                if (rx_index_order_nth(dbf, (uint32_t)n, &fi, &ti) != R_OK) {
                    return rx_raise(ctx, R_ERR_RANGE, s->line, s->col, "Invalid index number %ld", n);
                }
                return rx_index_order_set(dbf, fi, ti);
            }
            /* bare word: tag name without the TAG keyword */
            int fi, ti;
            if (rx_index_find_tag(dbf, NULL, w, &fi, &ti) != R_OK) {
                return rx_raise(ctx, R_ERR_CDX_TAG, s->line, s->col, "Tag %s not found", w);
            }
            return rx_index_order_set(dbf, fi, ti);
        }
        if (strcmp(c->key, "ORD_EXPR") == 0) {
            rval v;
            rx_rval_undef(&v);
            rc = rx_eval(ctx, c->e, &v, &ctx->err);
            if (rc != R_OK) {
                return rc;
            }
            double d = 0;
            rv_to_num(&v, &d);
            rx_rval_free(&v);
            long n = (long)d;
            if (n <= 0) {
                return rx_index_order_set(dbf, -1, -1);
            }
            int fi, ti;
            if (rx_index_order_nth(dbf, (uint32_t)n, &fi, &ti) != R_OK) {
                return rx_raise(ctx, R_ERR_RANGE, s->line, s->col, "Invalid index number %ld", n);
            }
            return rx_index_order_set(dbf, fi, ti);
        }
    }
    return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "SET ORDER requires an argument");
}

/* ============================ ordered navigation ============================ */

int rx_nav_top(rx_ctx *ctx)
{
    rx_dbf *dbf;
    int rc = rx_wa_dbf(0, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    if (!rx_index_order_active(dbf)) {
        return rx_nav_goto(ctx, 1);
    }
    uint32_t nk = order_key_count(dbf);
    if (nk == 0) {
        return rx_nav_goto(ctx, 1);
    }
    uint32_t r = 0;
    rx_index_recno_at(dbf, 0, &r);
    return rx_nav_goto(ctx, r);
}

int rx_nav_bottom(rx_ctx *ctx)
{
    rx_dbf *dbf;
    int rc = rx_wa_dbf(0, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    if (!rx_index_order_active(dbf)) {
        uint32_t n;
        rx_dbf_recount(dbf, &n);
        return rx_nav_goto(ctx, n ? n : 1);
    }
    uint32_t nk = order_key_count(dbf);
    if (nk == 0) {
        uint32_t n;
        rx_dbf_recount(dbf, &n);
        return rx_nav_goto(ctx, n ? n : 1);
    }
    uint32_t r = 0;
    rx_index_recno_at(dbf, nk - 1, &r);
    return rx_nav_goto(ctx, r);
}

int rx_nav_skip_ordered(rx_ctx *ctx, int32_t delta)
{
    rx_dbf *dbf;
    int rc = rx_wa_dbf(0, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    uint32_t nk = order_key_count(dbf);
    uint32_t cur;
    rx_dbf_recno(dbf, &cur);
    int eof;
    rx_dbf_eof(dbf, &eof);
    int64_t base;
    if (eof) {
        base = (int64_t)nk;
    } else {
        int64_t pos = -1;
        rx_index_pos_of(dbf, cur, &pos);
        base = pos >= 0 ? pos : (int64_t)(cur ? cur - 1 : 0);
    }
    int64_t t = base + delta;
    if (t < 0) {
        rx_dbf_goto(dbf, 1);
        rx_dbf_skip(dbf, -1); /* BOF: recno 1, BOF() = .T. */
    } else if (t >= (int64_t)nk) {
        uint32_t n;
        rx_dbf_recount(dbf, &n);
        rx_goto_logical(dbf, n ? n + 1 : 1); /* EOF */
    } else {
        uint32_t r = 0;
        rx_index_recno_at(dbf, (uint32_t)t, &r);
        rc = rx_dbf_goto(dbf, r);
        if (rc != R_OK) {
            return rc;
        }
    }
    rx_rel_apply(ctx, rx_wa_current(), 1);
    return R_OK;
}
