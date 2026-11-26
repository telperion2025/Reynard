/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file picture.c
 * @brief PICTURE / FUNCTION code engine (spec §9.2, M7).
 *
 * Implements the FoxPro 2.6 template codes required by the spec:
 *   A X 9 # $ * . , L Y N R ! I B
 * (template chars) plus the matching FUNCTION letters (@A @X @9 @# @$ @*
 * @. @, @L @Y @! @B @I @R @C @X @D). Used by TRANSFORM(), by @ SAY/GET
 * rendering, and to validate GET input. Where FoxPro is ambiguous we follow
 * the FoxPro 2.6a manual.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lang/lang.h"
#include "lang/funcs.h"
#include "util/util.h"

static int is_func_letter(char c)
{
    return strchr("AX9#$*.,LYNR!IBCDE", c) != NULL;
}

/* character class test for template char c against input char ch */
static int tmpl_class_ok(char c, char ch)
{
    switch (c) {
    case 'X':
        return 1;
    case 'A':
        return isalpha((unsigned char)ch) != 0;
    case '9':
        return isdigit((unsigned char)ch) || ch == ' ' || ch == '+' || ch == '-';
    case '#':
        return isdigit((unsigned char)ch) || ch == ' ' || ch == '+' || ch == '-';
    case 'L':
        return ch == 'T' || ch == 'F' || ch == 'Y' || ch == 'N' || ch == 't' || ch == 'f' ||
               ch == 'y' || ch == 'n';
    case 'Y':
        return ch == 'Y' || ch == 'N' || ch == 'y' || ch == 'n';
    case '!':
        return isalpha((unsigned char)ch);
    case '$':
    case '*':
    case '.':
    case ',':
        return isdigit((unsigned char)ch);
    default:
        return ch == c; /* literal */
    }
}

/** Validate typed input against a PICTURE template (FUNCTION letters in
 *  pic after '@' are honored for case classes). Returns 1 when valid. */
int rx_picture_input_ok(const char *pic, const char *func, const char *input)
{
    if (!pic) {
        return 1;
    }
    int upper = 0;
    const char *t = pic;
    if (t[0] == '@') {
        for (const char *f = t + 1; *f; f++) {
            if (*f == '!') {
                upper = 1;
            }
            if (!is_func_letter(*f)) {
                break; /* template begins */
            }
        }
        if (t[0] == '@') {
            t++;
            while (*t && is_func_letter(*t)) {
                t++;
            }
        }
    }
    if (func) {
        for (const char *f = func; *f; f++) {
            if (*f == '!') {
                upper = 1;
            }
        }
    }
    size_t ti = 0;
    for (size_t i = 0; input[i]; i++) {
        char ch = input[i];
        if (upper) {
            ch = (char)toupper((unsigned char)ch);
        }
        char c = t[ti];
        if (c == '\0') {
            return 0; /* input longer than template (unless template ends in ...) */
        }
        if (c == 'R' || c == 'r') {
            continue; /* R: non-template chars are inserted, not validated */
        }
        if (!tmpl_class_ok(c, ch)) {
            return 0;
        }
        ti++;
    }
    /* trailing template may be padding classes (9 # $ * space) */
    for (; t[ti]; ti++) {
        char c = t[ti];
        if (c == '9' || c == '#' || c == '$' || c == '*' || c == ' ' || c == 'R') {
            continue;
        }
        return 0;
    }
    return 1;
}

#define PICSENT '\x01' /* sentinel for FoxPro space-fill emitted by '9' positions */

