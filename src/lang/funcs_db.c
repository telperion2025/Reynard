/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file funcs_db.c
 * @brief Database introspection functions (spec Appendix A, funcs_db.c).
 *        They read the documented work-area table (include/reynard/dbf.h).
 */
#include "lang/funcs.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "reynard/dbf.h"
#include "util/util.h"

static int cur_dbf(rx_ctx *c, rx_dbf **out, r_err *err)
{
    (void)c;
    int rc = rx_wa_dbf(0, out);
    if (rc != R_OK) {
        err->code = rc;
        snprintf(err->msg, sizeof(err->msg), "No table in use");
    }
    return rc;
}

static int f_recno(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "RECNO() expects no arguments");
        return err->code;
    }
    (void)argv;
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        return rc;
    }
    uint32_t r;
    rx_dbf_recno(dbf, &r);
    f_num(c, out, r);
    return R_OK;
}

static int f_reccount(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "RECCOUNT() expects no arguments");
        return err->code;
    }
    (void)argv;
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        return rc;
    }
    uint32_t r;
    rx_dbf_recount(dbf, &r);
    f_num(c, out, r);
    return R_OK;
}

static int f_bof(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "BOF() expects no arguments");
        return err->code;
    }
    (void)argv;
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        return rc;
    }
    int v = 0;
    rx_dbf_bof(dbf, &v);
    f_logic(out, v ? L_T : L_F);
    return R_OK;
}

static int f_eof(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "EOF() expects no arguments");
        return err->code;
    }
    (void)argv;
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        return rc;
    }
    int v = 0;
    rx_dbf_eof(dbf, &v);
    f_logic(out, v ? L_T : L_F);
    return R_OK;
}

static int f_deleted(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "DELETED() expects no arguments");
        return err->code;
    }
    (void)argv;
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        return rc;
    }
    int v = 0;
    rx_dbf_deleted(dbf, &v);
    f_logic(out, v ? L_T : L_F);
    return R_OK;
}

static int f_alias(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    int area = 0;
    if (argc == 1) {
        double d;
        if (rv_to_num(&argv[0], &d)) {
            area = (int)d;
        } else {
            const char *s;
            size_t n;
            if (!rv_to_str(&argv[0], &s, &n)) {
                err->code = R_ERR_TYPE;
                snprintf(err->msg, sizeof(err->msg), "ALIAS() argument must be numeric or alias");
                return err->code;
            }
            for (int i = 1; i <= 255; i++) {
                if (rx_wa_used(i)) {
                    const char *a;
                    rx_wa_alias(i, &a);
                    if (n == strlen(a) && memcmp(s, a, n) == 0) {
                        area = i;
                        break;
                    }
                }
            }
            if (area == 0) {
                f_str(out, rx_strdup(""), 0);
                (void)c;
                return R_OK;
            }
        }
    } else if (argc > 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ALIAS() expects 0 or 1 arguments");
        return err->code;
    }
    const char *a = "";
    rx_wa_alias(area, &a);
    f_str(out, rx_strdup(a), strlen(a));
    (void)c;
    return R_OK;
}

static int f_select(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "SELECT() expects 0 or 1 arguments");
        return err->code;
    }
    if (argc == 1) {
        double d;
        if (rv_to_num(&argv[0], &d)) {
            f_num(c, out, (int)d == 0 ? rx_wa_current() : (double)(int)d);
            return R_OK;
        }
        const char *s;
        size_t n;
        if (!rv_to_str(&argv[0], &s, &n)) {
            err->code = R_ERR_TYPE;
            return err->code;
        }
        for (int i = 1; i <= 255; i++) {
            if (rx_wa_used(i)) {
                const char *a;
                rx_wa_alias(i, &a);
                if (strlen(a) == n && memcmp(s, a, n) == 0) {
                    f_num(c, out, i);
                    return R_OK;
                }
            }
        }
        f_num(c, out, 0);
        return R_OK;
    }
    f_num(c, out, rx_wa_current());
    return R_OK;
}

