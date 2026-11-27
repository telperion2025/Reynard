/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_proj.c
 * @brief Project manager (spec 9.6, M9): BUILD PROJECT <name> FROM <file.rpj>
 *        compiles a text .RPJ into a .RPP container (deterministic stored-ZIP
 *        via util/rpp.c: manifest.json + src/ raw sources + bin/ RXP1 compiled
 *        blobs + res/ screens/reports/labels + data/ tables + dictionary.json),
 *        plus a <name>-app.sh launcher. `reynard --run app.rpp` executes the
 *        packed main program (rx_run_rpp); SET PROCEDURE TO app.rpp registers
 *        the packed sources so DO finds procedures inside. Timestamp/content
 *        based recompile: unchanged sources (sha256 match) skip the rebuild.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lang/lang.h"
#include "reynard/dbf.h"
#include "util/rpp.h"
#include "util/util.h"

/* ============================ .RPJ parsing ============================ */

typedef struct proj_file {
    char rel[300];  /**< path relative to the .RPJ dir, as listed */
    char kind[8];   /**< PRG MENU SCREEN REPORT LABEL DBF */
} proj_file;

typedef struct rpj {
    char dir[600];  /**< directory of the .rpj (files resolve against it) */
    char name[64];
    char main[300];
    proj_file files[64];
    size_t nfiles;
} rpj;

static void rpj_dir_of(const char *path, char *out, size_t outsz)
{
    snprintf(out, outsz, "%.*s", (int)outsz - 1, path);
    char *sl = strrchr(out, '/');
    if (sl) {
        *sl = '\0';
    } else {
        snprintf(out, outsz, ".");
    }
}

static char *read_text(const char *path, size_t *outn)
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
    if (outn) {
        *outn = b.len;
    }
    return b.buf;
}

/* one whitespace token; supports "quoted strings"; returns NULL at end */
static char *next_tok(char **pp)
{
    char *p = *pp;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        *pp = p;
        return NULL;
    }
    char *start;
    if (*p == '"') {
        p++;
        start = p;
        while (*p && *p != '"') {
            p++;
        }
        if (*p == '"') {
            *p = '\0';
            p++;
        }
    } else {
        start = p;
        while (*p && !isspace((unsigned char)*p)) {
            p++;
        }
        if (*p) {
            *p = '\0';
            p++;
        }
    }
    *pp = p;
    return start;
}

static int rpj_add(rpj *pj, const char *kind, const char *rel, r_err *err, int line)
{
    if (pj->nfiles >= 64) {
        err->code = R_ERR_RANGE;
        snprintf(err->msg, sizeof(err->msg), ".RPJ line %d: too many files (64 max)", line);
        return err->code;
    }
    proj_file *f = &pj->files[pj->nfiles++];
    snprintf(f->rel, sizeof(f->rel), "%s", rel);
    snprintf(f->kind, sizeof(f->kind), "%s", kind);
    return R_OK;
}

static int rpj_parse(const char *path, rpj *pj, r_err *err)
{
    size_t n;
    char *src = read_text(path, &n);
    if (!src) {
        err->code = R_ERR_IO_NOTFOUND;
        snprintf(err->msg, sizeof(err->msg), "Cannot open project file %.460s", path);
        return err->code;
    }
    memset(pj, 0, sizeof(*pj));
    rpj_dir_of(path, pj->dir, sizeof(pj->dir));
    snprintf(pj->name, sizeof(pj->name), "%s", "APP");
    int lineno = 0;
    int header_ok = 0;
    int ended = 0;
    char *cur = src;
    while (cur && *cur) {
        /* extract one line */
        char *nl = strchr(cur, '\n');
        if (nl) {
            *nl = '\0';
        }
        lineno++;
        char *p = cur;
        char *w = next_tok(&p);
        if (!w || w[0] == '#' || w[0] == '*') {
            goto nextline;
        }
        if (lineno == 1) {
            char *v = next_tok(&p);
            char *num = next_tok(&p);
            if (rx_strcasecmp(w, "REYNARD") != 0 || !v || rx_strcasecmp(v, "PROJECT") != 0 ||
                !num || atoi(num) != 1) {
                err->code = R_ERR_SYNTAX;
                snprintf(err->msg, sizeof(err->msg),
                         ".RPJ line 1: expected 'REYNARD PROJECT 1'");
                free(src);
                return err->code;
            }
            header_ok = 1;
            goto nextline;
        }
        if (!header_ok) {
            err->code = R_ERR_SYNTAX;
            snprintf(err->msg, sizeof(err->msg), ".RPJ: missing header line");
            free(src);
            return err->code;
        }
        if (rx_strcasecmp(w, "END") == 0) {
            ended = 1;
            goto nextline;
        }
        if (rx_strcasecmp(w, "NAME") == 0) {
            char *v = next_tok(&p);
            if (!v) {
                goto badkw;
            }
            snprintf(pj->name, sizeof(pj->name), "%s", v);
        } else if (rx_strcasecmp(w, "MAIN") == 0) {
            char *v = next_tok(&p);
            if (!v) {
                goto badkw;
            }
            snprintf(pj->main, sizeof(pj->main), "%s", v);
        } else if (rx_strcasecmp(w, "PRG") == 0 || rx_strcasecmp(w, "MENU") == 0 ||
                   rx_strcasecmp(w, "SCREEN") == 0 || rx_strcasecmp(w, "REPORT") == 0 ||
                   rx_strcasecmp(w, "LABEL") == 0 || rx_strcasecmp(w, "DBF") == 0) {
            char *v = next_tok(&p);
            if (!v) {
                goto badkw;
            }
            int rc = rpj_add(pj, w, v, err, lineno);
            if (rc != R_OK) {
                free(src);
                return rc;
            }
        } else {
            err->code = R_ERR_SYNTAX;
            snprintf(err->msg, sizeof(err->msg), ".RPJ line %d: unknown keyword '%s'",
                     lineno, w);
            free(src);
            return err->code;
        }
        goto nextline;
    badkw:
        err->code = R_ERR_SYNTAX;
        snprintf(err->msg, sizeof(err->msg), ".RPJ line %d: '%s' needs a value", lineno, w);
        free(src);
        return err->code;
    nextline:
        if (!nl) {
            break;
        }
        cur = nl + 1;
    }
    free(src);
    if (!header_ok || !ended) {
        err->code = R_ERR_SYNTAX;
        snprintf(err->msg, sizeof(err->msg), ".RPJ: missing header or END");
        return err->code;
    }
    if (!pj->main[0]) {
        snprintf(pj->main, sizeof(pj->main), "%s.prg", pj->name);
    }
    return R_OK;
}

