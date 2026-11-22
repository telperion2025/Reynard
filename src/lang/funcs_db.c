/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file funcs_db.c
 * @brief Database introspection functions (spec Appendix A, funcs_db.c).
 *        They read the documented work-area table (include/reynard/dbf.h).
 */
#include "lang/funcs.h"

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
    /* M1: no LOCATE yet; FOUND() is false unless a seek landed (M6) */
    f_logic(out, L_F);
    (void)c;
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
    /* read-only engine at M1: no updates, report empty date */
    (void)dbf;
    r_date d = { 0, 0, 0 };
    f_date(out, d);
    (void)c;
    return R_OK;
}

static int f_empty_str(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err,
                       const char *nm)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() expects no arguments", nm);
        return err->code;
    }
    (void)argv;
    f_str(out, rx_strdup(""), 0);
    (void)c;
    return R_OK;
}

static int f_filter(rx_ctx *c, size_t a, const rval *v, rval *o, r_err *e)
{
    return f_empty_str(c, a, v, o, e, "FILTER");
}

static int f_order(rx_ctx *c, size_t a, const rval *v, rval *o, r_err *e)
{
    return f_empty_str(c, a, v, o, e, "ORDER");
}

static int f_tag(rx_ctx *c, size_t a, const rval *v, rval *o, r_err *e)
{
    return f_empty_str(c, a, v, o, e, "TAG");
}

const rx_funcdef rx_funcs_db[] = {
    { "RECNO", f_recno },     { "RECCOUNT", f_reccount }, { "BOF", f_bof },     { "EOF", f_eof },
    { "DELETED", f_deleted }, { "ALIAS", f_alias },       { "SELECT", f_select }, { "USED", f_used },
    { "FCOUNT", f_fcount },   { "FIELD", f_field },       { "FSIZE", f_fsize }, { "HEADER", f_header },
    { "RECSIZE", f_recsize }, { "FOUND", f_found },       { "LUPDATE", f_lupdate },
    { "FILTER", f_filter },   { "ORDER", f_order },       { "TAG", f_tag },
};

const size_t rx_funcs_db_n = sizeof(rx_funcs_db) / sizeof(rx_funcs_db[0]);