static int f_used(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    int area = 0;
    if (argc == 1) {
        double d;
        if (rv_to_num(&argv[0], &d)) {
            area = (int)d;
        } else {
            const char *s;
            size_t n;
            if (!rv_to_str(&argv[0], &s, &n)) {
                err->code = R_ERR_TYPE;
                snprintf(err->msg, sizeof(err->msg), "USED() argument must be numeric or alias");
                return err->code;
            }
            area = -1;
            for (int i = 1; i <= 255; i++) {
                if (rx_wa_used(i)) {
                    const char *a;
                    rx_wa_alias(i, &a);
                    if (strlen(a) == n && memcmp(s, a, n) == 0) {
                        area = i;
                        break;
                    }
                }
            }
        }
    } else if (argc > 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "USED() expects 0 or 1 arguments");
        return err->code;
    }
    f_logic(out, rx_wa_used(area) ? L_T : L_F);
    (void)c;
    return R_OK;
}

static int f_fcount(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "FCOUNT() expects no arguments");
        return err->code;
    }
    (void)argv;
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        return rc;
    }
    size_t n;
    rx_dbf_field_count(dbf, &n);
    f_num(c, out, (double)n);
    return R_OK;
}

static int f_field(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (f_argc(err, argc, 1, "FIELD") != 0) {
        return err->code;
    }
    double d;
    if (!rv_to_num(&argv[0], &d)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "FIELD() expects numeric");
        return err->code;
    }
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        return rc;
    }
    rx_fielddef fd;
    if (rx_dbf_field_def(dbf, (size_t)d - 1, &fd) != R_OK) {
        f_str(out, rx_strdup(""), 0);
        (void)c;
        return R_OK;
    }
    f_str(out, rx_strdup(fd.name), strlen(fd.name));
    (void)c;
    return R_OK;
}

static int f_fsize(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (f_argc(err, argc, 1, "FSIZE") != 0) {
        return err->code;
    }
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        return rc;
    }
    const char *s;
    size_t n;
    size_t idx;
    if (rv_to_str(&argv[0], &s, &n)) {
        char nm[64];
        size_t k = n < 63 ? n : 63;
        memcpy(nm, s, k);
        nm[k] = '\0';
        rx_strtoupper(nm);
        if (rx_dbf_field_index(dbf, nm, &idx) != R_OK) {
            err->code = R_ERR_FIELD;
            snprintf(err->msg, sizeof(err->msg), "Unknown field %s", nm);
            return err->code;
        }
    } else {
        double d;
        if (!rv_to_num(&argv[0], &d)) {
            err->code = R_ERR_TYPE;
            snprintf(err->msg, sizeof(err->msg), "FSIZE() expects field name or number");
            return err->code;
        }
        idx = (size_t)d - 1;
    }
    rx_fielddef fd;
    rx_dbf_field_def(dbf, idx, &fd);
    f_num(c, out, fd.len);
    return R_OK;
}

static int f_header(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "HEADER() expects no arguments");
        return err->code;
    }
    (void)argv;
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        return rc;
    }
    /* header size: expose via field table + fixed overhead */
    size_t nf;
    rx_dbf_field_count(dbf, &nf);
    f_num(c, out, (double)(32 + nf * 32 + 1));
    return R_OK;
}

static int f_recsize(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "RECSIZE() expects no arguments");
        return err->code;
    }
    (void)argv;
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        return rc;
    }
    size_t nf;
    rx_dbf_field_count(dbf, &nf);
    int w = 1;
    for (size_t i = 0; i < nf; i++) {
        rx_fielddef fd;
        rx_dbf_field_def(dbf, i, &fd);
        w += fd.len;
    }
    f_num(c, out, w);
    return R_OK;
}

static int f_found(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "FOUND() expects no arguments");
        return err->code;
    }
    (void)argv;
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        f_logic(out, L_F);
        return R_OK;
    }
    f_logic(out, rx_dbf_found(dbf) ? L_T : L_F);
    return R_OK;
}

static int f_flock(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "FLOCK() expects no arguments");
        return err->code;
    }
    (void)argv;
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        f_logic(out, L_F);
        return R_OK;
    }
    rc = rx_flock(dbf);
    f_logic(out, rc == R_OK ? L_T : L_F);
    return R_OK;
}