/* ============================ helpers ============================ */

static int sha_file(const char *path, uint8_t hash[32], char **data, size_t *len)
{
    size_t n;
    char *b = read_text(path, &n);
    if (!b) {
        return R_ERR_IO_NOTFOUND;
    }
    rx_sha256(b, n, hash);
    if (data) {
        *data = b;
    }
    if (len) {
        *len = n;
    }
    if (!data) {
        free(b);
    }
    return R_OK;
}

static void hex32(const uint8_t h[32], char out[65])
{
    for (int i = 0; i < 32; i++) {
        snprintf(out + i * 2, 3, "%02x", h[i]);
    }
}

static void json_escape(strbuf *sb, const char *s)
{
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        switch (*c) {
        case '"': sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\n': sb_puts(sb, "\\n"); break;
        case '\r': sb_puts(sb, "\\r"); break;
        case '\t': sb_puts(sb, "\\t"); break;
        default:
            if (*c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "\\u%04x", *c);
                sb_puts(sb, tmp);
            } else {
                sb_putc(sb, (char)*c);
            }
        }
    }
}

static const char *base_name(const char *path)
{
    const char *sl = strrchr(path, '/');
    return sl ? sl + 1 : path;
}

static void stem_of(const char *path, char *out, size_t outsz)
{
    const char *b = base_name(path);
    snprintf(out, outsz, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
}

static int is_kind(const proj_file *f, const char *k)
{
    return strcmp(f->kind, k) == 0;
}

/* data dictionary entry for one DBF, appended as JSON to sb */
static int dict_dbfs(strbuf *sb, rpj *pj)
{
    sb_puts(sb, "{\"databases\": [");
    int first = 1;
    for (size_t i = 0; i < pj->nfiles; i++) {
        proj_file *f = &pj->files[i];
        if (!is_kind(f, "DBF")) {
            continue;
        }
        char full[900];
        snprintf(full, sizeof(full), "%s/%s", pj->dir, f->rel);
        uint8_t hash[32];
        if (sha_file(full, hash, NULL, NULL) != R_OK) {
            continue;
        }
        char hx[65];
        hex32(hash, hx);
        if (!first) {
            sb_putc(sb, ',');
        }
        first = 0;
        sb_puts(sb, "{\"file\": \"");
        json_escape(sb, f->rel);
        sb_puts(sb, "\", \"sha256\": \"");
        sb_puts(sb, hx);
        sb_puts(sb, "\", \"fields\": [");
        rx_dbf *dbf = NULL;
        int rc = rx_dbf_open(full, RD_READONLY | RD_SHARED, &dbf);
        if (rc == R_OK) {
            size_t nf = 0;
            rx_dbf_field_count(dbf, &nf);
            for (size_t fi = 0; fi < nf; fi++) {
                rx_fielddef fd;
                if (rx_dbf_field_def(dbf, fi, &fd) != R_OK) {
                    continue;
                }
                if (fi) {
                    sb_putc(sb, ',');
                }
                char t[2] = { (char)fd.type, '\0' };
                sb_puts(sb, "{\"name\": \"");
                json_escape(sb, fd.name);
                sb_puts(sb, "\", \"type\": \"");
                json_escape(sb, t);
                sb_puts(sb, "\", \"len\": ");
                char num[16];
                snprintf(num, sizeof(num), "%u", fd.len);
                sb_puts(sb, num);
                sb_puts(sb, ", \"dec\": ");
                snprintf(num, sizeof(num), "%u", fd.dec);
                sb_puts(sb, num);
                sb_putc(sb, '}');
            }
            rx_dbf_close(dbf);
        }
        sb_puts(sb, "]}");
    }
    sb_puts(sb, "]}");
    return R_OK;
}

/* ============================ manifest (mini JSON) ============================ */

/* find "key" in json, then return pointer just past the following ':' */
static const char *json_find(const char *j, const char *key)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(j, pat);
    if (!p) {
        return NULL;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    return p;
}

/* read a JSON string value at p into out (handles \" \\ \n etc.) */
static const char *json_str(const char *p, char *out, size_t outsz)
{
    if (!p || *p != '"') {
        return NULL;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            c = *p;
            if (c == 'n') {
                c = '\n';
            } else if (c == 't') {
                c = '\t';
            } else if (c == 'r') {
                c = '\r';
            }
        }
        if (n + 1 < outsz) {
            out[n++] = c;
        }
        p++;
    }
    out[n] = '\0';
    return *p == '"' ? p + 1 : NULL;
}

