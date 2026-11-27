/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file funcs_num.c
 * @brief Numeric functions (spec Appendix A, funcs_num.c). RAND() is
 *        seed-frozen in -t test mode (spec §3.3).
 */
#include "lang/funcs.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "util/util.h"

static int unary_num(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err,
                     double (*fn)(double), const char* nm) {
    if (f_argc(err, argc, 1, nm) != 0) {
        return err->code;
    }
    double d;
    if (!rv_to_num(&argv[0], &d)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() expects numeric", nm);
        return err->code;
    }
    f_num(c, out, fn(d));
    return R_OK;
}

static int f_abs(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return unary_num(c, a, v, o, e, fabs, "ABS");
}

static int f_intfn(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return unary_num(c, a, v, o, e, floor, "INT");
}

static int f_ceiling(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return unary_num(c, a, v, o, e, ceil, "CEILING");
}

static int f_floor(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return unary_num(c, a, v, o, e, floor, "FLOOR");
}

static int f_exp(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return unary_num(c, a, v, o, e, exp, "EXP");
}

static int f_log(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return unary_num(c, a, v, o, e, log, "LOG");
}

static int f_log10(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return unary_num(c, a, v, o, e, log10, "LOG10");
}

static int f_sqrt(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return unary_num(c, a, v, o, e, sqrt, "SQRT");
}

static int f_sign(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "SIGN") != 0) {
        return err->code;
    }
    double d;
    if (!rv_to_num(&argv[0], &d)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "SIGN() expects numeric");
        return err->code;
    }
    f_num(c, out, d > 0 ? 1 : d < 0 ? -1 : 0);
    return R_OK;
}

static int f_round(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 1 || argc > 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ROUND() expects 1 or 2 arguments");
        return err->code;
    }
    double d;
    int dec = 0;
    if (!rv_to_num(&argv[0], &d)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "ROUND() expects numeric");
        return err->code;
    }
    if (argc == 2) {
        double dd;
        if (!rv_to_num(&argv[1], &dd)) {
            err->code = R_ERR_TYPE;
            return err->code;
        }
        dec = (int)dd;
    }
    double m = pow(10.0, dec);
    f_num(c, out, floor(d * m + (d >= 0 ? 0.5 : -0.5)) / m);
    return R_OK;
}

static int f_mod(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 2, "MOD") != 0) {
        return err->code;
    }
    double a, b;
    if (!rv_to_num(&argv[0], &a) || !rv_to_num(&argv[1], &b)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "MOD() expects numeric");
        return err->code;
    }
    if (b == 0) {
        err->code = R_ERR_DIVZERO;
        snprintf(err->msg, sizeof(err->msg), "MOD() division by zero");
        return err->code;
    }
    f_num(c, out, fmod(a, b));
    return R_OK;
}

static int f_rand(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc > 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "RAND() expects 0 or 1 arguments");
        return err->code;
    }
    if (argc == 1) {
        double s;
        if (!rv_to_num(&argv[0], &s)) {
            err->code = R_ERR_TYPE;
            return err->code;
        }
        c->rng = (uint32_t)s;
    }
    c->rng = c->rng * 1103515245u + 12345u;
    f_num(c, out, (double)((c->rng >> 16) & 0x7FFFu) / 32768.0);
    return R_OK;
}

static int f_minmax(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err, int ismax) {
    const char* nm = ismax ? "MAX" : "MIN";
    if (argc < 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() needs arguments", nm);
        return err->code;
    }
    rval best = argv[0];
    double bd = 0;
    int numeric = rv_to_num(&argv[0], &bd);
    for (size_t i = 1; i < argc; i++) {
        double d;
        int num = rv_to_num(&argv[i], &d);
        if (numeric && num) {
            if ((ismax && d > bd) || (!ismax && d < bd)) {
                bd = d;
                best = argv[i];
            }
        } else if (!numeric && argv[i].t == RX_T_C && argv[0].t == RX_T_C) {
            if ((ismax && rstr_cmp(&argv[i].u.c, &best.u.c) > 0) ||
                (!ismax && rstr_cmp(&argv[i].u.c, &best.u.c) < 0)) {
                best = argv[i];
            }
        } else {
            err->code = R_ERR_TYPE;
            snprintf(err->msg, sizeof(err->msg), "%s() operand type mismatch", nm);
            return err->code;
        }
    }
    if (numeric) {
        f_num(c, out, bd);
    } else {
        const char* s = "";
        size_t n = 0;
        rv_to_str(&best, &s, &n);
        char* copy = malloc(n + 1);
        if (!copy) {
            RX_FATAL("out of memory");
        }
        memcpy(copy, s, n);
        copy[n] = '\0';
        f_str(out, copy, n);
    }
    return R_OK;
}

