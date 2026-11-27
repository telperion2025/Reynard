/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file funcs_str.c
 * @brief String and introspection functions (spec Appendix A, funcs_str.c).
 */
#include "lang/funcs.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "reynard/dbf.h"
#include "util/util.h"

static char* dup_range(const char* s, size_t n, size_t off, size_t len) {
    if (off > n) {
        off = n;
    }
    if (len > n - off) {
        len = n - off;
    }
    char* r = malloc(len + 1);
    if (!r) {
        RX_FATAL("out of memory");
    }
    memcpy(r, s + off, len);
    r[len] = '\0';
    return r;
}

static int f_len(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "LEN") != 0) {
        return err->code;
    }
    const char* s;
    size_t n;
    if (rv_to_str(&argv[0], &s, &n)) {
        f_num(c, out, (double)n);
        return R_OK;
    }
    double d;
    if (rv_to_num(&argv[0], &d)) {
        char tmp[64];
        rx_format_num(tmp, sizeof(tmp), d, c->set.decimals, c->set.fixed);
        f_num(c, out, (double)strlen(tmp));
        return R_OK;
    }
    if (argv[0].t == RX_T_D) {
        f_num(c, out, 8);
        return R_OK;
    }
    err->code = R_ERR_TYPE;
    snprintf(err->msg, sizeof(err->msg), "LEN() data type mismatch");
    return err->code;
}

static int f_upper(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "UPPER") != 0) {
        return err->code;
    }
    const char* s;
    size_t n;
    if (!rv_to_str(&argv[0], &s, &n)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "UPPER() expects character");
        return err->code;
    }
    char* r = dup_range(s, n, 0, n);
    for (size_t i = 0; i < n; i++) {
        r[i] = (char)toupper((unsigned char)r[i]);
    }
    f_str(out, r, n);
    (void)c;
    return R_OK;
}

static int f_lower(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "LOWER") != 0) {
        return err->code;
    }
    const char* s;
    size_t n;
    if (!rv_to_str(&argv[0], &s, &n)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "LOWER() expects character");
        return err->code;
    }
    char* r = dup_range(s, n, 0, n);
    for (size_t i = 0; i < n; i++) {
        r[i] = (char)tolower((unsigned char)r[i]);
    }
    f_str(out, r, n);
    (void)c;
    return R_OK;
}

static int trim_impl(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err, int mode) {
    const char* nm = mode == 0 ? "TRIM" : mode == 1 ? "LTRIM" : mode == 2 ? "RTRIM" : "ALLTRIM";
    if (f_argc(err, argc, 1, nm) != 0) {
        return err->code;
    }
    const char* s;
    size_t n;
    if (!rv_to_str(&argv[0], &s, &n)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() expects character", nm);
        return err->code;
    }
    size_t a = 0, b = n;
    if (mode != 2) { /* not RTRIM: trim left */
        while (a < b && isspace((unsigned char)s[a])) {
            a++;
        }
    }
    if (mode != 1) { /* not LTRIM: trim right */
        while (b > a && isspace((unsigned char)s[b - 1])) {
            b--;
        }
    }
    if (mode == 3) { /* ALLTRIM trims both, including interior runs kept */
    }
    f_str(out, dup_range(s, n, a, b - a), b - a);
    (void)c;
    return R_OK;
}

static int f_trim(rx_ctx* c, size_t argc, const rval* a, rval* o, r_err* e) {
    return trim_impl(c, argc, a, o, e, 0);
}

static int f_ltrim(rx_ctx* c, size_t argc, const rval* a, rval* o, r_err* e) {
    return trim_impl(c, argc, a, o, e, 1);
}

static int f_rtrim(rx_ctx* c, size_t argc, const rval* a, rval* o, r_err* e) {
    return trim_impl(c, argc, a, o, e, 2);
}

static int f_alltrim(rx_ctx* c, size_t argc, const rval* a, rval* o, r_err* e) {
    return trim_impl(c, argc, a, o, e, 3);
}

static int f_left(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc != 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "LEFT() expects 2 arguments");
        return err->code;
    }
    const char* s;
    size_t n;
    double k;
    if (!rv_to_str(&argv[0], &s, &n) || !rv_to_num(&argv[1], &k)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "LEFT() type mismatch");
        return err->code;
    }
    size_t len = k < 0 ? 0 : (size_t)k;
    f_str(out, dup_range(s, n, 0, len), len > n ? n : len);
    (void)c;
    return R_OK;
}