static int f_rlock(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "RLOCK() expects 0 or 1 arguments");
        return err->code;
    }
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        f_logic(out, L_F);
        return R_OK;
    }
    uint32_t recno;
    if (argc == 1) {
        double d;
        if (!rv_to_num(&argv[0], &d)) {
            err->code = R_ERR_TYPE;
            snprintf(err->msg, sizeof(err->msg), "RLOCK() expects a record number");
            return err->code;
        }
        recno = (uint32_t)(d < 1 ? 1 : d);
    } else {
        rx_dbf_recno(dbf, &recno);
    }
    rc = rx_rlock(dbf, recno);
    f_logic(out, rc == R_OK ? L_T : L_F);
    return R_OK;
}

static int f_lupdate(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "LUPDATE() expects no arguments");
        return err->code;
    }
    (void)argv;
    rx_dbf *dbf;
    int rc = cur_dbf(c, &dbf, err);
    if (rc != R_OK) {
        return rc;
    }
    int y, m, d;
    if (rx_dbf_lupdate(dbf, &y, &m, &d) == R_OK) {
        r_date dt = { (int16_t)y, (int16_t)m, (int16_t)d };
        f_date(out, dt);
    } else {
        r_date dt = { 0, 0, 0 };
        f_date(out, dt);
    }
    (void)c;
    return R_OK;
}

/* ---- M6 §8.4 index introspection (Appendix A) ---- */

/* area argument: numeric work area or alias; area 0 = current */
static int area_arg(rx_ctx *c, const rval *v, int *area, r_err *err)
{
    (void)c;
    double d;
    if (rv_to_num(v, &d)) {
        *area = (int)d;
        return R_OK;
    }
    const char *s;
    size_t n;
    if (rv_to_str(v, &s, &n)) {
        for (int i = 1; i <= 255; i++) {
            if (rx_wa_used(i)) {
                const char *a;
                rx_wa_alias(i, &a);
                if (strlen(a) == n && memcmp(a, s, n) == 0) {
                    *area = i;
                    return R_OK;
                }
            }
        }
        err->code = R_ERR_FIELD;
        snprintf(err->msg, sizeof(err->msg), "Alias not found");
        return err->code;
    }
    err->code = R_ERR_TYPE;
    snprintf(err->msg, sizeof(err->msg), "Work area must be numeric or an alias");
    return err->code;
}

/* case-insensitive prefix compare of exactly n bytes */
static int ci_eq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

/* find an attached index file by path, basename or stem (case-insensitive) */
static int file_of_name(rx_dbf *dbf, const char *s, size_t n)
{
    int nf = rx_index_file_count(dbf);
    for (int fi = 0; fi < nf; fi++) {
        const char *p = NULL;
        rx_index_file_path(dbf, fi, &p);
        if (strlen(p) == n && ci_eq_n(p, s, n)) {
            return fi;
        }
        const char *base = strrchr(p, '/');
        base = base ? base + 1 : p;
        if (strlen(base) == n && ci_eq_n(base, s, n)) {
            return fi;
        }
        /* stem without extension */
        size_t bl = strlen(base);
        if (bl > n && base[bl - n - 1] == '.' && ci_eq_n(base, s, n)) {
            return fi;
        }
    }
    return -1;
}

static int f_filter(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "FILTER() expects no arguments");
        return err->code;
    }
    (void)argv;
    const char *t = c->filter_text ? c->filter_text : "";
    f_str(out, rx_strdup(t), strlen(t));
    return R_OK;
}

static int f_order(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    int area = 0;
    if (argc == 1) {
        if (area_arg(c, &argv[0], &area, err) != R_OK) {
            return err->code;
        }
    } else if (argc > 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ORDER() expects 0 or 1 arguments");
        return err->code;
    }
    rx_dbf *dbf;
    if (rx_wa_dbf(area == 0 ? rx_wa_current() : area, &dbf) != R_OK) {
        f_str(out, rx_strdup(""), 0);
        return R_OK;
    }
    int fi, ti;
    rx_index_order_get(dbf, &fi, &ti);
    if (fi < 0) {
        f_str(out, rx_strdup(""), 0);
        return R_OK;
    }
    const char *name = "";
    rx_tag_info(dbf, fi, ti, &name, NULL, NULL);
    f_str(out, rx_strdup(name), strlen(name));
    return R_OK;
}