/** Render a value through a PICTURE template (and FUNCTION codes). */
int rx_picture_render(rx_ctx *ctx, const rval *v, const char *pic, const char *func, strbuf *out)
{
    char raw[512];
    size_t rn = 0;
    int upper = 0, center = 0, left = 0, nontemplate = 0, date_us = 0, date_eu = 0;
    /* FUNCTION codes may also be embedded as "@X..." in pic */
    const char *t = pic ? pic : "";
    if (t[0] == '@') {
        for (const char *f = t + 1; *f && is_func_letter(*f); f++) {
            switch (*f) {
            case '!':
                upper = 1;
                break;
            case 'I':
                center = 1;
                break;
            case 'B':
                left = 1;
                break;
            case 'R':
                nontemplate = 1;
                break;
            case 'D':
                date_us = 1;
                break;
            case 'E':
                date_eu = 1;
                break;
            default:
                break;
            }
        }
        t++;
        while (*t && is_func_letter(*t)) {
            t++;
        }
    }
    if (func) {
        for (const char *f = func; *f; f++) {
            switch (*f) {
            case '!':
                upper = 1;
                break;
            case 'I':
                center = 1;
                break;
            case 'B':
                left = 1;
                break;
            case 'R':
                nontemplate = 1;
                break;
            case 'D':
                date_us = 1;
                break;
            case 'E':
                date_eu = 1;
                break;
            default:
                break;
            }
        }
    }
    /* raw text of the value */
    switch (v->t) {
    case RX_T_N:
    case RX_T_I:
    case RX_T_B: {
        double d;
        rv_to_num(v, &d);
        snprintf(raw, sizeof(raw), "%.*f", ctx->set.decimals, d);
        rn = strlen(raw);
        break;
    }
    case RX_T_L:
        raw[0] = v->u.l == L_T ? 'T' : v->u.l == L_F ? 'F' : '?';
        raw[1] = '\0';
        rn = 1;
        break;
    case RX_T_D:
        if (v->u.d.y) {
            if (date_eu) {
                snprintf(raw, sizeof(raw), "%02d/%02d/%02d", v->u.d.d, v->u.d.m, v->u.d.y % 100);
            } else if (date_us) {
                snprintf(raw, sizeof(raw), "%02d/%02d/%02d", v->u.d.m, v->u.d.d, v->u.d.y % 100);
            } else {
                snprintf(raw, sizeof(raw), "%04d-%02d-%02d", v->u.d.y, v->u.d.m, v->u.d.d);
            }
        } else {
            raw[0] = '\0';
        }
        rn = strlen(raw);
        break;
    default: {
        const char *s;
        size_t n;
        if (rv_to_str(v, &s, &n) && n < sizeof(raw)) {
            memcpy(raw, s, n);
            raw[n] = '\0';
            rn = n;
        } else {
            raw[0] = '\0';
            rn = 0;
        }
        break;
    }
    }
    if (upper) {
        for (size_t i = 0; i < rn; i++) {
            raw[i] = (char)toupper((unsigned char)raw[i]);
        }
    }
    if (!t[0]) {
        /* no template: plain value */
        sb_put(out, raw, rn);
        return R_OK;
    }
    int numeric = v->t == RX_T_N || v->t == RX_T_I || v->t == RX_T_B;
    char res[600];
    size_t rl = 0;
    size_t vi = 0;
    if (numeric) {
        /* right-to-left digit mapping: split value into int/frac digit runs,
           walk the template from the right edge */
        double d;
        rv_to_num(v, &d);
        int neg = d < 0;
        double a = neg ? -d : d;
        /* count template fraction digits (after first '.') */
        int tdot = -1;
        for (size_t i = 0; t[i]; i++) {
            if (t[i] == '.') {
                tdot = (int)i;
                break;
            }
        }
        int nfrac = 0;
        if (tdot >= 0) {
            for (size_t i = (size_t)tdot + 1; t[i]; i++) {
                if (t[i] == '9' || t[i] == '#') {
                    nfrac++;
                }
            }
        }
        char fbuf[64];
        snprintf(fbuf, sizeof(fbuf), "%.*f", nfrac > 0 ? nfrac : 0, a);
        /* split fbuf into intdigits/fracdigits */
        char intd[64], fracd[64];
        size_t ni = 0, nf = 0;
        int dotseen = 0;
        for (char *q = fbuf; *q; q++) {
            if (*q == '.') {
                dotseen = 1;
                continue;
            }
            if (!isdigit((unsigned char)*q)) {
                continue;
            }
            if (dotseen) {
                if (nf < sizeof(fracd) - 1) {
                    fracd[nf++] = *q;
                }
            } else if (ni < sizeof(intd) - 1) {
                intd[ni++] = *q;
            }
        }
        /* pad frac to nfrac */
        while (nf < (size_t)nfrac && nf < sizeof(fracd) - 1) {
            fracd[nf++] = '0';
        }
        intd[ni] = '\0';
        fracd[nf] = '\0';
        if (ni == 0) {
            intd[ni++] = '0';
            intd[ni] = '\0';
        }
        /* drop leading zeros of the integer part (keep one digit): a value
           below 1 has int "0", which must not look like template overflow */
        while (ni > 1 && intd[0] == '0') {
            memmove(intd, intd + 1, ni);
            ni--;
            intd[ni] = '\0';
        }
        if (neg && ni < sizeof(intd) - 1) {
            /* the sign consumes a template slot, glued to the digits */
            memmove(intd + 1, intd, ni + 1);
            intd[0] = '-';
            ni++;
        }
        size_t ip = ni, fp = nf; /* next digit from the right */
        int in_frac = tdot >= 0;
        int has_dollar = strchr(t, '$') != NULL;
        int dollar_emitted = 0;
        char out_r[600];
        size_t orl = 0;
        for (size_t ti = strlen(t); ti-- > 0 && orl < sizeof(out_r) - 1;) {
            char c = t[ti];
            if (c == 'R' || c == 'r') {
                continue;
            }
            if (c == '.') {
                out_r[orl++] = '.';
                in_frac = 0;
                continue;
            }
            if (c == ',') {
                if (!in_frac && ip > 0) {
                    out_r[orl++] = ',';
                } else {
                    out_r[orl++] = PICSENT; /* suppressed comma keeps its slot */
                }
                continue;
            }
            if (c == '$' || c == '*') {
                /* $ floats: one $ before the first digit; * check-fills with
                   asterisks left of the digits. Both still consume digits. */
                char ch = in_frac ? (fp > 0 ? fracd[--fp] : ' ') : (ip > 0 ? intd[--ip] : ' ');
                if (isdigit((unsigned char)ch)) {
                    out_r[orl++] = ch;
                    if (has_dollar && !dollar_emitted && !in_frac && ip == 0) {
                        out_r[orl++] = '$'; /* after the last int digit in emission order */
                        dollar_emitted = 1;
                    }
                } else if (c == '*') {
                    out_r[orl++] = '*';
                }
                continue;
            }
            if (c == '9' || c == '#') {
                char ch;
                if (in_frac) {
                    ch = fp > 0 ? fracd[--fp] : ' ';
                } else {
                    ch = ip > 0 ? intd[--ip] : ' ';
                }
                if (isdigit((unsigned char)ch) || ch == '-') {
                    out_r[orl++] = ch;
                    if (has_dollar && !dollar_emitted && !in_frac && ip == 0 && ch != '-') {
                        out_r[orl++] = '$';
                        dollar_emitted = 1;
                    }
                } else if (c == '9') {
                    /* FoxPro space-fills missing '9' digits; the sentinel keeps
                       them through the leading-space strip below */
                    out_r[orl++] = PICSENT;
                } /* '#' drops leading blanks */
                continue;
            }
            /* literal */
            out_r[orl++] = c;
        }
        /* value wider than the template: FoxPro fills digit positions with '*'.
           A remaining int digit of "0" is not significant (value < 1, template
           has no integer positions). */
        int int_sig = 0;
        for (size_t i = 0; i < ip; i++) {
            if (intd[i] != '0') {
                int_sig = 1;
            }
        }
        if (ip > 0 && int_sig) {
            /* overflow: the whole field fills with asterisks (FoxPro) */
            orl = 0;
            for (size_t ti = 0; t[ti] && orl < sizeof(out_r) - 1; ti++) {
                out_r[orl++] = '*';
            }
            out_r[orl] = '\0';
            sb_put(out, out_r, orl);
            return R_OK;
        }
        /* reverse */
        for (size_t i = 0; i < orl / 2; i++) {
            char tmp = out_r[i];
            out_r[i] = out_r[orl - 1 - i];
            out_r[orl - 1 - i] = tmp;
        }
        out_r[orl] = '\0';
        /* strip leading real spaces (literal/'$' fill); '9' sentinel spaces are
           kept (FoxPro space-pads). Glue a leading '-' to the digits. */
        char *p = out_r;
        while (*p == ' ') {
            p++;
        }
        for (char *q = p; *q; q++) {
            if (*q == PICSENT) {
                *q = ' ';
            }
        }
        if (left) {
            /* @B: left-justify, pad right to the template width */
            size_t tw = 0;
            for (const char *q = t; *q; q++) {
                if (*q != 'R') {
                    tw++;
                }
            }
            size_t pl = strlen(p);
            while (pl < tw && pl < sizeof(out_r) - 1) {
                p[pl++] = ' ';
            }
            p[pl] = '\0';
        }
        if (center) {
            size_t tw = strlen(t);
            size_t pl = strlen(p);
            if (tw > pl) {
                size_t pad = (tw - pl) / 2;
                for (size_t i = 0; i < pad; i++) {
                    sb_putc(out, ' ');
                }
            }
        }
        sb_put(out, p, strlen(p));
        return R_OK;
    }
    /* character/logical/date template walk (left to right) */
    for (size_t ti = 0; t[ti] && rl < sizeof(res) - 1; ti++) {
        char c = t[ti];
        if (c == 'R' || c == 'r') {
            continue;
        }
        if (c == '.') {
            res[rl++] = '.';
            continue;
        }
        if (c == ',') {
            res[rl++] = ',';
            continue;
        }
        if (c == '9' || c == '#' || c == '$' || c == '*') {
            res[rl++] = vi < rn ? raw[vi++] : ' ';
            continue;
        }
        if (c == '!') {
            char ch = vi < rn ? raw[vi++] : ' ';
            res[rl++] = (char)toupper((unsigned char)ch);
            continue;
        }
        if (c == 'A' || c == 'X' || c == 'L' || c == 'Y') {
            res[rl++] = vi < rn ? raw[vi++] : ' ';
            continue;
        }
        res[rl++] = c;
        if (!nontemplate && vi < rn) {
            vi++;
        }
    }
    res[rl] = '\0';
    if (center && rl < 40) {
        /* @I centers within the template width */
        size_t tw = 0;
        for (const char *q = t; *q; q++) {
            if (*q != 'R') {
                tw++;
            }
        }
        if (tw > rl) {
            size_t pad = (tw - rl) / 2;
            memmove(res + pad, res, rl + 1);
            memset(res, ' ', pad);
            rl += pad;
        }
    }
    (void)left;
    sb_put(out, res, rl);
    return R_OK;
}