static int f_right(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc != 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "RIGHT() expects 2 arguments");
        return err->code;
    }
    const char* s;
    size_t n;
    double k;
    if (!rv_to_str(&argv[0], &s, &n) || !rv_to_num(&argv[1], &k)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "RIGHT() type mismatch");
        return err->code;
    }
    size_t len = k < 0 ? 0 : (size_t)k;
    size_t off = len > n ? 0 : n - len;
    f_str(out, dup_range(s, n, off, n - off), n - off);
    (void)c;
    return R_OK;
}

static int f_substr(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 2 || argc > 3) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "SUBSTR() expects 2 or 3 arguments");
        return err->code;
    }
    const char* s;
    size_t n;
    double start, len = -1;
    if (!rv_to_str(&argv[0], &s, &n) || !rv_to_num(&argv[1], &start)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "SUBSTR() type mismatch");
        return err->code;
    }
    if (argc == 3 && !rv_to_num(&argv[2], &len)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "SUBSTR() type mismatch");
        return err->code;
    }
    if (start < 1) {
        start = 1;
    }
    size_t off = (size_t)start - 1;
    if (off > n) {
        off = n;
    }
    size_t avail = n - off;
    size_t take = len < 0 ? avail : (size_t)len;
    if (take > avail) {
        take = avail;
    }
    f_str(out, dup_range(s, n, off, take), take);
    (void)c;
    return R_OK;
}

static int at_impl(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err, int caseless) {
    const char* nm = caseless ? "ATC" : "AT";
    if (f_argc(err, argc, 2, nm) != 0) {
        return err->code;
    }
    const char *needle, *hay;
    size_t nn, hn;
    if (!rv_to_str(&argv[0], &needle, &nn) || !rv_to_str(&argv[1], &hay, &hn)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() expects character arguments", nm);
        return err->code;
    }
    size_t found = 0;
    if (nn == 0) {
        found = 1;
    } else {
        for (size_t i = 0; i + nn <= hn; i++) {
            int ok = 1;
            for (size_t j = 0; j < nn; j++) {
                char a = hay[i + j], b = needle[j];
                if (caseless) {
                    a = (char)tolower((unsigned char)a);
                    b = (char)tolower((unsigned char)b);
                }
                if (a != b) {
                    ok = 0;
                    break;
                }
            }
            if (ok) {
                found = i + 1;
                break;
            }
        }
    }
    f_num(c, out, (double)found);
    return R_OK;
}

static int f_at(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return at_impl(c, a, v, o, e, 0);
}

static int f_atc(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return at_impl(c, a, v, o, e, 1);
}

static int f_rat(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 2, "RAT") != 0) {
        return err->code;
    }
    const char *needle, *hay;
    size_t nn, hn;
    if (!rv_to_str(&argv[0], &needle, &nn) || !rv_to_str(&argv[1], &hay, &hn)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "RAT() expects character arguments");
        return err->code;
    }
    size_t found = 0;
    if (nn == 0) {
        found = hn + 1;
    } else {
        for (size_t i = hn; i-- > 0;) {
            if (i + nn <= hn && memcmp(hay + i, needle, nn) == 0) {
                found = i + 1;
                break;
            }
        }
    }
    f_num(c, out, (double)found);
    return R_OK;
}

static int f_occurs(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 2, "OCCURS") != 0) {
        return err->code;
    }
    const char *needle, *hay;
    size_t nn, hn;
    if (!rv_to_str(&argv[0], &needle, &nn) || !rv_to_str(&argv[1], &hay, &hn)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "OCCURS() expects character arguments");
        return err->code;
    }
    int count = 0;
    if (nn > 0) {
        for (size_t i = 0; i + nn <= hn;) {
            if (memcmp(hay + i, needle, nn) == 0) {
                count++;
                i += nn;
            } else {
                i++;
            }
        }
    }
    f_num(c, out, count);
    return R_OK;
}