/* TAG([cdx,] n [, area]) / KEY([cdx,] n [, area]): shared walk */
static int tag_nth(rx_dbf *dbf, const char *cdx, long n, int want_key, char **res)
{
    int nf = rx_index_file_count(dbf);
    if (cdx) {
        int fi = file_of_name(dbf, cdx, strlen(cdx));
        if (fi < 0) {
            return 0; /* "" */
        }
        int nt = rx_tag_count(dbf, fi);
        if (n < 1 || n > nt) {
            return 0;
        }
        const char *name = NULL, *key = NULL;
        rx_tag_info(dbf, fi, n - 1, &name, &key, NULL);
        *res = rx_strdup(want_key ? key : name);
        return 1;
    }
    long k = 0;
    for (int fi = 0; fi < nf; fi++) {
        int nt = rx_tag_count(dbf, fi);
        for (int ti = 0; ti < nt; ti++) {
            k++;
            if (k == n) {
                const char *name = NULL, *key = NULL;
                rx_tag_info(dbf, fi, ti, &name, &key, NULL);
                *res = rx_strdup(want_key ? key : name);
                return 1;
            }
        }
    }
    return 0;
}

static int f_tag(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    size_t i = 0;
    const char *cdx = NULL;
    if (argc == 0) {
        /* TAG() = name of the controlling order tag */
        rx_dbf *dbf;
        if (rx_wa_dbf(0, &dbf) != R_OK) {
            f_str(out, rx_strdup(""), 0);
            return R_OK;
        }
        int fi, ti;
        rx_index_order_get(dbf, &fi, &ti);
        if (fi < 0) {
            f_str(out, rx_strdup(""), 0);
            return R_OK;
        }
        const char *name = "";
        rx_tag_info(dbf, fi, ti, &name, NULL, NULL);
        f_str(out, rx_strdup(name), strlen(name));
        return R_OK;
    }
    if (argc >= 1) {
        double d;
        if (!rv_to_num(&argv[0], &d)) {
            const char *s;
            size_t n;
            if (!rv_to_str(&argv[0], &s, &n)) {
                err->code = R_ERR_TYPE;
                snprintf(err->msg, sizeof(err->msg), "TAG() expects a file name or index number");
                return err->code;
            }
            cdx = s; /* borrowed; argv outlives the call */
            i = 1;
        }
    }
    if (argc < i + 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "TAG() expects an index number");
        return err->code;
    }
    double d;
    if (!rv_to_num(&argv[i], &d)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "TAG() index number must be numeric");
        return err->code;
    }
    int area = 0;
    if (argc > i + 1 && area_arg(c, &argv[i + 1], &area, err) != R_OK) {
        return err->code;
    }
    if (argc > i + 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "TAG() expects at most 3 arguments");
        return err->code;
    }
    rx_dbf *dbf;
    if (rx_wa_dbf(area == 0 ? rx_wa_current() : area, &dbf) != R_OK) {
        f_str(out, rx_strdup(""), 0);
        return R_OK;
    }
    char *res = NULL;
    if (!tag_nth(dbf, cdx, (long)d, 0, &res)) {
        f_str(out, rx_strdup(""), 0);
        return R_OK;
    }
    f_str(out, res, strlen(res));
    return R_OK;
}

static int f_key(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    size_t i = 0;
    const char *cdx = NULL;
    if (argc == 0) {
        /* KEY() = key expression of the controlling order tag */
        rx_dbf *dbf;
        if (rx_wa_dbf(0, &dbf) != R_OK) {
            f_str(out, rx_strdup(""), 0);
            return R_OK;
        }
        int fi, ti;
        rx_index_order_get(dbf, &fi, &ti);
        if (fi < 0) {
            f_str(out, rx_strdup(""), 0);
            return R_OK;
        }
        const char *key = "";
        rx_tag_info(dbf, fi, ti, NULL, &key, NULL);
        f_str(out, rx_strdup(key), strlen(key));
        return R_OK;
    }
    if (argc >= 1) {
        double d;
        if (!rv_to_num(&argv[0], &d)) {
            const char *s;
            size_t n;
            if (!rv_to_str(&argv[0], &s, &n)) {
                err->code = R_ERR_TYPE;
                snprintf(err->msg, sizeof(err->msg), "KEY() expects a file name or index number");
                return err->code;
            }
            cdx = s;
            i = 1;
        }
    }
    if (argc < i + 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "KEY() expects an index number");
        return err->code;
    }
    double d;
    if (!rv_to_num(&argv[i], &d)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "KEY() index number must be numeric");
        return err->code;
    }
    int area = 0;
    if (argc > i + 1 && area_arg(c, &argv[i + 1], &area, err) != R_OK) {
        return err->code;
    }
    rx_dbf *dbf;
    if (rx_wa_dbf(area == 0 ? rx_wa_current() : area, &dbf) != R_OK) {
        f_str(out, rx_strdup(""), 0);
        return R_OK;
    }
    char *res = NULL;
    if (!tag_nth(dbf, cdx, (long)d, 1, &res)) {
        f_str(out, rx_strdup(""), 0);
        return R_OK;
    }
    f_str(out, res, strlen(res));
    return R_OK;
}