typedef struct manifest_file {
    char path[300];
    char kind[8];
    char sha[65];
    long size;
} manifest_file;

typedef struct manifest {
    char name[64];
    char main[300];
    manifest_file files[64];
    size_t nfiles;
} manifest;

/* parse the manifest we wrote (objects inside "files": [...]) */
static int manifest_parse(const char *j, manifest *m)
{
    memset(m, 0, sizeof(*m));
    const char *p = json_find(j, "name");
    if (p) {
        json_str(p, m->name, sizeof(m->name));
    }
    p = json_find(j, "main");
    if (p) {
        json_str(p, m->main, sizeof(m->main));
    }
    p = json_find(j, "files");
    if (!p || *p != '[') {
        return R_ERR_DBF_TRUNCATED;
    }
    p++;
    while (*p && *p != ']') {
        if (*p == '{') {
            p++;
            manifest_file *f = &m->files[m->nfiles < 64 ? m->nfiles : 63];
            memset(f, 0, sizeof(*f));
            while (*p && *p != '}') {
                char key[32];
                const char *kp = json_find(p, "");
                (void)kp;
                /* read key */
                while (*p == ' ' || *p == ',') {
                    p++;
                }
                if (*p != '"') {
                    break;
                }
                p = json_str(p, key, sizeof(key));
                if (!p) {
                    break;
                }
                while (*p == ' ' || *p == ':') {
                    p++;
                }
                if (strcmp(key, "path") == 0) {
                    p = json_str(p, f->path, sizeof(f->path));
                } else if (strcmp(key, "kind") == 0) {
                    p = json_str(p, f->kind, sizeof(f->kind));
                } else if (strcmp(key, "sha256") == 0) {
                    p = json_str(p, f->sha, sizeof(f->sha));
                } else if (strcmp(key, "size") == 0) {
                    f->size = strtol(p, (char **)&p, 10);
                } else {
                    /* skip unknown value: string, number, nested */
                    if (*p == '"') {
                        char tmp[300];
                        p = json_str(p, tmp, sizeof(tmp));
                    } else {
                        while (*p && *p != ',' && *p != '}') {
                            p++;
                        }
                    }
                }
                if (!p) {
                    break;
                }
            }
            if (*p == '}') {
                p++;
            }
            if (m->nfiles < 64) {
                m->nfiles++;
            }
        } else {
            p++;
        }
    }
    return R_OK;
}

/* ============================ packed sources ============================ */