static int f_chr(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "CHR") != 0) {
        return err->code;
    }
    double d;
    if (!rv_to_num(&argv[0], &d)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "CHR() expects numeric");
        return err->code;
    }
    char tmp[5];
    size_t n = utf8_encode((uint32_t)(d < 0 ? 0 : d), tmp);
    f_str(out, dup_range(tmp, n, 0, n), n);
    (void)c;
    return R_OK;
}

static int f_asc(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "ASC") != 0) {
        return err->code;
    }
    const char* s;
    size_t n;
    if (!rv_to_str(&argv[0], &s, &n) || n == 0) {
        f_num(c, out, 0);
        return R_OK;
    }
    uint32_t cp;
    utf8_decode(s, n, &cp);
    f_num(c, out, (double)cp);
    return R_OK;
}

static int f_space(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "SPACE") != 0) {
        return err->code;
    }
    double k;
    if (!rv_to_num(&argv[0], &k) || k < 0) {
        k = 0;
    }
    size_t n = (size_t)k;
    if (n > 65500) {
        err->code = R_ERR_STR_OVERFLOW;
        snprintf(err->msg, sizeof(err->msg), "SPACE() count too large");
        return err->code;
    }
    char* r = malloc(n + 1);
    if (!r) {
        RX_FATAL("out of memory");
    }
    memset(r, ' ', n);
    r[n] = '\0';
    f_str(out, r, n);
    (void)c;
    return R_OK;
}

static int f_replicate(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 2, "REPLICATE") != 0) {
        return err->code;
    }
    const char* s;
    size_t n;
    double k;
    if (!rv_to_str(&argv[0], &s, &n) || !rv_to_num(&argv[1], &k) || k < 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "REPLICATE() type mismatch");
        return err->code;
    }
    size_t reps = (size_t)k;
    if (n && reps > 65500 / n) {
        err->code = R_ERR_STR_OVERFLOW;
        snprintf(err->msg, sizeof(err->msg), "REPLICATE() result too large");
        return err->code;
    }
    size_t total = n * reps;
    char* r = malloc(total + 1);
    if (!r) {
        RX_FATAL("out of memory");
    }
    for (size_t i = 0; i < reps; i++) {
        memcpy(r + i * n, s, n);
    }
    r[total] = '\0';
    f_str(out, r, total);
    (void)c;
    return R_OK;
}

static int pad_impl(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err, int mode) {
    const char* nm = mode == 0 ? "PADL" : mode == 1 ? "PADR" : "PADC";
    if (argc < 2 || argc > 3) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() expects 2 or 3 arguments", nm);
        return err->code;
    }
    double wd;
    if (!rv_to_num(&argv[1], &wd)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() expects numeric width", nm);
        return err->code;
    }
    char fill = ' ';
    if (argc == 3) {
        const char* fs;
        size_t fn;
        if (!rv_to_str(&argv[2], &fs, &fn) || fn == 0) {
            err->code = R_ERR_TYPE;
            snprintf(err->msg, sizeof(err->msg), "%s() pad character must be character", nm);
            return err->code;
        }
        fill = fs[0];
    }
    char tmp[320];
    const char* s;
    size_t n;
    double d;
    if (rv_to_str(&argv[0], &s, &n)) {
        /* character expression: copy out of the value buffer */
        static _Thread_local char cbuf[320];
        if (n >= sizeof(cbuf)) {
            n = sizeof(cbuf) - 1;
        }
        memcpy(cbuf, s, n);
        cbuf[n] = '\0';
        s = cbuf;
    } else if (rv_to_num(&argv[0], &d)) {
        rx_format_num(tmp, sizeof(tmp), d, c->set.decimals, c->set.fixed);
        s = tmp;
        n = strlen(tmp);
    } else {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() bad value type", nm);
        return err->code;
    }
    size_t w = (size_t)(wd < 0 ? 0 : wd);
    if (w > 65500) {
        err->code = R_ERR_STR_OVERFLOW;
        snprintf(err->msg, sizeof(err->msg), "%s() width too large", nm);
        return err->code;
    }
    char* r = malloc(w + 1);
    if (!r) {
        RX_FATAL("out of memory");
    }
    if (w <= n) {
        memcpy(r, s, w);
    } else if (mode == 0) { /* PADL: pad left */
        memset(r, fill, w - n);
        memcpy(r + (w - n), s, n);
    } else if (mode == 1) { /* PADR */
        memcpy(r, s, n);
        memset(r + n, fill, w - n);
    } else { /* PADC: center */
        size_t left = (w - n) / 2;
        memset(r, fill, left);
        memcpy(r + left, s, n);
        memset(r + left + n, fill, w - n - left);
    }
    r[w] = '\0';
    f_str(out, r, w);
    return R_OK;
}

