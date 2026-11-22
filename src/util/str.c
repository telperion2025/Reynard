/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file str.c
 * @brief r_str helpers and small C-string utilities.
 */
#include "util/util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int rstr_dup(r_str *dst, const char *utf8, size_t n)
{
    char *copy = malloc(n + 1);
    if (!copy) {
        RX_FATAL("out of memory");
    }
    if (n) {
        memcpy(copy, utf8, n);
    }
    copy[n] = '\0';
    dst->n = n;
    dst->utf8 = copy;
    return R_OK;
}

int rstr_cmp(const r_str *a, const r_str *b)
{
    size_t n = a->n < b->n ? a->n : b->n;
    int c = memcmp(a->utf8, b->utf8, n);
    if (c != 0) {
        return c;
    }
    if (a->n < b->n) {
        return -1;
    }
    if (a->n > b->n) {
        return 1;
    }
    return 0;
}

void rval_set_str(rval *v, char *utf8, size_t n)
{
    rx_rval_free(v);
    v->t = RX_T_C;
    v->u.c.utf8 = utf8;
    v->u.c.n = n;
}

int rx_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

void rx_strtoupper(char *s)
{
    for (; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
}

char *rx_strdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s);
    char *copy = malloc(n + 1);
    if (!copy) {
        RX_FATAL("out of memory");
    }
    memcpy(copy, s, n + 1);
    return copy;
}
