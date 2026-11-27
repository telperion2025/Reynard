/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file funcs_array.c
 * @brief Array functions (spec §7.5 / Appendix A): AELEMENT, ASUBSCRIPT,
 *        ACOPY, ADEL, AINS, ASCAN, ASORT, AFIELDS, ADIR. Arrays are memvars
 *        created by DIMENSION/DECLARE (M5).
 */
#include "lang/funcs.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "reynard/dbf.h"

static int arr_dims(rx_ctx* c, const rval* v, const char* name, size_t* d1, size_t* d2,
                    r_err* err) {
    const char* s;
    size_t n;
    if (!rv_to_str(v, &s, &n)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() expects an array name", name);
        return -1;
    }
    char nm[64];
    size_t l = n < sizeof(nm) - 1 ? n : sizeof(nm) - 1;
    memcpy(nm, s, l);
    nm[l] = '\0';
    if (rx_array_dims(c, nm, d1, d2) != R_OK) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s is not an array", nm);
        return -1;
    }
    return 0;
}

static size_t elem_num(size_t d1, size_t d2, size_t i, size_t j) {
    (void)d1;
    size_t cols = d2 ? d2 : 1;
    return (i - 1) * cols + (j - 1) + 1;
}

static int f_alen(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 1 || argc > 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ALEN() expects 1-2 arguments");
        return err->code;
    }
    size_t d1, d2;
    if (arr_dims(c, &argv[0], "ALEN", &d1, &d2, err) != 0) {
        return err->code;
    }
    double which = 1;
    if (argc == 2 && !rv_to_num(&argv[1], &which)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ALEN() dimension must be numeric");
        return err->code;
    }
    f_num(c, out, which == 2 ? (double)d2 : (double)d1);
    return R_OK;
}

static int f_aelement(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 2 || argc > 3) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "AELEMENT() expects 2-3 arguments");
        return err->code;
    }
    size_t d1, d2;
    if (arr_dims(c, &argv[0], "AELEMENT", &d1, &d2, err) != 0) {
        return err->code;
    }
    double di, dj = 1;
    if (!rv_to_num(&argv[1], &di)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "AELEMENT() row must be numeric");
        return err->code;
    }
    if (argc == 3 && !rv_to_num(&argv[2], &dj)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "AELEMENT() column must be numeric");
        return err->code;
    }
    size_t i = (size_t)di, j = (size_t)dj;
    if (i < 1 || i > d1 || (d2 && (j < 1 || j > d2))) {
        err->code = R_ERR_RANGE;
        snprintf(err->msg, sizeof(err->msg), "AELEMENT() subscript out of range");
        return err->code;
    }
    f_num(c, out, (double)elem_num(d1, d2, i, j));
    return R_OK;
}

static int f_asubscript(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    /* ASUBSCRIPT(a, n, dim): subscript of element n in dimension dim (1 or 2) */
    if (argc != 3) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ASUBSCRIPT() expects 3 arguments");
        return err->code;
    }
    size_t d1, d2;
    if (arr_dims(c, &argv[0], "ASUBSCRIPT", &d1, &d2, err) != 0) {
        return err->code;
    }
    double dn, dd;
    if (!rv_to_num(&argv[1], &dn) || !rv_to_num(&argv[2], &dd)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ASUBSCRIPT() arguments must be numeric");
        return err->code;
    }
    size_t n = (size_t)dn;
    int dim = (int)dd;
    size_t total = d1 * (d2 ? d2 : 1);
    if (n < 1 || n > total) {
        err->code = R_ERR_RANGE;
        snprintf(err->msg, sizeof(err->msg), "ASUBSCRIPT() element out of range");
        return err->code;
    }
    size_t cols = d2 ? d2 : 1;
    size_t res = (dim == 2) ? ((n - 1) % cols) + 1 : ((n - 1) / cols) + 1;
    f_num(c, out, (double)res);
    return R_OK;
}

static int name_of(rx_ctx* c, const rval* v, char* out, size_t outsz, r_err* err, const char* fn) {
    const char* s;
    size_t n;
    if (!rv_to_str(v, &s, &n)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() expects an array name", fn);
        return -1;
    }
    size_t l = n < outsz - 1 ? n : outsz - 1;
    memcpy(out, s, l);
    out[l] = '\0';
    for (char* q = out; *q; q++) {
        *q = (char)toupper((unsigned char)*q);
    }
    (void)c;
    return 0;
}