static int packed_add(rx_ctx *ctx, const char *stem, char *src /* owned */)
{
    char up[300];
    snprintf(up, sizeof(up), "%s", stem);
    for (char *c = up; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    for (size_t i = 0; i < ctx->npacked; i++) {
        if (strcmp(ctx->packed_names[i], up) == 0) {
            free(ctx->packed_srcs[i]);
            ctx->packed_srcs[i] = src;
            return R_OK;
        }
    }
    if (ctx->npacked == ctx->cappacked) {
        size_t nc = ctx->cappacked ? ctx->cappacked * 2 : 8;
        char **nn = realloc(ctx->packed_names, nc * sizeof(char *));
        char **ns = realloc(ctx->packed_srcs, nc * sizeof(char *));
        if (!nn || !ns) {
            RX_FATAL("out of memory");
        }
        ctx->packed_names = nn;
        ctx->packed_srcs = ns;
        ctx->cappacked = nc;
    }
    ctx->packed_names[ctx->npacked] = rx_strdup(up);
    ctx->packed_srcs[ctx->npacked] = src;
    ctx->npacked++;
    return R_OK;
}

static void packed_clear(rx_ctx *ctx)
{
    for (size_t i = 0; i < ctx->npacked; i++) {
        free(ctx->packed_names[i]);
        free(ctx->packed_srcs[i]);
    }
    free(ctx->packed_names);
    free(ctx->packed_srcs);
    ctx->packed_names = NULL;
    ctx->packed_srcs = NULL;
    ctx->npacked = ctx->cappacked = 0;
}

/* register every src/ entry of an open rpp as packed source + procs */
static int rpp_register_sources(rx_ctx *ctx, const rx_rpp *r)
{
    for (size_t i = 0; i < r->nents; i++) {
        const rx_rpp_ent *e = &r->ents[i];
        if (strncmp(e->name, "src/", 4) != 0) {
            continue;
        }
        uint8_t *buf;
        size_t n;
        if (rx_rpp_read(r, e, &buf, &n) != R_OK) {
            return R_ERR_DBF_TRUNCATED;
        }
        char *src = malloc(n + 1);
        if (!src) {
            free(buf);
            RX_FATAL("out of memory");
        }
        memcpy(src, buf, n);
        src[n] = '\0';
        free(buf);
        char stem[300];
        stem_of(e->name + 4, stem, sizeof(stem));
        packed_add(ctx, stem, src);
        /* pre-register procedures so DO finds them without running the file */
        r_err err;
        memset(&err, 0, sizeof(err));
        rx_stmt *stmts = NULL;
        size_t nstmt = 0;
        char nm[700];
        snprintf(nm, sizeof(nm), "%.590s!%.100s", ctx->rpp_path, e->name);
        if (rx_compile_source(src, nm, &stmts, &nstmt, &err) == R_OK) {
            rx_register_procs(ctx, stmts, nstmt);
            for (size_t k = 0; k < nstmt; k++) {
                rx_stmt_free(&stmts[k]);
            }
            free(stmts);
        }
    }
    return R_OK;
}

/* ============================ BUILD PROJECT ============================ */

static int write_atomic(const char *path, const char *data, size_t n)
{
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return R_ERR_IO_OPEN;
    }
    int rc = R_OK;
    if (n && fwrite(data, 1, n, f) != n) {
        rc = R_ERR_IO_WRITE;
    }
    if (fclose(f) != 0 && rc == R_OK) {
        rc = R_ERR_IO_CLOSE;
    }
    if (rc != R_OK) {
        rx_unlink(tmp);
        return rc;
    }
    if (rename(tmp, path) != 0) {
        rx_unlink(tmp);
        return R_ERR_IO_WRITE;
    }
    return R_OK;
}

/* content-based up-to-date check against an existing .rpp */
static int rpp_up_to_date(const char *rpppath, rpj *pj)
{
    rx_rpp r;
    if (rx_rpp_open(rpppath, &r) != R_OK) {
        return 0;
    }
    const rx_rpp_ent *me = rx_rpp_find(&r, "manifest.json");
    int up = 0;
    if (me) {
        uint8_t *buf;
        size_t n;
        if (rx_rpp_read(&r, me, &buf, &n) == R_OK) {
            char *j = malloc(n + 1);
            if (j) {
                memcpy(j, buf, n);
                j[n] = '\0';
                manifest m;
                if (manifest_parse(j, &m) == R_OK && rx_strcasecmp(m.name, pj->name) == 0 &&
                    m.nfiles == pj->nfiles) {
                    up = 1;
                    for (size_t i = 0; i < pj->nfiles && up; i++) {
                        char full[900];
                        snprintf(full, sizeof(full), "%s/%s", pj->dir, pj->files[i].rel);
                        uint8_t h[32];
                        if (sha_file(full, h, NULL, NULL) != R_OK) {
                            up = 0;
                            break;
                        }
                        char hx[65];
                        hex32(h, hx);
                        const manifest_file *mf = NULL;
                        for (size_t k = 0; k < m.nfiles; k++) {
                            if (strcmp(m.files[k].path, pj->files[i].rel) == 0) {
                                mf = &m.files[k];
                                break;
                            }
                        }
                        if (!mf || strcmp(mf->sha, hx) != 0) {
                            up = 0;
                        }
                    }
                }
                free(j);
            }
        }
        free(buf);
    }
    rx_rpp_close(&r);
    return up;
}