static int f_padl(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return pad_impl(c, a, v, o, e, 0);
}

static int f_padr(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return pad_impl(c, a, v, o, e, 1);
}

static int f_padc(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return pad_impl(c, a, v, o, e, 2);
}

static int f_strfn(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 1 || argc > 3) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "STR() expects 1..3 arguments");
        return err->code;
    }
    double d;
    if (!rv_to_num(&argv[0], &d)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "STR() expects numeric");
        return err->code;
    }
    int len = 10, dec = c->set.decimals;
    if (argc >= 2) {
        double l;
        if (!rv_to_num(&argv[1], &l)) {
            err->code = R_ERR_TYPE;
            return err->code;
        }
        len = (int)l;
    }
    if (argc == 3) {
        double dd;
        if (!rv_to_num(&argv[2], &dd)) {
            err->code = R_ERR_TYPE;
            return err->code;
        }
        dec = (int)dd;
    }
    if (len < 0) {
        len = 0;
    }
    if (len > 254) {
        len = 254;
    }
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%*.*f", len, dec, d);
    f_str(out, rx_strdup(tmp), strlen(tmp));
    return R_OK;
}

static int f_val(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "VAL") != 0) {
        return err->code;
    }
    const char* s;
    size_t n;
    if (!rv_to_str(&argv[0], &s, &n)) {
        f_num(c, out, 0);
        return R_OK;
    }
    int blank;
    double d = rx_parse_num(s, n, &blank);
    (void)blank;
    f_num(c, out, d);
    return R_OK;
}

static int f_strtran(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 3 || argc > 4) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "STRTRAN() expects 3 or 4 arguments");
        return err->code;
    }
    const char *src, *seek, *repl;
    size_t ns, nk, nr;
    if (!rv_to_str(&argv[0], &src, &ns) || !rv_to_str(&argv[1], &seek, &nk) ||
        !rv_to_str(&argv[2], &repl, &nr)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "STRTRAN() expects character arguments");
        return err->code;
    }
    strbuf sb;
    sb_init(&sb);
    size_t i = 0;
    while (i < ns) {
        if (nk && i + nk <= ns && memcmp(src + i, seek, nk) == 0) {
            sb_put(&sb, repl, nr);
            i += nk;
        } else {
            sb_putc(&sb, src[i]);
            i++;
        }
    }
    if (!sb.buf) {
        sb_putc(&sb, '\0');
        sb.len = 0;
    }
    f_str(out, sb.buf, sb.len);
    (void)c;
    return R_OK;
}

static int f_stuff(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 4, "STUFF") != 0) {
        return err->code;
    }
    const char *src, *repl;
    size_t ns, nr;
    double start, del;
    if (!rv_to_str(&argv[0], &src, &ns) || !rv_to_num(&argv[1], &start) ||
        !rv_to_num(&argv[2], &del) || !rv_to_str(&argv[3], &repl, &nr)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "STUFF() type mismatch");
        return err->code;
    }
    if (start < 1) {
        start = 1;
    }
    size_t off = (size_t)start - 1;
    if (off > ns) {
        off = ns;
    }
    size_t d = del < 0 ? 0 : (size_t)del;
    if (d > ns - off) {
        d = ns - off;
    }
    strbuf sb;
    sb_init(&sb);
    sb_put(&sb, src, off);
    sb_put(&sb, repl, nr);
    sb_put(&sb, src + off + d, ns - off - d);
    if (!sb.buf) {
        sb_putc(&sb, '\0');
        sb.len = 0;
    }
    f_str(out, sb.buf, sb.len);
    (void)c;
    return R_OK;
}

