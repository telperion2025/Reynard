/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file funcs_date.c
 * @brief Date/time functions (spec Appendix A, funcs_date.c). In -t test
 *        mode DATE() is frozen to 2000-01-01, TIME() to "00:00:00",
 *        SECONDS() to 0 (spec §3.3, session seed 1).
 */
#include "lang/funcs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/util.h"

#define RX_TEST_YEAR 2000
#define RX_TEST_MONTH 1
#define RX_TEST_DAY 1

static r_date test_date(void) {
    r_date d = {RX_TEST_YEAR, RX_TEST_MONTH, RX_TEST_DAY};
    return d;
}

static r_date now_date(void) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    r_date d = {(int16_t)(tmv.tm_year + 1900), (int16_t)(tmv.tm_mon + 1), (int16_t)tmv.tm_mday};
    return d;
}

/** Extract a date from a D/T value; character accepted as YYYYMMDD or
 *  MM/DD/YY[YY]. Returns R_OK or an error code (err filled). */
static int arg_date(const rval* v, r_date* out, r_err* err) {
    memset(out, 0, sizeof(*out));
    if (v->t == RX_T_D) {
        *out = v->u.d;
        return R_OK;
    }
    if (v->t == RX_T_T) {
        *out = v->u.tm.d;
        return R_OK;
    }
    if (v->t == RX_T_C) {
        const char* s = v->u.c.utf8 ? v->u.c.utf8 : "";
        size_t n = v->u.c.n;
        int y = 0, m = 0, d = 0;
        if (n == 8 && s[4] != '/') {
            if (sscanf(s, "%4d%2d%2d", &y, &m, &d) == 3 && rx_date_valid(y, m, d)) {
                out->y = (int16_t)y;
                out->m = (int16_t)m;
                out->d = (int16_t)d;
                return R_OK;
            }
        } else if (sscanf(s, "%d/%d/%d", &m, &d, &y) == 3) {
            if (y < 100) {
                y += y < 30 ? 2000 : 1900; /* FoxPro century pivot */
            }
            if (rx_date_valid(y, m, d)) {
                out->y = (int16_t)y;
                out->m = (int16_t)m;
                out->d = (int16_t)d;
                return R_OK;
            }
        }
    }
    err->code = R_ERR_TYPE;
    snprintf(err->msg, sizeof(err->msg), "date argument expected");
    return err->code;
}

static int f_datefn(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "DATE() expects no arguments");
        return err->code;
    }
    (void)argv;
    f_date(out, c->test_mode ? test_date() : now_date());
    return R_OK;
}

static int f_time(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "TIME() expects no arguments");
        return err->code;
    }
    (void)argv;
    const char* t = "00:00:00";
    if (!c->test_mode) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        static _Thread_local char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        t = buf;
    }
    f_str(out, rx_strdup(t), strlen(t));
    return R_OK;
}

static int f_seconds(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "SECONDS() expects no arguments");
        return err->code;
    }
    (void)argv;
    double s = 0;
    if (!c->test_mode) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        s = tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec;
    }
    f_num(c, out, s);
    return R_OK;
}

static int d_part(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err, int part,
                  const char* nm) {
    if (f_argc(err, argc, 1, nm) != 0) {
        return err->code;
    }
    r_date d;
    int rc = arg_date(&argv[0], &d, err);
    if (rc != R_OK) {
        return rc;
    }
    switch (part) {
    case 0:
        f_num(c, out, d.y);
        break;
    case 1:
        f_num(c, out, d.m);
        break;
    case 2:
        f_num(c, out, d.d);
        break;
    case 3: { /* DOW: 1 = Sunday */
        long j = rx_julian(d.y, d.m, d.d);
        long dow = (j + 1) % 7;
        if (dow < 0) {
            dow += 7;
        }
        f_num(c, out, (double)(dow + 1));
        break;
    }
    case 4: { /* CDOW */
        static const char* names[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                      "Thursday", "Friday", "Saturday"};
        long j = rx_julian(d.y, d.m, d.d);
        long dow = (j + 1) % 7;
        if (dow < 0) {
            dow += 7;
        }
        f_str(out, rx_strdup(names[dow]), strlen(names[dow]));
        break;
    }
    case 5: { /* CMONTH */
        static const char* mons[] = {"January",   "February", "March",    "April",
                                     "May",       "June",     "July",     "August",
                                     "September", "October",  "November", "December"};
        f_str(out, rx_strdup(mons[d.m - 1]), strlen(mons[d.m - 1]));
        break;
    }
    case 6: { /* QUARTER */
        f_num(c, out, (d.m - 1) / 3 + 1);
        break;
    }
    case 7: { /* WEEK: week 1 contains Jan 1, weeks start Sunday */
        long jan1 = rx_julian(d.y, 1, 1);
        long cur = rx_julian(d.y, d.m, d.d);
        long dow_jan1 = (jan1 + 1) % 7;
        if (dow_jan1 < 0) {
            dow_jan1 += 7;
        }
        f_num(c, out, (double)((cur - jan1 + dow_jan1) / 7 + 1));
        break;
    }
    default:
        break;
    }
    return R_OK;
}

static int f_year(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return d_part(c, a, v, o, e, 0, "YEAR");
}

static int f_month(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return d_part(c, a, v, o, e, 1, "MONTH");
}

static int f_day(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return d_part(c, a, v, o, e, 2, "DAY");
}

static int f_dow(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return d_part(c, a, v, o, e, 3, "DOW");
}

static int f_cdow(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return d_part(c, a, v, o, e, 4, "CDOW");
}

static int f_cmonth(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return d_part(c, a, v, o, e, 5, "CMONTH");
}