int rx_cmd_build_project(rx_ctx *ctx, rx_stmt *s)
{
    if (s->nstrs < 1) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "BUILD PROJECT needs a name");
    }
    const char *name = s->strs[0];
    char rpjpath[700];
    if (s->nstrs > 1) {
        snprintf(rpjpath, sizeof(rpjpath), "%s", s->strs[1]);
    } else {
        snprintf(rpjpath, sizeof(rpjpath), "%s.rpj", name);
    }
    r_err err;
    memset(&err, 0, sizeof(err));
    rpj pj;
    int rc = rpj_parse(rpjpath, &pj, &err);
    if (rc != R_OK) {
        return rx_raise(ctx, err.code, s->line, s->col, "%s", err.msg);
    }
    /* use the .RPJ NAME when the command name differs only by case */
    if (rx_strcasecmp(pj.name, name) != 0) {
        snprintf(pj.name, sizeof(pj.name), "%s", name);
    }

    /* missing-dependency reporting: collect ALL missing files */
    char missing[1024];
    missing[0] = '\0';
    size_t nmiss = 0;
    for (size_t i = 0; i < pj.nfiles; i++) {
        char full[900];
        snprintf(full, sizeof(full), "%s/%s", pj.dir, pj.files[i].rel);
        if (access(full, R_OK) != 0) {
            size_t l = strlen(missing);
            snprintf(missing + l, sizeof(missing) - l, "%s%s", nmiss ? ", " : "", pj.files[i].rel);
            nmiss++;
        }
    }
    char mainfull[900];
    snprintf(mainfull, sizeof(mainfull), "%s/%s", pj.dir, pj.main);
    if (access(mainfull, R_OK) != 0) {
        size_t l = strlen(missing);
        snprintf(missing + l, sizeof(missing) - l, "%s%s", nmiss ? ", " : "", pj.main);
        nmiss++;
    }
    if (nmiss) {
        return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col,
                        "BUILD PROJECT: %zu missing dependenc%s: %s", nmiss,
                        nmiss == 1 ? "y" : "ies", missing);
    }

    char rpppath[700];
    snprintf(rpppath, sizeof(rpppath), "%s/%s.rpp", pj.dir, pj.name);

    /* timestamp/content recompile: identical sources -> skip */
    if (access(rpppath, R_OK) == 0 && rpp_up_to_date(rpppath, &pj)) {
        if (ctx->set.talk) {
            rx_outf(ctx, "Project '%s' is up to date (%s).\n", pj.name, rpppath);
        }
        return R_OK;
    }

    /* build the container */
    rx_zipw *w = rx_zipw_new();
    if (!w) {
        return rx_raise(ctx, R_ERR_OS, s->line, s->col, "out of memory");
    }
    /* manifest first (built after we know sha/size of each file) */
    strbuf man;
    sb_init(&man);
    sb_puts(&man, "{\n  \"format\": \"RPP1\",\n  \"name\": \"");
    json_escape(&man, pj.name);
    sb_puts(&man, "\",\n  \"main\": \"");
    json_escape(&man, pj.main);
    sb_puts(&man, "\",\n  \"files\": [\n");
    uint8_t hashes[64][32];
    size_t sizes[64];
    for (size_t i = 0; i < pj.nfiles; i++) {
        char full[900];
        snprintf(full, sizeof(full), "%s/%s", pj.dir, pj.files[i].rel);
        if (sha_file(full, hashes[i], NULL, &sizes[i]) != R_OK) {
            sb_free(&man);
            rx_zipw_free(w);
            return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "Cannot read %s",
                            pj.files[i].rel);
        }
        char hx[65];
        hex32(hashes[i], hx);
        if (i) {
            sb_putc(&man, ',');
        }
        sb_puts(&man, "\n    {\"path\": \"");
        json_escape(&man, pj.files[i].rel);
        sb_puts(&man, "\", \"kind\": \"");
        json_escape(&man, pj.files[i].kind);
        sb_puts(&man, "\", \"sha256\": \"");
        sb_puts(&man, hx);
        char num[24];
        snprintf(num, sizeof(num), "\", \"size\": %zu}", sizes[i]);
        sb_puts(&man, num);
    }
    sb_puts(&man, "\n  ]\n}\n");
    rx_zipw_add(w, "manifest.json", man.buf ? man.buf : "", man.len);

    /* per-file entries */
    for (size_t i = 0; i < pj.nfiles; i++) {
        proj_file *f = &pj.files[i];
        char full[900];
        snprintf(full, sizeof(full), "%s/%s", pj.dir, f->rel);
        size_t n;
        char *data = read_text(full, &n);
        if (!data) {
            sb_free(&man);
            rx_zipw_free(w);
            return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "Cannot read %s", f->rel);
        }
        char entry[400];
        if (is_kind(f, "PRG") || is_kind(f, "MENU")) {
            snprintf(entry, sizeof(entry), "src/%s", f->rel);
            rx_zipw_add(w, entry, data, n);
            /* compiled RXP1 blob */
            r_err cerr;
            memset(&cerr, 0, sizeof(cerr));
            rx_stmt *stmts = NULL;
            size_t nstmt = 0;
            if (rx_compile_source(data, f->rel, &stmts, &nstmt, &cerr) == R_OK) {
                char *blob;
                size_t bloblen;
                if (rx_rxp_pack(hashes[i], stmts, nstmt, &blob, &bloblen) == R_OK) {
                    char stem[300];
                    stem_of(f->rel, stem, sizeof(stem));
                    char bent[400];
                    snprintf(bent, sizeof(bent), "bin/%s.rxp", stem);
                    rx_zipw_add(w, bent, blob, bloblen);
                    free(blob);
                }
                for (size_t k = 0; k < nstmt; k++) {
                    rx_stmt_free(&stmts[k]);
                }
                free(stmts);
            } else {
                free(data);
                sb_free(&man);
                rx_zipw_free(w);
                return rx_raise(ctx, cerr.code, s->line, s->col,
                                "Compile error in %s: %s", f->rel, cerr.msg);
            }
        } else if (is_kind(f, "SCREEN") || is_kind(f, "REPORT") || is_kind(f, "LABEL")) {
            snprintf(entry, sizeof(entry), "res/%s", f->rel);
            rx_zipw_add(w, entry, data, n);
        } else if (is_kind(f, "DBF")) {
            snprintf(entry, sizeof(entry), "data/%s", f->rel);
            rx_zipw_add(w, entry, data, n);
        }
        free(data);
    }
    /* data dictionary */
    {
        strbuf dict;
        sb_init(&dict);
        dict_dbfs(&dict, &pj);
        sb_putc(&dict, '\n');
        rx_zipw_add(w, "data/dictionary.json", dict.buf ? dict.buf : "", dict.len);
        sb_free(&dict);
    }
    sb_free(&man);

    char *zbuf;
    size_t zlen;
    size_t nent = rx_zipw_count(w);
    rc = rx_zipw_finish(w, &zbuf, &zlen);
    rx_zipw_free(w);
    if (rc != R_OK) {
        return rx_raise(ctx, rc, s->line, s->col, "Cannot serialize .rpp");
    }
    rc = write_atomic(rpppath, zbuf, zlen);
    free(zbuf);
    if (rc != R_OK) {
        return rx_raise(ctx, rc, s->line, s->col, "Cannot write %s", rpppath);
    }

    /* launcher convenience script next to the .rpp */
    char shpath[700];
    snprintf(shpath, sizeof(shpath), "%s/%s-app.sh", pj.dir, pj.name);
    {
        strbuf sh;
        sb_init(&sh);
        sb_puts(&sh, "#!/bin/sh\n");
        sb_puts(&sh, "# Reynard application launcher (generated by BUILD PROJECT)\n");
        sb_puts(&sh, "exec reynard --run \"$(dirname \"$0\")/");
        sb_puts(&sh, pj.name);
        sb_puts(&sh, ".rpp\" \"$@\"\n");
        rc = write_atomic(shpath, sh.buf ? sh.buf : "", sh.len);
        sb_free(&sh);
        if (rc == R_OK) {
            chmod(shpath, 0755);
        }
    }

    if (ctx->set.talk) {
        rx_outf(ctx, "Project '%s' built: %s (%zu entries, %zu bytes)\n", pj.name, rpppath,
                nent, zlen);
        rx_outf(ctx, "Launcher: %s\n", shpath);
    }
    return R_OK;
}