static int f_like(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 2, "LIKE") != 0) {
        return err->code;
    }
    const char *pat, *s;
    size_t np, ns;
    if (!rv_to_str(&argv[0], &pat, &np) || !rv_to_str(&argv[1], &s, &ns)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "LIKE() expects character arguments");
        return err->code;
    }
    /* wildcard match: * any run, ? any char */
    size_t pi = 0, si = 0, star = (size_t)-1, mark = 0;
    while (si < ns) {
        if (pi < np && (pat[pi] == '?' || pat[pi] == s[si])) {
            pi++;
            si++;
        } else if (pi < np && pat[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (star != (size_t)-1) {
            pi = star + 1;
            si = ++mark;
        } else {
            f_logic(out, L_F);
            (void)c;
            return R_OK;
        }
    }
    while (pi < np && pat[pi] == '*') {
        pi++;
    }
    f_logic(out, pi == np ? L_T : L_F);
    (void)c;
    return R_OK;
}

static int f_inlist(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (argc < 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "INLIST() expects at least 2 arguments");
        return err->code;
    }
    for (size_t i = 1; i < argc; i++) {
        if (argv[0].t == argv[i].t) {
            if (argv[0].t == RX_T_C && argv[i].t == RX_T_C) {
                if (rstr_cmp(&argv[0].u.c, &argv[i].u.c) == 0) {
                    f_logic(out, L_T);
                    (void)c;
                    return R_OK;
                }
                continue;
            }
            double a, b;
            if (rv_to_num(&argv[0], &a) && rv_to_num(&argv[i], &b) && a == b) {
                f_logic(out, L_T);
                (void)c;
                return R_OK;
            }
            if (argv[0].t == RX_T_L && argv[0].u.l == argv[i].u.l) {
                f_logic(out, L_T);
                (void)c;
                return R_OK;
            }
        }
    }
    f_logic(out, L_F);
    (void)c;
    return R_OK;
}

static int is_x(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err, int which) {
    const char* nm = which == 0   ? "ISALPHA"
                     : which == 1 ? "ISLOWER"
                     : which == 2 ? "ISUPPER"
                                  : "ISDIGIT";
    if (f_argc(err, argc, 1, nm) != 0) {
        return err->code;
    }
    const char* s;
    size_t n;
    if (!rv_to_str(&argv[0], &s, &n) || n == 0) {
        f_logic(out, L_F);
        (void)c;
        return R_OK;
    }
    int ok = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        int good = which == 0   ? isalpha(ch)
                   : which == 1 ? islower(ch)
                   : which == 2 ? isupper(ch)
                                : isdigit(ch);
        if (!good) {
            ok = 0;
            break;
        }
    }
    f_logic(out, ok ? L_T : L_F);
    (void)c;
    return R_OK;
}

static int f_isalpha(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return is_x(c, a, v, o, e, 0);
}

static int f_islower(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return is_x(c, a, v, o, e, 1);
}

static int f_isupper(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return is_x(c, a, v, o, e, 2);
}

static int f_isdigit(rx_ctx* c, size_t a, const rval* v, rval* o, r_err* e) {
    return is_x(c, a, v, o, e, 3);
}

static int f_proper(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "PROPER") != 0) {
        return err->code;
    }
    const char* s;
    size_t n;
    if (!rv_to_str(&argv[0], &s, &n)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "PROPER() expects character");
        return err->code;
    }
    char* r = dup_range(s, n, 0, n);
    int newword = 1;
    for (size_t i = 0; i < n; i++) {
        if (isalpha((unsigned char)r[i])) {
            r[i] = (char)(newword ? toupper((unsigned char)r[i]) : tolower((unsigned char)r[i]));
            newword = 0;
        } else {
            newword = 1;
        }
    }
    f_str(out, r, n);
    (void)c;
    return R_OK;
}

