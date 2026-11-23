/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file getsay.c
 * @brief Shared single-line editor core (spec §6.4): used by the Command
 *        window and later by @GET fields. Full cursor motion, Ctrl+Y
 *        kill-to-end, Ctrl+Up/Down history ring (persisted to
 *        $XDG_CACHE_HOME/reynard/history, 1000 entries), Tab command-name
 *        completion.
 */
#include "ui/ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static void mkpath(const char *dir)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void gs_ensure(getsay *g, size_t need)
{
    if (g->len + need + 1 <= g->cap) {
        return;
    }
    size_t nc = g->cap ? g->cap * 2 : 64;
    while (nc < g->len + need + 1) {
        nc *= 2;
    }
    char *nb = realloc(g->buf, nc);
    if (!nb) {
        RX_FATAL("out of memory");
    }
    g->buf = nb;
    g->cap = nc;
}

void gs_init(getsay *g, size_t maxhist)
{
    memset(g, 0, sizeof(*g));
    g->maxhist = maxhist ? maxhist : GS_MAXHIST;
    g->hpos = 0;
    g->comp = -1;
    gs_hist_path(g, g->hist_path, sizeof(g->hist_path));
    gs_hist_load(g);
}

void gs_free(getsay *g)
{
    free(g->buf);
    for (size_t i = 0; i < g->nhist; i++) {
        free(g->hist[i]);
    }
    free(g->hist);
    memset(g, 0, sizeof(*g));
}

void gs_clear(getsay *g)
{
    g->len = 0;
    g->cur = 0;
    if (g->buf) {
        g->buf[0] = '\0';
    }
    g->comp = -1;
}

void gs_insert_cp(getsay *g, uint32_t cp)
{
    char tmp[5];
    size_t n = utf8_encode(cp, tmp);
    gs_insert(g, tmp, n);
}

void gs_insert(getsay *g, const char *s, size_t n)
{
    gs_ensure(g, n);
    memmove(g->buf + g->cur + n, g->buf + g->cur, g->len - g->cur);
    memcpy(g->buf + g->cur, s, n);
    g->len += n;
    g->cur += n;
    g->buf[g->len] = '\0';
    g->comp = -1;
}

static size_t prev_cp_off(getsay *g, size_t pos)
{
    if (pos == 0) {
        return 0;
    }
    size_t p = pos - 1;
    while (p > 0 && ((unsigned char)g->buf[p] & 0xC0) == 0x80) {
        p--;
    }
    return p;
}

static size_t next_cp_off(getsay *g, size_t pos)
{
    if (pos >= g->len) {
        return g->len;
    }
    uint32_t cp;
    size_t used = utf8_decode(g->buf + pos, g->len - pos, &cp);
    return pos + (used ? used : 1);
}

int gs_backspace(getsay *g)
{
    if (g->cur == 0) {
        return 0;
    }
    size_t p = prev_cp_off(g, g->cur);
    memmove(g->buf + p, g->buf + g->cur, g->len - g->cur);
    g->len -= g->cur - p;
    g->cur = p;
    g->buf[g->len] = '\0';
    g->comp = -1;
    return 1;
}

int gs_delete(getsay *g)
{
    if (g->cur >= g->len) {
        return 0;
    }
    size_t n = next_cp_off(g, g->cur);
    memmove(g->buf + g->cur, g->buf + n, g->len - n);
    g->len -= n - g->cur;
    g->buf[g->len] = '\0';
    g->comp = -1;
    return 1;
}

void gs_left(getsay *g)
{
    g->cur = prev_cp_off(g, g->cur);
    g->comp = -1;
}

void gs_right(getsay *g)
{
    g->cur = next_cp_off(g, g->cur);
    g->comp = -1;
}

void gs_home(getsay *g)
{
    g->cur = 0;
    g->comp = -1;
}

void gs_end(getsay *g)
{
    g->cur = g->len;
    g->comp = -1;
}

void gs_kill_end(getsay *g)
{
    g->len = g->cur;
    g->buf[g->len] = '\0';
    g->comp = -1;
}

int gs_hist_up(getsay *g)
{
    if (g->hpos == 0) {
        return 0;
    }
    g->hpos--;
    const char *h = g->hist[g->hpos];
    gs_clear(g);
    gs_insert(g, h, strlen(h));
    return 1;
}

int gs_hist_down(getsay *g)
{
    if (g->hpos >= g->nhist) {
        return 0;
    }
    g->hpos++;
    gs_clear(g);
    if (g->hpos < g->nhist) {
        const char *h = g->hist[g->hpos];
        gs_insert(g, h, strlen(h));
    }
    return 1;
}

void gs_hist_add(getsay *g, const char *line)
{
    if (!line || !*line) {
        return;
    }
    if (g->nhist && g->hist[g->nhist - 1] && strcmp(g->hist[g->nhist - 1], line) == 0) {
        return; /* no consecutive duplicates */
    }
    if (g->nhist == g->caphist) {
        size_t nc = g->caphist ? g->caphist * 2 : 64;
        char **nh = realloc(g->hist, nc * sizeof(char *));
        if (!nh) {
            RX_FATAL("out of memory");
        }
        g->hist = nh;
        g->caphist = nc;
    }
    g->hist[g->nhist++] = rx_strdup(line);
    if (g->nhist > g->maxhist) {
        free(g->hist[0]);
        memmove(g->hist, g->hist + 1, (g->nhist - 1) * sizeof(char *));
        g->nhist--;
    }
    g->hpos = g->nhist;
    gs_hist_save(g);
}