/* ============================ SET PROCEDURE TO ============================ */

int rx_cmd_set_proc(rx_ctx *ctx, rx_stmt *s)
{
    /* SET PROCEDURE TO [file[.prg|.rpp]] — strs: PROCEDURE TO <file> */
    const char *toval = NULL;
    if (s->nstrs >= 3 && rx_strcasecmp(s->strs[1], "TO") == 0) {
        toval = s->strs[2];
    } else if (s->nstrs >= 2 && rx_strcasecmp(s->strs[1], "TO") != 0) {
        toval = s->strs[1];
    }
    if (!toval || !toval[0]) {
        /* SET PROCEDURE TO : close the procedure file */
        packed_clear(ctx);
        ctx->rpp_path[0] = '\0';
        return R_OK;
    }
    size_t l = strlen(toval);
    if (l > 4 && rx_strcasecmp(toval + l - 4, ".rpp") == 0) {
        rx_rpp r;
        int rc = rx_rpp_open(toval, &r);
        if (rc != R_OK) {
            return rx_raise(ctx, rc, s->line, s->col, "Cannot open procedure file %s",
                            toval);
        }
        snprintf(ctx->rpp_path, sizeof(ctx->rpp_path), "%s", toval);
        rc = rpp_register_sources(ctx, &r);
        rx_rpp_close(&r);
        if (rc != R_OK) {
            return rx_raise(ctx, rc, s->line, s->col, "Cannot load packed sources from %s",
                            toval);
        }
        return R_OK;
    }
    /* plain .prg procedure file: compile and register procs, don't execute */
    char path[700];
    if (strchr(toval, '.') || strchr(toval, '/')) {
        snprintf(path, sizeof(path), "%s", toval);
    } else {
        snprintf(path, sizeof(path), "%s.prg", toval);
    }
    size_t n;
    char *src = read_text(path, &n);
    if (!src) {
        return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "Cannot open %s", path);
    }
    r_err err;
    memset(&err, 0, sizeof(err));
    rx_stmt *stmts = NULL;
    size_t nstmt = 0;
    int rc = rx_compile_source(src, path, &stmts, &nstmt, &err);
    free(src);
    if (rc != R_OK) {
        return rx_raise(ctx, err.code, s->line, s->col, "%s", err.msg);
    }
    rx_register_procs(ctx, stmts, nstmt);
    for (size_t i = 0; i < nstmt; i++) {
        rx_stmt_free(&stmts[i]);
    }
    free(stmts);
    return R_OK;
}

