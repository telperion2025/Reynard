/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file screen.c
 * @brief Virtual screen canvas (spec §5.2): rcell grid, 64-pair palette,
 *        damage-rect accumulation (merged; >30% handled as full paint by
 *        the backends), differential blit.
 */
#include "ui/ui_internal.h"

#include <stdlib.h>
#include <string.h>

#define SCR_MAX_DMG 512

void scr_init(rx_screen* s, int w, int h) {
    memset(s, 0, sizeof(*s));
    s->w = w;
    s->h = h;
    s->cells = calloc((size_t)w * h, sizeof(rcell));
    if (!s->cells) {
        RX_FATAL("out of memory");
    }
    s->capdmg = 64;
    s->dmg = calloc(s->capdmg, sizeof(struct rrect));
    if (!s->dmg) {
        RX_FATAL("out of memory");
    }
    for (int i = 0; i < 256; i++) {
        s->pal[i][0] = (uint8_t)(i % 16);
        s->pal[i][1] = (uint8_t)(i / 16);
    }
}

void scr_free(rx_screen* s) {
    free(s->cells);
    free(s->dmg);
    memset(s, 0, sizeof(*s));
}

static void mark_full(rx_screen* s) {
    s->ndmg = 1;
    s->dmg[0] = (struct rrect){0, 0, (uint16_t)s->w, (uint16_t)s->h};
}

static int rects_touch(const struct rrect* a, const struct rrect* b) {
    return a->x <= b->x + b->w + 1 && b->x <= a->x + a->w + 1 && a->y <= b->y + b->h + 1 &&
           b->y <= a->y + a->h + 1;
}

static void mark(rx_screen* s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }
    if (s->ndmg == 1 && s->dmg[0].x == 0 && s->dmg[0].y == 0 && s->dmg[0].w == s->w &&
        s->dmg[0].h == s->h) {
        return;
    }
    struct rrect r = {(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h};
    for (size_t i = 0; i < s->ndmg; i++) {
        if (rects_touch(&s->dmg[i], &r)) {
            int x0 = s->dmg[i].x < r.x ? s->dmg[i].x : r.x;
            int y0 = s->dmg[i].y < r.y ? s->dmg[i].y : r.y;
            int x1 = s->dmg[i].x + s->dmg[i].w > r.x + r.w ? s->dmg[i].x + s->dmg[i].w : r.x + r.w;
            int y1 = s->dmg[i].y + s->dmg[i].h > r.y + r.h ? s->dmg[i].y + s->dmg[i].h : r.y + r.h;
            s->dmg[i] = (struct rrect){(uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0),
                                       (uint16_t)(y1 - y0)};
            return;
        }
    }
    if (s->ndmg >= SCR_MAX_DMG || s->ndmg >= s->capdmg) {
        if (s->ndmg >= s->capdmg) {
            size_t nc = s->capdmg * 2;
            struct rrect* nd = realloc(s->dmg, nc * sizeof(struct rrect));
            if (!nd) {
                RX_FATAL("out of memory");
            }
            s->dmg = nd;
            s->capdmg = nc;
        }
        if (s->ndmg >= SCR_MAX_DMG) {
            mark_full(s);
            return;
        }
    }
    s->dmg[s->ndmg++] = r;
}

void scr_resize(rx_screen* s, int w, int h) {
    if (w == s->w && h == s->h) {
        return;
    }
    rcell* nc = calloc((size_t)w * h, sizeof(rcell));
    if (!nc) {
        RX_FATAL("out of memory");
    }
    int cw = w < s->w ? w : s->w;
    int ch = h < s->h ? h : s->h;
    for (int y = 0; y < ch; y++) {
        memcpy(nc + (size_t)y * w, s->cells + (size_t)y * s->w, (size_t)cw * sizeof(rcell));
    }
    free(s->cells);
    s->cells = nc;
    s->w = w;
    s->h = h;
    mark_full(s);
}

void scr_put(rx_screen* s, int x, int y, uint32_t ch, uint16_t attr) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) {
        return;
    }
    rcell* c = &s->cells[(size_t)y * s->w + x];
    if (c->ch == ch && c->attr == attr) {
        return;
    }
    c->ch = ch;
    c->attr = attr;
    mark(s, x, y, 1, 1);
}

void scr_textn_clip(rx_screen* s, int x, int y, const char* utf8, size_t n, uint16_t attr,
                    int maxw) {
    if (maxw <= 0) {
        return;
    }
    size_t i = 0;
    int cx = x;
    while (i < n) {
        uint32_t cp;
        size_t used = utf8_decode(utf8 + i, n - i, &cp);
        if (used == 0) {
            break;
        }
        i += used;
        if (cp == '\n') {
            continue;
        }
        int cw = utf8_cp_width(cp) > 0 ? utf8_cp_width(cp) : 1;
        if (cx + cw > x + maxw) {
            break; /* would cross the right edge (e.g. a window border) */
        }
        scr_put(s, cx, y, cp, attr);
        cx += cw;
    }
}

void scr_textn(rx_screen* s, int x, int y, const char* utf8, size_t n, uint16_t attr) {
    scr_textn_clip(s, x, y, utf8, n, attr, x < s->w ? s->w - x : 0);
}

void scr_text(rx_screen* s, int x, int y, const char* utf8, uint16_t attr) {
    scr_textn(s, x, y, utf8, strlen(utf8), attr);
}

void scr_fill(rx_screen* s, int x, int y, int w, int h, uint32_t ch, uint16_t attr) {
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > s->w) {
        w = s->w - x;
    }
    if (y + h > s->h) {
        h = s->h - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            scr_put(s, xx, yy, ch, attr);
        }
    }
}

void scr_hline(rx_screen* s, int x, int y, int w, uint32_t ch, uint16_t attr) {
    scr_fill(s, x, y, w, 1, ch, attr);
}

void scr_box(rx_screen* s, int x, int y, int w, int h, const char* title, uint16_t attr) {
    if (w < 2 || h < 2) {
        return;
    }
    scr_hline(s, x, y, w, 0x2500, attr); /* ─ */
    scr_hline(s, x, y + h - 1, w, 0x2500, attr);
    for (int yy = y + 1; yy < y + h - 1; yy++) {
        scr_put(s, x, yy, 0x2502, attr); /* │ */
        scr_put(s, x + w - 1, yy, 0x2502, attr);
    }
    scr_put(s, x, y, 0x250C, attr);                 /* ┌ */
    scr_put(s, x + w - 1, y, 0x2510, attr);         /* ┐ */
    scr_put(s, x, y + h - 1, 0x2514, attr);         /* └ */
    scr_put(s, x + w - 1, y + h - 1, 0x2518, attr); /* ┘ */
    if (title && *title) {
        char t[128];
        snprintf(t, sizeof(t), " %s ", title);
        scr_textn_clip(s, x + 2, y, t, strlen(t), attr, w - 4);
    }
}

void scr_set_pair(rx_screen* s, int idx, uint8_t fg, uint8_t bg) {
    if (idx < 0 || idx >= 256) {
        return;
    }
    s->pal[idx][0] = fg;
    s->pal[idx][1] = bg;
}

void scr_damage(rx_screen* s, int x, int y, int w, int h) {
    mark(s, x, y, w, h);
}

int scr_blit(rx_screen* s, const term_backend* tb) {
    if (s->ndmg == 0) {
        return 0;
    }
    tb->blit(s->cells, (size_t)s->w, (size_t)s->h, s->dmg, s->ndmg);
    s->ndmg = 0;
    return 1;
}
