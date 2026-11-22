/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file strbuf.c
 * @brief Growable byte buffer used across util, lang and data.
 */
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/utf8.h"

void sb_init(strbuf *sb)
{
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(strbuf *sb)
{
    free(sb->buf);
    sb_init(sb);
}

static int sb_reserve(strbuf *sb, size_t extra)
{
    if (sb->len + extra + 1 <= sb->cap) {
        return R_OK;
    }
    size_t ncap = sb->cap ? sb->cap : 64;
    while (ncap < sb->len + extra + 1) {
        ncap *= 2;
    }
    char *nbuf = realloc(sb->buf, ncap);
    if (!nbuf) {
        RX_FATAL("out of memory");
    }
    sb->buf = nbuf;
    sb->cap = ncap;
    return R_OK;
}

int sb_putc(strbuf *sb, char c)
{
    sb_reserve(sb, 1);
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
    return R_OK;
}

int sb_put(strbuf *sb, const char *s, size_t n)
{
    sb_reserve(sb, n);
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return R_OK;
}

int sb_puts(strbuf *sb, const char *s)
{
    return sb_put(sb, s, strlen(s));
}

int sb_printf(strbuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return R_ERR_IO_WRITE;
    }
    if ((size_t)n < sizeof(tmp)) {
        return sb_put(sb, tmp, (size_t)n);
    }
    sb_reserve(sb, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
    return R_OK;
}

int sb_putcp(strbuf *sb, uint32_t cp)
{
    char tmp[4];
    size_t n = utf8_encode(cp, tmp);
    return sb_put(sb, tmp, n);
}