static int f_acopy(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 2 || argc > 5) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ACOPY() expects 2-5 arguments");
        return err->code;
    }
    char sn[64], dn[64];
    if (name_of(c, &argv[0], sn, sizeof(sn), err, "ACOPY") != 0 ||
        name_of(c, &argv[1], dn, sizeof(dn), err, "ACOPY") != 0) {
        return err->code;
    }
    size_t sd1, sd2, dd1, dd2;
    if (rx_array_dims(c, sn, &sd1, &sd2) != R_OK) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ACOPY() source is not an array");
        return err->code;
    }
    if (rx_array_dims(c, dn, &dd1, &dd2) != R_OK) {
        /* FoxPro creates the target array sized to the source */
        rx_array_def(c, dn, sd1, sd2);
        dd1 = sd1;
        dd2 = sd2;
    }
    double pstart = 1, pcount = (double)(sd1 * (sd2 ? sd2 : 1)), pdstart = 1;
    if (argc >= 3 && !rv_to_num(&argv[2], &pstart)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ACOPY() start must be numeric");
        return err->code;
    }
    if (argc >= 4 && !rv_to_num(&argv[3], &pcount)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ACOPY() count must be numeric");
        return err->code;
    }
    if (argc >= 5 && !rv_to_num(&argv[4], &pdstart)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ACOPY() target start must be numeric");
        return err->code;
    }
    size_t total_s = sd1 * (sd2 ? sd2 : 1);
    size_t total_d = dd1 * (dd2 ? dd2 : 1);
    size_t start = (size_t)pstart, count = (size_t)pcount, dstart = (size_t)pdstart;
    if (start < 1) {
        start = 1;
    }
    if (count > total_s - start + 1) {
        count = total_s - start + 1;
    }
    if (dstart < 1) {
        dstart = 1;
    }
    if (dstart + count - 1 > total_d) {
        count = total_d - dstart + 1;
    }
    size_t copied = 0;
    for (size_t k = 0; k < count; k++) {
        rval v;
        rx_rval_undef(&v);
        if (rx_array_get(c, sn, start + k, 1, &v) == R_OK) {
            if (rx_array_set(c, dn, dstart + k, 1, &v) == R_OK) {
                copied++;
            }
            rx_rval_free(&v);
        }
    }
    f_num(c, out, (double)copied);
    return R_OK;
}

static int f_adel(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    /* ADEL(a, n [, 2]) — delete element n (or row/column if dim==2) */
    if (argc < 2 || argc > 3) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ADEL() expects 2-3 arguments");
        return err->code;
    }
    char nm[64];
    if (name_of(c, &argv[0], nm, sizeof(nm), err, "ADEL") != 0) {
        return err->code;
    }
    double dn, dd = 1;
    if (!rv_to_num(&argv[1], &dn)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ADEL() element must be numeric");
        return err->code;
    }
    if (argc == 3 && !rv_to_num(&argv[2], &dd)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ADEL() dimension must be numeric");
        return err->code;
    }
    size_t d1, d2;
    if (rx_array_dims(c, nm, &d1, &d2) != R_OK) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ADEL() target is not an array");
        return err->code;
    }
    size_t n = (size_t)dn;
    if ((size_t)dd == 2 && d2 == 0) {
        /* 1-D array treated as single row: deleting column deletes the element */
    }
    size_t total = d1 * (d2 ? d2 : 1);
    if (n < 1 || n > total) {
        err->code = R_ERR_RANGE;
        snprintf(err->msg, sizeof(err->msg), "ADEL() element out of range");
        return err->code;
    }
    /* shift elements down, last becomes undefined (FoxPro semantics) */
    for (size_t k = n; k < total; k++) {
        rval v;
        rx_rval_undef(&v);
        rx_array_get(c, nm, k + 1, 1, &v);
        rx_array_set(c, nm, k, 1, &v);
        rx_rval_free(&v);
    }
    rval u;
    rx_rval_undef(&u);
    rx_array_set(c, nm, total, 1, &u);
    f_logic(out, L_T);
    return R_OK;
}

