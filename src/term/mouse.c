/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file mouse.c
 * @brief Normalizes all mouse wire protocols into rmouse (spec §5.4).
 *
 * Decodes SGR 1006 (ESC [ < b ; x ; y M/m), legacy 1000 (ESC [ M b x y),
 * and urxvt 1015 (ESC [ b ; x ; y M). Returns 1 when a full event was
 * decoded, 0 when more bytes are needed, -1 on malformed input.
 */
#include "term/mouse.h"

#include <ctype.h>
#include <string.h>

static int parse_num(const uint8_t *buf, size_t n, size_t *pos, long *out)
{
    if (*pos >= n || !isdigit(buf[*pos])) {
        return -1;
    }
    long v = 0;
    while (*pos < n && isdigit(buf[*pos])) {
        v = v * 10 + (buf[*pos] - '0');
        if (v > 1000000) {
            return -1;
        }
        (*pos)++;
    }
    *out = v;
    return 0;
}

static void fill(rmouse *m, long btn, long x, long y, int release)
{
    memset(m, 0, sizeof(*m));
    long b = btn;
    if (b & 64) { /* wheel */
        m->kind = (b & 1) ? RM_WHEEL_DN : RM_WHEEL_UP;
        m->button = 0;
    } else if (b & 32) { /* motion */
        m->kind = (b & 3) ? RM_DRAG : RM_MOVE;
        m->button = (uint8_t)((b & 3) + 1);
    } else {
        m->kind = release ? RM_RELEASE : RM_PRESS;
        m->button = (uint8_t)((b & 3) + 1);
    }
    if (x > 0) {
        m->x = (uint16_t)(x - 1);
    }
    if (y > 0) {
        m->y = (uint16_t)(y - 1);
    }
}

int mouse_decode(const uint8_t *buf, size_t n, rmouse *out, size_t *used)
{
    if (n < 3 || buf[0] != 0x1B || buf[1] != '[') {
        return -1;
    }
    if (buf[2] == '<') { /* SGR 1006 or urxvt 1015 */
        size_t i = 3;
        long b, x, y;
        if (parse_num(buf, n, &i, &b) != 0) {
            return n < 12 ? 0 : -1; /* possibly incomplete */
        }
        if (i >= n || buf[i] != ';') {
            return 0;
        }
        i++;
        if (parse_num(buf, n, &i, &x) != 0) {
            return 0;
        }
        if (i >= n || buf[i] != ';') {
            return 0;
        }
        i++;
        if (parse_num(buf, n, &i, &y) != 0) {
            return 0;
        }
        if (i >= n || (buf[i] != 'M' && buf[i] != 'm')) {
            return n < 24 ? 0 : -1;
        }
        int release = buf[i] == 'm';
        i++;
        fill(out, b, x, y, release);
        *used = i;
        return 1;
    }
    if (buf[2] == 'M') { /* legacy 1000: ESC [ M b x y (bytes, +32) */
        if (n < 6) {
            return 0;
        }
        long b = buf[3] - 32;
        long x = buf[4] - 32;
        long y = buf[5] - 32;
        fill(out, b, x, y, 0);
        *used = 6;
        return 1;
    }
    return -1;
}