static int f_empty(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "EMPTY") != 0) {
        return err->code;
    }
    const rval* v = &argv[0];
    rx_logic res = L_F;
    switch (v->t) {
    case RX_T_C:
        res = v->u.c.n == 0 ? L_T : L_F;
        break;
    case RX_T_N:
    case RX_T_B: {
        double d;
        rv_to_num(v, &d);
        res = d == 0 ? L_T : L_F;
        break;
    }
    case RX_T_I:
        res = v->u.i == 0 ? L_T : L_F;
        break;
    case RX_T_Y:
        res = v->u.y == 0 ? L_T : L_F;
        break;
    case RX_T_D:
        res = (v->u.d.y == 0 && v->u.d.m == 0 && v->u.d.d == 0) ? L_T : L_F;
        break;
    case RX_T_L:
        res = (v->u.l == L_F || v->u.l == L_UNDEF) ? L_T : L_F;
        break;
    case RX_T_M:
        res = (!v->u.memo.loaded || v->u.memo.loaded->n == 0) ? L_T : L_F;
        break;
    default:
        res = L_T;
        break;
    }
    f_logic(out, res);
    (void)c;
    return R_OK;
}

static int f_isblank(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "ISBLANK") != 0) {
        return err->code;
    }
    const rval* v = &argv[0];
    rx_logic res = L_F;
    if (v->t == RX_T_C) {
        int blank = 1;
        for (size_t i = 0; i < v->u.c.n; i++) {
            if (!isspace((unsigned char)v->u.c.utf8[i])) {
                blank = 0;
                break;
            }
        }
        res = blank ? L_T : L_F;
    } else if (v->t == RX_T_D) {
        res = (v->u.d.y == 0) ? L_T : L_F;
    } else if (v->t == RX_T_L) {
        res = v->u.l == L_UNDEF ? L_T : L_F;
    }
    f_logic(out, res);
    (void)c;
    return R_OK;
}

static int f_valtype(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 1, "VALTYPE") != 0) {
        return err->code;
    }
    char t = 'U';
    switch (argv[0].t) {
    case RX_T_C:
        t = 'C';
        break;
    case RX_T_N:
        t = 'N';
        break;
    case RX_T_I:
        t = 'I';
        break;
    case RX_T_Y:
        t = 'Y';
        break;
    case RX_T_B:
        t = 'B';
        break;
    case RX_T_D:
        t = 'D';
        break;
    case RX_T_T:
        t = 'T';
        break;
    case RX_T_L:
        t = 'L';
        break;
    case RX_T_M:
        t = 'M';
        break;
    case RX_T_G:
        t = 'G';
        break;
    default:
        t = 'U';
        break;
    }
    char r[2] = {t, '\0'};
    f_str(out, rx_strdup(r), 1);
    (void)c;
    return R_OK;
}

static int f_type(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    /* M5 §7.6: TYPE("name") resolves the named memvar/field; "U" when the
       name does not exist. A non-character argument falls back to VALTYPE. */
    if (f_argc(err, argc, 1, "TYPE") != 0) {
        return err->code;
    }
    if (argv[0].t == RX_T_C && argv[0].u.c.utf8) {
        char t = 'U';
        rx_memvar* mv = rx_memvar_lookup(c, argv[0].u.c.utf8);
        if (mv) {
            if (mv->is_array) {
                t = 'A';
            } else {
                switch (mv->val.t) {
                case RX_T_C:
                    t = 'C';
                    break;
                case RX_T_N:
                case RX_T_I:
                case RX_T_Y:
                case RX_T_B:
                    t = 'N';
                    break;
                case RX_T_D:
                    t = 'D';
                    break;
                case RX_T_T:
                    t = 'T';
                    break;
                case RX_T_L:
                    t = 'L';
                    break;
                case RX_T_M:
                    t = 'M';
                    break;
                case RX_T_G:
                    t = 'G';
                    break;
                default:
                    t = 'U';
                    break;
                }
            }
        } else {
            rx_dbf* dbf;
            if (rx_wa_dbf(0, &dbf) == R_OK) {
                size_t idx;
                if (rx_dbf_field_index(dbf, argv[0].u.c.utf8, &idx) == R_OK) {
                    rx_fielddef fd;
                    if (rx_dbf_field_def(dbf, idx, &fd) == R_OK) {
                        switch (fd.type) {
                        case RFT_C:
                            t = 'C';
                            break;
                        case RFT_N:
                        case RFT_F:
                            t = 'N';
                            break;
                        case RFT_Y:
                            t = 'Y';
                            break;
                        case RFT_D:
                            t = 'D';
                            break;
                        case RFT_T:
                            t = 'T';
                            break;
                        case RFT_L:
                            t = 'L';
                            break;
                        case RFT_M:
                            t = 'M';
                            break;
                        case RFT_G:
                            t = 'G';
                            break;
                        case RFT_B:
                            t = 'B';
                            break;
                        default:
                            t = 'U';
                            break;
                        }
                    }
                }
            }
        }
        char r[2] = {t, '\0'};
        f_str(out, rx_strdup(r), 1);
        return R_OK;
    }
    return f_valtype(c, argc, argv, out, err);
}