/* ============================ --run ============================ */

static int mkdir_p(const char *path)
{
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        return R_ERR_IO_OPEN;
    }
    return R_OK;
}

/* extract res/ and data/ entries of the rpp into a per-app cache dir and
   return it (static buffer) so the caller can prepend it to SET PATH */
static const char *rpp_extract(const rx_ctx *ctx, const rx_rpp *r, char *out, size_t outsz)
{
    uint8_t h[32];
    rx_sha256(r->data, r->len, h);
    char hex[65];
    hex32(h, hex);
    const char *xdg = getenv("XDG_CACHE_HOME");
    char base[600];
    if (xdg && *xdg) {
        snprintf(base, sizeof(base), "%s/reynard/rpp/%s", xdg, hex);
    } else {
        const char *home = getenv("HOME");
        if (home && *home) {
            snprintf(base, sizeof(base), "%s/.cache/reynard/rpp/%s", home, hex);
        } else {
            return NULL;
        }
    }
    int any = 0;
    for (size_t i = 0; i < r->nents; i++) {
        const char *nm = r->ents[i].name;
        if (strncmp(nm, "res/", 4) != 0 && strncmp(nm, "data/", 5) != 0) {
            continue;
        }
        const char *rel = nm + (nm[0] == 'r' ? 4 : 5);
        if (!*rel || strchr(rel, '/')) {
            continue; /* keep the cache dir flat */
        }
        any = 1;
    }
    if (!any) {
        return NULL;
    }
    if (mkdir_p(base) != R_OK) {
        return NULL;
    }
    for (size_t i = 0; i < r->nents; i++) {
        const char *nm = r->ents[i].name;
        const char *rel = NULL;
        if (strncmp(nm, "res/", 4) == 0) {
            rel = nm + 4;
        } else if (strncmp(nm, "data/", 5) == 0 && strcmp(nm, "data/dictionary.json") != 0) {
            rel = nm + 5;
        }
        if (!rel || !*rel || strchr(rel, '/')) {
            continue;
        }
        uint8_t *buf;
        size_t n;
        if (rx_rpp_read(r, &r->ents[i], &buf, &n) != R_OK) {
            continue;
        }
        char dst[800];
        snprintf(dst, sizeof(dst), "%s/%s", base, rel);
        write_atomic(dst, (const char *)buf, n);
        free(buf);
    }
    snprintf(out, outsz, "%s", base);
    (void)ctx;
    return out;
}