static int f_tagcount(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    const char *cdx = NULL;
    int area = 0;
    size_t i = 0;
    if (argc >= 1) {
        double d;
        if (!rv_to_num(&argv[0], &d)) {
            const char *s;
            size_t n;
            if (!rv_to_str(&argv[0], &s, &n)) {
                err->code = R_ERR_TYPE;
                snprintf(err->msg, sizeof(err->msg), "TAGCOUNT() expects a file name or area");
                return err->code;
            }
            cdx = s;
            i = 1;
            if (argc > 1 && area_arg(c, &argv[1], &area, err) != R_OK) {
                return err->code;
            }
        } else if (argc > 0 && area_arg(c, &argv[0], &area, err) != R_OK) {
            return err->code;
        }
    }
    (void)i;
    rx_dbf *dbf;
    if (rx_wa_dbf(area == 0 ? rx_wa_current() : area, &dbf) != R_OK) {
        f_num(c, out, 0);
        return R_OK;
    }
    long total = 0;
    int nf = rx_index_file_count(dbf);
    if (cdx) {
        int fi = file_of_name(dbf, cdx, strlen(cdx));
        total = fi >= 0 ? rx_tag_count(dbf, fi) : 0;
    } else {
        for (int fi = 0; fi < nf; fi++) {
            total += rx_tag_count(dbf, fi);
        }
    }
    f_num(c, out, (double)total);
    return R_OK;
}

/* CDX(n [, area]) / MDX(n [, area]) / NDX(n [, area]): nth attached file */
static int f_idxfile(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err, int want_cdx)
{
    if (argc < 1 || argc > 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "CDX()/NDX() expect 1 or 2 arguments");
        return err->code;
    }
    double d;
    if (!rv_to_num(&argv[0], &d)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "Index file number must be numeric");
        return err->code;
    }
    int area = 0;
    if (argc == 2 && area_arg(c, &argv[1], &area, err) != R_OK) {
        return err->code;
    }
    rx_dbf *dbf;
    if (rx_wa_dbf(area == 0 ? rx_wa_current() : area, &dbf) != R_OK) {
        f_str(out, rx_strdup(""), 0);
        return R_OK;
    }
    long k = 0;
    int nf = rx_index_file_count(dbf);
    for (int fi = 0; fi < nf; fi++) {
        if (rx_index_file_is_cdx(dbf, fi) != want_cdx) {
            continue;
        }
        k++;
        if (k == (long)d) {
            const char *p = NULL;
            rx_index_file_path(dbf, fi, &p);
            f_str(out, rx_strdup(p), strlen(p));
            return R_OK;
        }
    }
    f_str(out, rx_strdup(""), 0);
    return R_OK;
}

static int f_cdx(rx_ctx *c, size_t a, const rval *v, rval *o, r_err *e)
{
    return f_idxfile(c, a, v, o, e, 1);
}

static int f_mdx(rx_ctx *c, size_t a, const rval *v, rval *o, r_err *e)
{
    return f_idxfile(c, a, v, o, e, 1);
}

static int f_ndx(rx_ctx *c, size_t a, const rval *v, rval *o, r_err *e)
{
    return f_idxfile(c, a, v, o, e, 0);
}