static int f_max(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return f_minmax(c, a, v, o, e, 1);
}

static int f_min(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return f_minmax(c, a, v, o, e, 0);
}

static int f_between(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    (void)c;
    if (f_argc(err, argc, 3, "BETWEEN") != 0) {
        return err->code;
    }
    double x, lo, hi;
    if (rv_to_num(&argv[0], &x) && rv_to_num(&argv[1], &lo) && rv_to_num(&argv[2], &hi)) {
        f_logic(out, (x >= lo && x <= hi) ? L_T : L_F);
        return R_OK;
    }
    if (argv[0].t == RX_T_C && argv[1].t == RX_T_C && argv[2].t == RX_T_C) {
        f_logic(out, (rstr_cmp(&argv[0].u.c, &argv[1].u.c) >= 0 &&
                      rstr_cmp(&argv[0].u.c, &argv[2].u.c) <= 0)
                         ? L_T
                         : L_F);
        return R_OK;
    }
    err->code = R_ERR_TYPE;
    snprintf(err->msg, sizeof(err->msg), "BETWEEN() type mismatch");
    return err->code;
}

static int f_iif(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    (void)c;
    if (f_argc(err, argc, 3, "IIF") != 0) {
        return err->code;
    }
    rx_logic l;
    if (!rv_to_logic(&argv[0], &l)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "IIF() condition must be logical");
        return err->code;
    }
    const rval* pick = (l == L_T) ? &argv[1] : &argv[2];
    rx_rval_free(out);
    *out = *pick; /* shallow: function results are consumed immediately */
    if (pick->t == RX_T_C) {
        char* copy = malloc(pick->u.c.n + 1);
        if (!copy) {
            RX_FATAL("out of memory");
        }
        memcpy(copy, pick->u.c.utf8, pick->u.c.n + 1);
        out->u.c.utf8 = copy;
    }
    return R_OK;
}

static int f_fv(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 3, "FV") != 0) {
        return err->code;
    }
    double pmt, rate, nper;
    if (!rv_to_num(&argv[0], &pmt) || !rv_to_num(&argv[1], &rate) || !rv_to_num(&argv[2], &nper)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "FV() expects numeric");
        return err->code;
    }
    double r = rate / 12.0;
    double fv = r == 0 ? -pmt * nper : -pmt * (pow(1 + r, nper) - 1) / r;
    f_num(c, out, fv);
    return R_OK;
}

static int f_pv(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 3, "PV") != 0) {
        return err->code;
    }
    double pmt, rate, nper;
    if (!rv_to_num(&argv[0], &pmt) || !rv_to_num(&argv[1], &rate) || !rv_to_num(&argv[2], &nper)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "PV() expects numeric");
        return err->code;
    }
    double r = rate / 12.0;
    double pv = r == 0 ? -pmt * nper : -pmt * (1 - pow(1 + r, -nper)) / r;
    f_num(c, out, pv);
    return R_OK;
}

static int f_payment(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 3, "PAYMENT") != 0) {
        return err->code;
    }
    double prin, rate, nper;
    if (!rv_to_num(&argv[0], &prin) || !rv_to_num(&argv[1], &rate) || !rv_to_num(&argv[2], &nper)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "PAYMENT() expects numeric");
        return err->code;
    }
    double r = rate / 12.0;
    double pay = r == 0 ? -prin / nper : -prin * r / (1 - pow(1 + r, -nper));
    f_num(c, out, pay);
    return R_OK;
}

static int f_pi(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc != 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "PI() expects no arguments");
        return err->code;
    }
    (void)argv;
    f_num(c, out, 3.14159265358979323846);
    return R_OK;
}

const rx_funcdef rx_funcs_num[] = {
    {"ABS", f_abs},   {"CEILING", f_ceiling}, {"FLOOR", f_floor},     {"EXP", f_exp},
    {"INT", f_intfn}, {"LOG", f_log},         {"LOG10", f_log10},     {"SQRT", f_sqrt},
    {"SIGN", f_sign}, {"ROUND", f_round},     {"MOD", f_mod},         {"RAND", f_rand},
    {"MAX", f_max},   {"MIN", f_min},         {"BETWEEN", f_between}, {"IIF", f_iif},
    {"FV", f_fv},     {"PV", f_pv},           {"PAYMENT", f_payment}, {"PI", f_pi},
};

const size_t rx_funcs_num_n = sizeof(rx_funcs_num) / sizeof(rx_funcs_num[0]);