int rx_run_rpp(rx_ctx *ctx, const char *path)
{
    rx_rpp r;
    int rc = rx_rpp_open(path, &r);
    if (rc != R_OK) {
        return rx_raise(ctx, rc, 0, 0, "Cannot open application %s", path);
    }
    const rx_rpp_ent *me = rx_rpp_find(&r, "manifest.json");
    if (!me) {
        rx_rpp_close(&r);
        return rx_raise(ctx, R_ERR_DBF_TRUNCATED, 0, 0, "%s: not a Reynard .rpp (no manifest)",
                        path);
    }
    uint8_t *mbuf;
    size_t mn;
    rc = rx_rpp_read(&r, me, &mbuf, &mn);
    if (rc != R_OK) {
        rx_rpp_close(&r);
        return rx_raise(ctx, rc, 0, 0, "%s: corrupt manifest", path);
    }
    char *j = malloc(mn + 1);
    if (!j) {
        free(mbuf);
        rx_rpp_close(&r);
        RX_FATAL("out of memory");
    }
    memcpy(j, mbuf, mn);
    j[mn] = '\0';
    free(mbuf);
    manifest m;
    rc = manifest_parse(j, &m);
    free(j);
    if (rc != R_OK || !m.main[0]) {
        rx_rpp_close(&r);
        return rx_raise(ctx, R_ERR_DBF_TRUNCATED, 0, 0, "%s: bad manifest", path);
    }
    snprintf(ctx->rpp_path, sizeof(ctx->rpp_path), "%s", path);
    rc = rpp_register_sources(ctx, &r);
    if (rc != R_OK) {
        rx_rpp_close(&r);
        return rx_raise(ctx, rc, 0, 0, "%s: cannot load packed sources", path);
    }
    /* extract packed res//data/ to the per-app cache and put it on SET PATH so
       GENERATE SCREEN / REPORT FORM / USE find them (spec 9.6 packaging) */
    {
        char exdir[700];
        const char *ex = rpp_extract(ctx, &r, exdir, sizeof(exdir));
        if (ex) {
            char np[1200];
            if (ctx->set.path[0]) {
                snprintf(np, sizeof(np), "%s,%.*s", ex, (int)(sizeof(ctx->set.path) - strlen(ex) - 2),
                         ctx->set.path);
            } else {
                snprintf(np, sizeof(np), "%s", ex);
            }
            snprintf(ctx->set.path, sizeof(ctx->set.path), "%.*s",
                     (int)sizeof(ctx->set.path) - 1, np);
        }
    }

    /* run main: prefer the compiled bin/ blob, fall back to source */
    char mainstem[300];
    stem_of(m.main, mainstem, sizeof(mainstem));
    char binname[400];
    snprintf(binname, sizeof(binname), "bin/%s.rxp", mainstem);
    const rx_rpp_ent *be = rx_rpp_find(&r, binname);
    const rx_rpp_ent *se = NULL;
    char srcname[400];
    snprintf(srcname, sizeof(srcname), "src/%s", m.main);
    se = rx_rpp_find(&r, srcname);
    if (!be && !se) {
        rx_rpp_close(&r);
        return rx_raise(ctx, R_ERR_IO_NOTFOUND, 0, 0, "%s: main program %s not found", path,
                        m.main);
    }
    if (se) {
        /* need the source sha for the rxp hash check */
        uint8_t *sbuf;
        size_t sn;
        if (rx_rpp_read(&r, se, &sbuf, &sn) == R_OK) {
            uint8_t h[32];
            rx_sha256(sbuf, sn, h);
            if (be) {
                uint8_t *bbuf;
                size_t bn;
                if (rx_rpp_read(&r, be, &bbuf, &bn) == R_OK) {
                    rx_stmt *stmts = NULL;
                    size_t nstmt = 0;
                    if (rx_rxp_unpack((const char *)bbuf, bn, h, &stmts, &nstmt) == R_OK) {
                        snprintf(ctx->prog, sizeof(ctx->prog), "%.150s!%.100s", path, m.main);
                        ctx->src = (const char *)sbuf;
                        rc = rx_exec(ctx, stmts, nstmt);
                        ctx->src = NULL;
                        for (size_t i = 0; i < nstmt; i++) {
                            rx_stmt_free(&stmts[i]);
                        }
                        free(stmts);
                    } else {
                        rc = R_ERR_DBF_BADRXP;
                    }
                    free(bbuf);
                } else {
                    rc = R_ERR_DBF_TRUNCATED;
                }
            }
            if (!be || rc == R_ERR_DBF_BADRXP || rc == R_ERR_DBF_TRUNCATED) {
                /* source fallback */
                char *src = malloc(sn + 1);
                if (!src) {
                    free(sbuf);
                    rx_rpp_close(&r);
                    RX_FATAL("out of memory");
                }
                memcpy(src, sbuf, sn);
                src[sn] = '\0';
                char nm[400];
                snprintf(nm, sizeof(nm), "%s!%s", path, m.main);
                rc = rx_run_source(ctx, src, nm);
                free(src);
            }
            free(sbuf);
        } else {
            rc = R_ERR_DBF_TRUNCATED;
        }
    } else {
        rc = R_ERR_IO_NOTFOUND;
    }
    rx_rpp_close(&r);
    if (rc != R_OK && !ctx->err_printed) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s", ctx->err.msg[0] ? ctx->err.msg : rx_status_str(rc));
        rx_raise(ctx, rc, ctx->err.line, ctx->err.col, "%s", msg);
    }
    return rc;
}

/* M9 9.6: resource resolution across SET DEFAULT / SET PATH (packed .rpp
   resources are extracted to a cache dir that --run prepends to SET PATH) */
int rx_resolve_res(rx_ctx *ctx, const char *name, const char *ext, char *out, size_t outsz)
{
    char base[400];
    if (strchr(name, '.')) {
        snprintf(base, sizeof(base), "%s", name);
    } else {
        snprintf(base, sizeof(base), "%s.%s", name, ext);
    }
    snprintf(out, outsz, "%s", base);
    FILE *f = fopen(out, "rb");
    if (f) {
        fclose(f);
        return R_OK;
    }
    if (ctx->set.defdir[0]) {
        char cand[700];
        snprintf(cand, sizeof(cand), "%s/%s", ctx->set.defdir, base);
        f = fopen(cand, "rb");
        if (f) {
            fclose(f);
            snprintf(out, outsz, "%s", cand);
            return R_OK;
        }
    }
    if (ctx->set.path[0]) {
        char pc[1024];
        snprintf(pc, sizeof(pc), "%s", ctx->set.path);
        char *save = NULL;
        for (char *d = strtok_r(pc, ",", &save); d; d = strtok_r(NULL, ",", &save)) {
            char cand[700];
            snprintf(cand, sizeof(cand), "%s/%s", d, base);
            f = fopen(cand, "rb");
            if (f) {
                fclose(f);
                snprintf(out, outsz, "%s", cand);
                return R_OK;
            }
        }
    }
    snprintf(out, outsz, "%s", base);
    return R_ERR_IO_NOTFOUND;
}
