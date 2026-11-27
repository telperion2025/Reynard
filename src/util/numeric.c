/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file numeric.c
 * @brief FoxPro numeric parsing and display formatting.
 *
 * Display rules (FoxPro 2.6a): with SET FIXED OFF (default), an
 * integer-valued number prints without a decimal point; other numbers
 * print with SET DECIMALS digits (default 2). With SET FIXED ON every
 * number prints with SET DECIMALS digits.
 */
#include "util/util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rx_format_num(char* out, size_t outsz, double d, int dec, int fixed) {
    if (isnan(d)) {
        snprintf(out, outsz, "NaN");
        return;
    }
    if (isinf(d)) {
        snprintf(out, outsz, d > 0 ? "Inf" : "-Inf");
        return;
    }
    if (!fixed && d == floor(d) && fabs(d) < 1e15) {
        snprintf(out, outsz, "%.0f", d);
        return;
    }
    if (dec < 0) {
        dec = 0;
    }
    if (dec > 15) {
        dec = 15;
    }
    snprintf(out, outsz, "%.*f", dec, d);
}

double rx_parse_num(const char* s, size_t n, int* blank) {
    size_t i = 0;
    int any = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    for (size_t j = i; j < n; j++) {
        if (s[j] != ' ' && s[j] != '\t') {
            any = 1;
            break;
        }
    }
    if (blank) {
        *blank = !any;
    }
    if (!any) {
        return 0.0;
    }
    char tmp[64];
    size_t m = n - i;
    if (m >= sizeof(tmp)) {
        m = sizeof(tmp) - 1;
    }
    memcpy(tmp, s + i, m);
    tmp[m] = '\0';
    return strtod(tmp, NULL);
}