static int f_ains(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 2 || argc > 3) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "AINS() expects 2-3 arguments");
        return err->code;
    }
    char nm[64];
    if (name_of(c, &argv[0], nm, sizeof(nm), err, "AINS") != 0) {
        return err->code;
    }
    double dn;
    if (!rv_to_num(&argv[1], &dn)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "AINS() element must be numeric");
        return err->code;
    }
    size_t d1, d2;
    if (rx_array_dims(c, nm, &d1, &d2) != R_OK) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "AINS() target is not an array");
        return err->code;
    }
    size_t n = (size_t)dn;
    size_t total = d1 * (d2 ? d2 : 1);
    if (n < 1 || n > total) {
        err->code = R_ERR_RANGE;
        snprintf(err->msg, sizeof(err->msg), "AINS() element out of range");
        return err->code;
    }
    for (size_t k = total; k > n; k--) {
        rval v;
        rx_rval_undef(&v);
        rx_array_get(c, nm, k - 1, 1, &v);
        rx_array_set(c, nm, k, 1, &v);
        rx_rval_free(&v);
    }
    rval u;
    rx_rval_undef(&u);
    rx_array_set(c, nm, n, 1, &u);
    f_logic(out, L_T);
    return R_OK;
}

static int val_eq(rx_ctx* c, const rval* a, const rval* b) {
    /* ASCAN exact comparison: use the VM's comparison via display strings for
       non-numerics, == for numerics/dates/logicals */
    if (a->t == RX_T_N || a->t == RX_T_I || a->t == RX_T_Y || a->t == RX_T_B) {
        double x, y;
        if (rv_to_num(a, &x) && rv_to_num(b, &y)) {
            return x == y;
        }
        return 0;
    }
    if (a->t == RX_T_D && b->t == RX_T_D) {
        return a->u.d.y == b->u.d.y && a->u.d.m == b->u.d.m && a->u.d.d == b->u.d.d;
    }
    if (a->t == RX_T_L && b->t == RX_T_L) {
        return a->u.l == b->u.l;
    }
    const char *x, *y;
    size_t nx, ny;
    if (rv_to_str(a, &x, &nx) && rv_to_str(b, &y, &ny)) {
        return nx == ny && memcmp(x, y, nx) == 0;
    }
    (void)c;
    return 0;
}

static int f_ascan(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 2 || argc > 5) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ASCAN() expects 2-5 arguments");
        return err->code;
    }
    char nm[64];
    if (name_of(c, &argv[0], nm, sizeof(nm), err, "ASCAN") != 0) {
        return err->code;
    }
    size_t d1, d2;
    if (rx_array_dims(c, nm, &d1, &d2) != R_OK) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ASCAN() target is not an array");
        return err->code;
    }
    double pstart = 1, pcount = 0; /* 0 = to end */
    if (argc >= 3 && !rv_to_num(&argv[2], &pstart)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ASCAN() start must be numeric");
        return err->code;
    }
    if (argc >= 4 && !rv_to_num(&argv[3], &pcount)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ASCAN() count must be numeric");
        return err->code;
    }
    size_t total = d1 * (d2 ? d2 : 1);
    size_t start = (size_t)pstart;
    if (start < 1) {
        start = 1;
    }
    size_t count = pcount > 0 ? (size_t)pcount : total - start + 1;
    if (start + count - 1 > total) {
        count = total - start + 1;
    }
    for (size_t k = 0; k < count; k++) {
        rval v;
        rx_rval_undef(&v);
        if (rx_array_get(c, nm, start + k, 1, &v) == R_OK) {
            if (val_eq(c, &v, &argv[1])) {
                rx_rval_free(&v);
                f_num(c, out, (double)(start + k));
                return R_OK;
            }
            rx_rval_free(&v);
        }
    }
    f_num(c, out, 0);
    return R_OK;
}

static int cmp_rval(const void* pa, const void* pb) {
    const rval *a = pa, *b = pb;
    double x, y;
    if (rv_to_num(a, &x) && rv_to_num(b, &y)) {
        return x < y ? -1 : x > y ? 1 : 0;
    }
    const char *x2, *y2;
    size_t nx, ny;
    if (rv_to_str(a, &x2, &nx) && rv_to_str(b, &y2, &ny)) {
        int c = memcmp(x2, y2, nx < ny ? nx : ny);
        if (c) {
            return c;
        }
        return nx < ny ? -1 : nx > ny ? 1 : 0;
    }
    return (int)a->t - (int)b->t;
}