static int f_quarter(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return d_part(c, a, v, o, e, 6, "QUARTER");
}

static int f_week(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return d_part(c, a, v, o, e, 7, "WEEK");
}

static int f_dtos(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "DTOS") != 0) {
        return err->code;
    }
    r_date d;
    int rc = arg_date(&argv[0], &d, err);
    if (rc != R_OK) {
        return rc;
    }
    char buf[48];
    if (d.y == 0) {
        snprintf(buf, sizeof(buf), "00000000");
    } else {
        snprintf(buf, sizeof(buf), "%04d%02d%02d", d.y, d.m, d.d);
    }
    f_str(out, rx_strdup(buf), strlen(buf));
    (void)c;
    return R_OK;
}

static int f_dtoc(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 1 || argc > 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "DTOC() expects 1 or 2 arguments");
        return err->code;
    }
    r_date d;
    int rc = arg_date(&argv[0], &d, err);
    if (rc != R_OK) {
        return rc;
    }
    char buf[48];
    if (d.y == 0) {
        snprintf(buf, sizeof(buf), "  /  /%s", c->set.century ? "    " : "  ");
    } else if (c->set.century) {
        snprintf(buf, sizeof(buf), "%02d/%02d/%04d", d.m, d.d, d.y);
    } else {
        snprintf(buf, sizeof(buf), "%02d/%02d/%02d", d.m, d.d, d.y % 100);
    }
    f_str(out, rx_strdup(buf), strlen(buf));
    return R_OK;
}

static int f_ctod(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "CTOD") != 0) {
        return err->code;
    }
    r_date d;
    int rc = arg_date(&argv[0], &d, err);
    if (rc != R_OK) {
        /* FoxPro CTOD("") is the empty date, not an error */
        const char* s;
        size_t n;
        if (rv_to_str(&argv[0], &s, &n) && n == 0) {
            memset(&d, 0, sizeof(d));
            f_date(out, d);
            return R_OK;
        }
        return rc;
    }
    f_date(out, d);
    (void)c;
    return R_OK;
}

static int f_gomonth(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    (void)c;
    if (f_argc(err, argc, 2, "GOMONTH") != 0) {
        return err->code;
    }
    r_date d;
    int rc = arg_date(&argv[0], &d, err);
    if (rc != R_OK) {
        return rc;
    }
    double k;
    if (!rv_to_num(&argv[1], &k)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "GOMONTH() expects numeric months");
        return err->code;
    }
    long total = (long)d.y * 12 + (d.m - 1) + (long)k;
    long y = total / 12;
    long m = total % 12 + 1;
    if (m < 1) {
        m += 12;
        y -= 1;
    }
    int dd = d.d;
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    int maxd = mdays[m - 1] + (m == 2 && leap ? 1 : 0);
    if (dd > maxd) {
        dd = maxd;
    }
    r_date r = {(int16_t)y, (int16_t)m, (int16_t)dd};
    f_date(out, r);
    return R_OK;
}

static int f_mdy(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "MDY") != 0) {
        return err->code;
    }
    r_date d;
    int rc = arg_date(&argv[0], &d, err);
    if (rc != R_OK) {
        return rc;
    }
    static const char* mons[] = {"January",   "February", "March",    "April",
                                 "May",       "June",     "July",     "August",
                                 "September", "October",  "November", "December"};
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %02d, %04d", mons[d.m - 1], d.d, d.y);
    f_str(out, rx_strdup(buf), strlen(buf));
    (void)c;
    return R_OK;
}

static int f_dmy(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "DMY") != 0) {
        return err->code;
    }
    r_date d;
    int rc = arg_date(&argv[0], &d, err);
    if (rc != R_OK) {
        return rc;
    }
    static const char* mons[] = {"January",   "February", "March",    "April",
                                 "May",       "June",     "July",     "August",
                                 "September", "October",  "November", "December"};
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d %s %04d", d.d, mons[d.m - 1], d.y);
    f_str(out, rx_strdup(buf), strlen(buf));
    (void)c;
    return R_OK;
}

static int dt_part(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err, int part,
                   const char* nm) {
    if (f_argc(err, argc, 1, nm) != 0) {
        return err->code;
    }
    if (argv[0].t != RX_T_T) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() expects datetime", nm);
        return err->code;
    }
    switch (part) {
    case 0:
        f_num(c, out, argv[0].u.tm.ms / 3600000);
        break;
    case 1:
        f_num(c, out, (argv[0].u.tm.ms / 60000) % 60);
        break;
    default:
        f_num(c, out, (argv[0].u.tm.ms / 1000) % 60);
        break;
    }
    return R_OK;
}

static int f_hour(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return dt_part(c, a, v, o, e, 0, "HOUR");
}

static int f_minute(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return dt_part(c, a, v, o, e, 1, "MINUTE");
}

static int f_sec(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return dt_part(c, a, v, o, e, 2, "SEC");
}

const rx_funcdef rx_funcs_date[] = {
    {"DATE", f_datefn},     {"TIME", f_time}, {"SECONDS", f_seconds}, {"YEAR", f_year},
    {"MONTH", f_month},     {"DAY", f_day},   {"DOW", f_dow},         {"CDOW", f_cdow},
    {"CMONTH", f_cmonth},   {"DTOS", f_dtos}, {"DTOC", f_dtoc},       {"CTOD", f_ctod},
    {"GOMONTH", f_gomonth}, {"MDY", f_mdy},   {"DMY", f_dmy},         {"QUARTER", f_quarter},
    {"WEEK", f_week},       {"HOUR", f_hour}, {"MINUTE", f_minute},   {"SEC", f_sec},
};

const size_t rx_funcs_date_n = sizeof(rx_funcs_date) / sizeof(rx_funcs_date[0]);