static int f_isleaddbyte(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    /* UTF-8 has no lead bytes: documented deviation, always .F. */
    if (f_argc(err, argc, 1, "ISLEADBYTE") != 0) {
        return err->code;
    }
    f_logic(out, L_F);
    (void)c;
    (void)argv;
    return R_OK;
}

static int f_transform(rx_ctx* c, size_t argc, const rval* argv, rval* out, r_err* err) {
    if (f_argc(err, argc, 2, "TRANSFORM") != 0) {
        return err->code;
    }
    const char* pic;
    size_t np;
    if (!rv_to_str(&argv[1], &pic, &np)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "TRANSFORM() picture must be character");
        return err->code;
    }
    /* M7: full PICTURE/FUNCTION engine (spec §9.2 codes) */
    if (pic[0]) {
        strbuf sb;
        sb_init(&sb);
        rx_picture_render(c, &argv[0], pic, NULL, &sb);
        f_str(out, sb.buf ? sb.buf : rx_strdup(""), sb.len);
        sb.buf = NULL;
        sb_free(&sb);
        return R_OK;
    }
    if (argv[0].t == RX_T_C && np >= 2 && pic[0] == '@' && pic[1] == '!') {
        const char* s;
        size_t n;
        rv_to_str(&argv[0], &s, &n);
        char* r = dup_range(s, n, 0, n);
        for (size_t i = 0; i < n; i++) {
            r[i] = (char)toupper((unsigned char)r[i]);
        }
        f_str(out, r, n);
        return R_OK;
    }
    if (argv[0].t == RX_T_N || argv[0].t == RX_T_I || argv[0].t == RX_T_B) {
        double d;
        rv_to_num(&argv[0], &d);
        char tmp[320];
        snprintf(tmp, sizeof(tmp), "%.*f", c->set.decimals, d);
        f_str(out, rx_strdup(tmp), strlen(tmp));
        return R_OK;
    }
    const char* s;
    size_t n;
    if (rv_to_str(&argv[0], &s, &n)) {
        f_str(out, dup_range(s, n, 0, n), n);
        return R_OK;
    }
    f_str(out, rx_strdup(""), 0);
    return R_OK;
}

const rx_funcdef rx_funcs_str[] = {
    {"LEN", f_len},
    {"UPPER", f_upper},
    {"LOWER", f_lower},
    {"TRIM", f_trim},
    {"LTRIM", f_ltrim},
    {"RTRIM", f_rtrim},
    {"ALLTRIM", f_alltrim},
    {"LEFT", f_left},
    {"RIGHT", f_right},
    {"SUBSTR", f_substr},
    {"AT", f_at},
    {"ATC", f_atc},
    {"RAT", f_rat},
    {"OCCURS", f_occurs},
    {"CHR", f_chr},
    {"ASC", f_asc},
    {"SPACE", f_space},
    {"REPLICATE", f_replicate},
    {"PADL", f_padl},
    {"PADR", f_padr},
    {"PADC", f_padc},
    {"STR", f_strfn},
    {"VAL", f_val},
    {"STRTRAN", f_strtran},
    {"STUFF", f_stuff},
    {"LIKE", f_like},
    {"INLIST", f_inlist},
    {"ISALPHA", f_isalpha},
    {"ISLOWER", f_islower},
    {"ISUPPER", f_isupper},
    {"ISDIGIT", f_isdigit},
    {"PROPER", f_proper},
    {"EMPTY", f_empty},
    {"ISBLANK", f_isblank},
    {"VALTYPE", f_valtype},
    {"TYPE", f_type},
    {"ISLEADBYTE", f_isleaddbyte},
    {"TRANSFORM", f_transform},
};

const size_t rx_funcs_str_n = sizeof(rx_funcs_str) / sizeof(rx_funcs_str[0]);