static int f_asort(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 1 || argc > 4) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ASORT() expects 1-4 arguments");
        return err->code;
    }
    char nm[64];
    if (name_of(c, &argv[0], nm, sizeof(nm), err, "ASORT") != 0) {
        return err->code;
    }
    size_t d1, d2;
    if (rx_array_dims(c, nm, &d1, &d2) != R_OK) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ASORT() target is not an array");
        return err->code;
    }
    double pstart = 1, pcount = 0, porder = 0;
    if (argc >= 2 && !rv_to_num(&argv[1], &pstart)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ASORT() start must be numeric");
        return err->code;
    }
    if (argc >= 3 && !rv_to_num(&argv[2], &pcount)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ASORT() count must be numeric");
        return err->code;
    }
    if (argc >= 4 && !rv_to_num(&argv[3], &porder)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ASORT() order must be numeric");
        return err->code;
    }
    size_t total = d1 * (d2 ? d2 : 1);
    size_t start = (size_t)pstart;
    if (start < 1) {
        start = 1;
    }
    size_t count = pcount > 0 ? (size_t)pcount : total - start + 1;
    if (start + count - 1 > total) {
        count = total - start + 1;
    }
    rval* tmp = calloc(count ? count : 1, sizeof(rval));
    if (!tmp) {
        RX_FATAL("out of memory");
    }
    for (size_t k = 0; k < count; k++) {
        rx_rval_undef(&tmp[k]);
        rx_array_get(c, nm, start + k, 1, &tmp[k]);
    }
    qsort(tmp, count, sizeof(rval), cmp_rval);
    if (porder != 0) { /* descending */
        for (size_t k = 0; k < count / 2; k++) {
            rval t = tmp[k];
            tmp[k] = tmp[count - 1 - k];
            tmp[count - 1 - k] = t;
        }
    }
    for (size_t k = 0; k < count; k++) {
        rx_array_set(c, nm, start + k, 1, &tmp[k]);
        rx_rval_free(&tmp[k]);
    }
    free(tmp);
    f_num(c, out, 1);
    return R_OK;
}

static int f_afields(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    /* AFIELDS(a [, workarea]) — rows: name, type, len, dec */
    if (argc < 1 || argc > 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "AFIELDS() expects 1-2 arguments");
        return err->code;
    }
    char nm[64];
    if (name_of(c, &argv[0], nm, sizeof(nm), err, "AFIELDS") != 0) {
        return err->code;
    }
    rx_dbf* dbf;
    int area = 0;
    if (argc == 2) {
        double d;
        if (rv_to_num(&argv[1], &d)) {
            area = (int)d;
        } else {
            const char* s;
            size_t n;
            if (rv_to_str(&argv[1], &s, &n)) {
                for (int i = 1; i <= 255; i++) {
                    if (rx_wa_used(i)) {
                        const char* a;
                        rx_wa_alias(i, &a);
                        if (strlen(a) == n && memcmp(a, s, n) == 0) {
                            area = i;
                            break;
                        }
                    }
                }
            }
        }
    }
    if (rx_wa_dbf(area, &dbf) != R_OK) {
        err->code = R_ERR_DBF_NOTOPEN;
        snprintf(err->msg, sizeof(err->msg), "No table in use");
        return err->code;
    }
    size_t nf;
    rx_dbf_field_count(dbf, &nf);
    rx_array_def(c, nm, nf, 4);
    for (size_t i = 0; i < nf; i++) {
        rx_fielddef fd;
        rx_dbf_field_def(dbf, i, &fd);
        rval v;
        rx_rval_undef(&v);
        v.t = RX_T_C;
        v.u.c.n = strlen(fd.name);
        v.u.c.utf8 = rx_strdup(fd.name);
        rx_array_set(c, nm, i + 1, 1, &v);
        rx_rval_free(&v);
        char ty[2] = {(char)fd.type, 0};
        rx_rval_undef(&v);
        v.t = RX_T_C;
        v.u.c.n = 1;
        v.u.c.utf8 = rx_strdup(ty);
        rx_array_set(c, nm, i + 1, 2, &v);
        rx_rval_free(&v);
        rx_rval_undef(&v);
        v.t = RX_T_I;
        v.u.i = fd.len;
        rx_array_set(c, nm, i + 1, 3, &v);
        rx_rval_undef(&v);
        v.t = RX_T_I;
        v.u.i = fd.dec;
        rx_array_set(c, nm, i + 1, 4, &v);
    }
    f_num(c, out, (double)nf);
    return R_OK;
}