static int f_keycount(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    const char *cdx = NULL;
    int area = 0;
    if (argc >= 1) {
        double d;
        if (!rv_to_num(&argv[0], &d)) {
            const char *s;
            size_t n;
            if (!rv_to_str(&argv[0], &s, &n)) {
                err->code = R_ERR_TYPE;
                snprintf(err->msg, sizeof(err->msg), "KEYCOUNT() expects a file name or area");
                return err->code;
            }
            cdx = s;
            if (argc > 1 && area_arg(c, &argv[1], &area, err) != R_OK) {
                return err->code;
            }
        } else if (area_arg(c, &argv[0], &area, err) != R_OK) {
            return err->code;
        }
    }
    rx_dbf *dbf;
    if (rx_wa_dbf(area == 0 ? rx_wa_current() : area, &dbf) != R_OK) {
        f_num(c, out, 0);
        return R_OK;
    }
    int fi = -1, ti = -1;
    rx_index_order_get(dbf, &fi, &ti);
    if (cdx) {
        int f2 = file_of_name(dbf, cdx, strlen(cdx));
        if (f2 < 0) {
            f_num(c, out, 0);
            return R_OK;
        }
        /* entries of the controlling tag when it lives in this file, else tag 0 */
        if (fi == f2 && ti >= 0) {
            /* use controlling tag */
        } else {
            fi = f2;
            ti = 0;
            if (rx_tag_count(dbf, fi) == 0) {
                f_num(c, out, 0);
                return R_OK;
            }
        }
    }
    uint32_t n = 0;
    if (fi >= 0 && ti >= 0) {
        rx_index_key_count(dbf, fi, ti, &n);
    }
    f_num(c, out, (double)n);
    return R_OK;
}

static int f_target(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc < 1 || argc > 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "TARGET() expects 1 or 2 arguments");
        return err->code;
    }
    double d;
    if (!rv_to_num(&argv[0], &d)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "TARGET() relation number must be numeric");
        return err->code;
    }
    int area = 0;
    if (argc == 2 && area_arg(c, &argv[1], &area, err) != R_OK) {
        return err->code;
    }
    int driver = area == 0 ? rx_wa_current() : area;
    long k = 0;
    for (size_t i = 0; i < c->nrels; i++) {
        if (c->rels[i].driver != driver) {
            continue;
        }
        k++;
        if (k == (long)d) {
            f_num(c, out, c->rels[i].target);
            return R_OK;
        }
    }
    f_num(c, out, 0);
    return R_OK;
}

static int f_relation(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc < 1 || argc > 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "RELATION() expects 1 or 2 arguments");
        return err->code;
    }
    double d;
    if (!rv_to_num(&argv[0], &d)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "RELATION() number must be numeric");
        return err->code;
    }
    int area = 0;
    if (argc == 2 && area_arg(c, &argv[1], &area, err) != R_OK) {
        return err->code;
    }
    int driver = area == 0 ? rx_wa_current() : area;
    long k = 0;
    for (size_t i = 0; i < c->nrels; i++) {
        if (c->rels[i].driver != driver) {
            continue;
        }
        k++;
        if (k == (long)d) {
            const char *t = c->rels[i].text ? c->rels[i].text : "";
            f_str(out, rx_strdup(t), strlen(t));
            return R_OK;
        }
    }
    f_str(out, rx_strdup(""), 0);
    return R_OK;
}

const rx_funcdef rx_funcs_db[] = {
    { "RECNO", f_recno },     { "RECCOUNT", f_reccount }, { "BOF", f_bof },     { "EOF", f_eof },
    { "DELETED", f_deleted }, { "ALIAS", f_alias },       { "SELECT", f_select }, { "USED", f_used },
    { "FCOUNT", f_fcount },   { "FIELD", f_field },       { "FSIZE", f_fsize }, { "HEADER", f_header },
    { "RECSIZE", f_recsize }, { "FOUND", f_found },       { "LUPDATE", f_lupdate },
    { "FILTER", f_filter },   { "ORDER", f_order },       { "TAG", f_tag },
    { "FLOCK", f_flock },     { "RLOCK", f_rlock },       { "LOCK", f_rlock },
    /* M6 §8.4 index introspection (Appendix A) */
    { "TAGCOUNT", f_tagcount }, { "CDX", f_cdx },         { "MDX", f_mdx },     { "NDX", f_ndx },
    { "KEY", f_key },         { "KEYCOUNT", f_keycount }, { "TARGET", f_target },
    { "RELATION", f_relation },
};

const size_t rx_funcs_db_n = sizeof(rx_funcs_db) / sizeof(rx_funcs_db[0]);
