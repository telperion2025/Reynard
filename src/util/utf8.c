/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file utf8.c
 * @brief Minimal UTF-8 codec. Decode is strict; invalid bytes decode as
 *        U+FFFD consuming one byte so corrupt input never loops forever.
 */
#include "util/util.h"

size_t utf8_decode(const char* s, size_t n, uint32_t* cp) {
    if (n == 0) {
        *cp = 0;
        return 0;
    }
    uint8_t c = (uint8_t)s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF && n >= 2) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF && n >= 3) {
        *cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)((uint8_t)s[1] & 0x3F) << 6) |
              ((uint8_t)s[2] & 0x3F);
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4 && n >= 4) {
        *cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)((uint8_t)s[1] & 0x3F) << 12) |
              ((uint32_t)((uint8_t)s[2] & 0x3F) << 6) | ((uint8_t)s[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

size_t utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t utf8_length(const char* s, size_t n) {
    size_t count = 0;
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        size_t used = utf8_decode(s + i, n - i, &cp);
        if (used == 0) {
            break;
        }
        i += used;
        count++;
    }
    return count;
}

int utf8_cp_width(uint32_t cp) {
    /* Combining diacriticals and zero-width joiners are width 0. */
    if (cp >= 0x0300 && cp <= 0x036F) {
        return 0;
    }
    if (cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F)) {
        return 0;
    }
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
        return 0;
    }
    return 1;
}