static int f_adir(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    /* ADIR(a, pattern) — rows: name, size, date (D), time (C), attr (C) */
    if (argc < 2 || argc > 3) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ADIR() expects 2-3 arguments");
        return err->code;
    }
    char nm[64];
    if (name_of(c, &argv[0], nm, sizeof(nm), err, "ADIR") != 0) {
        return err->code;
    }
    const char* pat;
    size_t pn;
    if (!rv_to_str(&argv[1], &pat, &pn)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ADIR() pattern must be a string");
        return err->code;
    }
    char patb[260];
    size_t l = pn < sizeof(patb) - 1 ? pn : sizeof(patb) - 1;
    memcpy(patb, pat, l);
    patb[l] = '\0';
    DIR* dir = opendir(".");
    if (!dir) {
        err->code = R_ERR_IO_OPEN;
        snprintf(err->msg, sizeof(err->msg), "ADIR() cannot open directory");
        return err->code;
    }
    char** names = NULL;
    size_t nn = 0, cap = 0;
    struct dirent* e;
    while ((e = readdir(dir))) {
        if (rx_wildmatch(patb, e->d_name)) {
            if (nn == cap) {
                cap = cap ? cap * 2 : 32;
                names = realloc(names, cap * sizeof(char*));
                if (!names) {
                    RX_FATAL("out of memory");
                }
            }
            names[nn++] = rx_strdup(e->d_name);
        }
    }
    closedir(dir);
    rx_array_def(c, nm, nn, 5);
    for (size_t i = 0; i < nn; i++) {
        struct stat st;
        rval v;
        rx_rval_undef(&v);
        v.t = RX_T_C;
        v.u.c.n = strlen(names[i]);
        v.u.c.utf8 = rx_strdup(names[i]);
        rx_array_set(c, nm, i + 1, 1, &v);
        rx_rval_free(&v);
        long sz = stat(names[i], &st) == 0 ? (long)st.st_size : 0;
        rx_rval_undef(&v);
        v.t = RX_T_I;
        v.u.i = (int32_t)sz;
        rx_array_set(c, nm, i + 1, 2, &v);
        rx_rval_undef(&v);
        if (stat(names[i], &st) == 0) {
            struct tm* tm = localtime(&st.st_mtime);
            v.t = RX_T_D;
            v.u.d.y = (int16_t)(tm->tm_year + 1900);
            v.u.d.m = (int16_t)(tm->tm_mon + 1);
            v.u.d.d = (int16_t)tm->tm_mday;
        } else {
            rx_rval_undef(&v);
        }
        rx_array_set(c, nm, i + 1, 3, &v);
        rx_rval_undef(&v);
        if (stat(names[i], &st) == 0) {
            struct tm* tm = localtime(&st.st_mtime);
            char tb[16];
            snprintf(tb, sizeof(tb), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
            v.t = RX_T_C;
            v.u.c.n = strlen(tb);
            v.u.c.utf8 = rx_strdup(tb);
        } else {
            v.t = RX_T_C;
            v.u.c.n = 0;
            v.u.c.utf8 = rx_strdup("");
        }
        rx_array_set(c, nm, i + 1, 4, &v);
        rx_rval_free(&v);
        rx_rval_undef(&v);
        v.t = RX_T_C;
        v.u.c.n = 1;
        v.u.c.utf8 = rx_strdup(stat(names[i], &st) == 0 && S_ISDIR(st.st_mode) ? "D" : "F");
        rx_array_set(c, nm, i + 1, 5, &v);
        rx_rval_free(&v);
        free(names[i]);
    }
    free(names);
    f_num(c, out, (double)nn);
    return R_OK;
}

const rx_funcdef rx_funcs_array[] = {
    {"AELEMENT", f_aelement}, {"ASUBSCRIPT", f_asubscript},
    {"ACOPY", f_acopy},       {"ADEL", f_adel},
    {"AINS", f_ains},         {"ASCAN", f_ascan},
    {"ASORT", f_asort},       {"AFIELDS", f_afields},
    {"ADIR", f_adir},         {"ALEN", f_alen},
};
const size_t rx_funcs_array_n = sizeof(rx_funcs_array) / sizeof(rx_funcs_array[0]);