void gs_hist_path(getsay *g, char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    char base[300];
    if (xdg && *xdg) {
        snprintf(base, sizeof(base), "%s/reynard", xdg);
    } else {
        const char *home = getenv("HOME");
        if (home && *home) {
            snprintf(base, sizeof(base), "%s/.cache/reynard", home);
        } else {
            snprintf(base, sizeof(base), "/tmp/reynard-cache");
        }
    }
    snprintf(out, outsz, "%s/history", base);
    (void)g;
}

void gs_hist_load(getsay *g)
{
    FILE *f = fopen(g->hist_path, "r");
    if (!f) {
        return;
    }
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (g->nhist >= g->maxhist) {
            free(g->hist[0]);
            memmove(g->hist, g->hist + 1, (g->nhist - 1) * sizeof(char *));
            g->nhist--;
        }
        if (g->nhist == g->caphist) {
            size_t nc = g->caphist ? g->caphist * 2 : 64;
            char **nh = realloc(g->hist, nc * sizeof(char *));
            if (!nh) {
                RX_FATAL("out of memory");
            }
            g->hist = nh;
            g->caphist = nc;
        }
        g->hist[g->nhist++] = rx_strdup(line);
    }
    fclose(f);
    g->hpos = g->nhist;
}

void gs_reset_hist_nav(getsay *g)
{
    g->hpos = g->nhist;
}

void gs_hist_save(getsay *g)
{
    if (!g->hist_path[0]) {
        return;
    }
    char dir[300];
    snprintf(dir, sizeof(dir), "%s", g->hist_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkpath(dir);
    }
    FILE *f = fopen(g->hist_path, "w");
    if (!f) {
        return;
    }
    for (size_t i = 0; i < g->nhist; i++) {
        fprintf(f, "%s\n", g->hist[i]);
    }
    fclose(f);
}

void gs_complete(getsay *g)
{
    size_t kw_n = 0;
    const char *const *kws = rx_keyword_list(&kw_n);
    /* the first word of the line is the completion subject */
    size_t wl = 0;
    while (wl < g->len && (g->buf[wl] == ' ' || g->buf[wl] == '\t')) {
        wl++;
    }
    size_t wend = wl;
    while (wend < g->len && g->buf[wend] != ' ' && g->buf[wend] != '\t') {
        wend++;
    }
    size_t wlen = wend - wl;
    char word[128];
    if (wlen >= sizeof(word)) {
        return;
    }
    memcpy(word, g->buf + wl, wlen);
    word[wlen] = '\0';
    rx_strtoupper(word);
    if (g->comp < 0) {
        snprintf(g->comp_base, sizeof(g->comp_base), "%s", word);
        g->comp = 0;
    }
    size_t start = (size_t)g->comp;
    for (size_t k = 0; k < kw_n; k++) {
        size_t i = (start + k) % kw_n;
        if (wlen == 0 || (strlen(kws[i]) >= wlen && strncasecmp(kws[i], word, wlen) == 0)) {
            if (strcmp(kws[i], word) == 0 && k + start != 0) {
                continue;
            }
            /* replace the first word */
            size_t newlen = wl + strlen(kws[i]) + (g->len - wend);
            gs_ensure(g, newlen + 1);
            memmove(g->buf + wl + strlen(kws[i]), g->buf + wend, g->len - wend);
            memcpy(g->buf + wl, kws[i], strlen(kws[i]));
            g->len = newlen;
            g->cur = wl + strlen(kws[i]);
            g->buf[g->len] = '\0';
            g->comp = (int)(i + 1) % (int)kw_n;
            return;
        }
    }
    g->comp = -1;
}

void gs_render(getsay *g, rx_screen *s, int x, int y, int w, uint16_t attr)
{
    scr_fill(s, x, y, w, 1, ' ', attr);
    if (!g->buf) {
        g->buf = calloc(1, 1);
        g->cap = 1;
    }
    scr_textn(s, x, y, g->buf, g->len, attr);
    /* cursor: inverse cell at the cursor position (no cursor API in the
       term vtable; §5.2 renders the caret as an attr-swapped cell) */
    int cx = x;
    for (size_t i = 0; i < g->cur && i < g->len;) {
        uint32_t cp;
        size_t used = utf8_decode(g->buf + i, g->len - i, &cp);
        if (!used) {
            break;
        }
        i += used;
        int cw = utf8_cp_width(cp);
        cx += cw > 0 ? cw : 1;
    }
    if (cx >= x + w) {
        cx = x + w - 1;
    }
    if (cx >= 0 && cx < s->w && y >= 0 && y < s->h) {
        rcell *c = &s->cells[(size_t)y * s->w + cx];
        uint16_t inv = (uint16_t)(((c->attr & 0x0F) << 4) | ((c->attr >> 4) & 0x0F));
        uint32_t oldch = c->ch;
        uint16_t oldattr = c->attr;
        if (c->ch == 0) {
            c->ch = ' ';
        }
        c->attr = inv;
        if (c->ch != oldch || c->attr != oldattr) {
            scr_damage(s, cx, y, 1, 1);
        }
    }
}
