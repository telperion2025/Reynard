/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file term_script.c
 * @brief Headless event-replay backend (spec §5.1). Replays a .events file
 *        (one event per line) against the same blit path and dumps cell
 *        grids for golden-screen comparison. Exists purely for CI
 *        determinism — no wall-clock, no terminal.
 *
 * Event file grammar (one per line, '#' comments, blank lines skipped):
 *   key <tok>            tok: single char, or enter esc tab backtab bs up
 *                        down left right home end pgup pgdn ins del f1..f12,
 *                        optionally prefixed ctrl- alt- shift-, or cp <hex>
 *   text <string>        expands to one key event per character
 *   mouse <kind> <x> <y> <button> [dc]
 *   resize <w> <h>
 *   eof
 *
 * Environment: RX_EVENTS = event file path (init fails without it),
 *              RX_DUMP   = file to write the final cell grid to on shutdown.
 */
#include "term/term_if.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/util.h"

typedef struct script_state {
    FILE *ev;
    rcell *cells;
    size_t w, h;
    unsigned long n_blits;
    char pending[1024]; /**< remaining chars of a partially consumed text event */
    size_t pending_pos;
    char dump_path[260];
} script_state;

static script_state g_state;

static int emit_pending(revent *out)
{
    if (g_state.pending_pos >= strlen(g_state.pending)) {
        return 0;
    }
    const char *arg = g_state.pending + g_state.pending_pos;
    uint32_t cp;
    size_t used = utf8_decode(arg, strlen(arg), &cp);
    g_state.pending_pos += used;
    out->kind = EV_KEY;
    out->u.k.cp = cp;
    out->u.k.sym = RXK_BASE;
    out->u.k.mods = 0;
    return 1;
}

static int sc_init(void)
{
    const char *path = getenv("RX_EVENTS");
    if (!path || !*path) {
        return R_ERR_IO_NOTFOUND;
    }
    memset(&g_state, 0, sizeof(g_state));
    g_state.ev = fopen(path, "r");
    if (!g_state.ev) {
        return R_ERR_IO_OPEN;
    }
    const char *dump = getenv("RX_DUMP");
    if (dump) {
        snprintf(g_state.dump_path, sizeof(g_state.dump_path), "%s", dump);
    }
    return R_OK;
}

static void write_dump(void)
{
    if (!g_state.dump_path[0] || !g_state.cells) {
        return;
    }
    FILE *f = fopen(g_state.dump_path, "w");
    if (!f) {
        return;
    }
    for (size_t y = 0; y < g_state.h; y++) {
        strbuf row;
        sb_init(&row);
        for (size_t x = 0; x < g_state.w; x++) {
            sb_putcp(&row, g_state.cells[y * g_state.w + x].ch ? g_state.cells[y * g_state.w + x].ch
                                                                : (uint32_t)' ');
        }
        /* rstrip spaces */
        while (row.len && row.buf[row.len - 1] == ' ') {
            row.len--;
        }
        row.buf[row.len] = '\0';
        fprintf(f, "%s\n", row.buf ? row.buf : "");
        sb_free(&row);
    }
    fclose(f);
}

static void sc_shutdown(void)
{
    write_dump();
    if (g_state.ev) {
        fclose(g_state.ev);
        g_state.ev = NULL;
    }
    free(g_state.cells);
    g_state.cells = NULL;
}

static int sc_blit(rcell *cells, size_t w, size_t h, struct rrect *damage, size_t n_dmg)
{
    (void)damage;
    (void)n_dmg;
    if (w == 0 || h == 0) {
        return R_ERR_RANGE;
    }
    rcell *nc = realloc(g_state.cells, w * h * sizeof(rcell));
    if (!nc) {
        RX_FATAL("out of memory");
    }
    g_state.cells = nc;
    memcpy(g_state.cells, cells, w * h * sizeof(rcell));
    g_state.w = w;
    g_state.h = h;
    g_state.n_blits++;
    return R_OK;
}

/* FoxPro 16-color base palette (ANSI order). */
static const uint8_t foxpro16[16] = { 0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15 };

static int sc_palette(void (*set_pair)(int idx, uint8_t fg, uint8_t bg))
{
    for (int i = 0; i < 64; i++) {
        set_pair(i, foxpro16[i % 16], foxpro16[(i / 16) % 16]);
    }
    return R_OK;
}

static int key_from_token(const char *tok, rkey *k)
{
    memset(k, 0, sizeof(*k));
    uint8_t mods = 0;
    const char *p = tok;
    for (;;) {
        if (strncmp(p, "ctrl-", 5) == 0) {
            mods |= RXK_MOD_CTRL;
            p += 5;
        } else if (strncmp(p, "alt-", 4) == 0) {
            mods |= RXK_MOD_ALT;
            p += 4;
        } else if (strncmp(p, "shift-", 6) == 0) {
            mods |= RXK_MOD_SHIFT;
            p += 6;
        } else {
            break;
        }
    }
    struct {
        const char *name;
        int sym;
    } names[] = {
        { "enter", RXK_ENTER }, { "esc", RXK_ESC }, { "tab", RXK_TAB },
        { "backtab", RXK_BACKTAB }, { "bs", RXK_BS }, { "up", RXK_UP },
        { "down", RXK_DOWN }, { "left", RXK_LEFT }, { "right", RXK_RIGHT },
        { "home", RXK_HOME }, { "end", RXK_END }, { "pgup", RXK_PGUP },
        { "pgdn", RXK_PGDN }, { "ins", RXK_INS }, { "del", RXK_DEL },
        { "f1", RXK_F1 }, { "f2", RXK_F2 }, { "f3", RXK_F3 }, { "f4", RXK_F4 },
        { "f5", RXK_F5 }, { "f6", RXK_F6 }, { "f7", RXK_F7 }, { "f8", RXK_F8 },
        { "f9", RXK_F9 }, { "f10", RXK_F10 }, { "f11", RXK_F11 }, { "f12", RXK_F12 },
    };
    if (strncmp(p, "cp", 2) == 0 && p[2] == ' ') {
        k->cp = (uint32_t)strtoul(p + 3, NULL, 16);
        k->sym = RXK_BASE;
        k->mods = mods;
        return 1;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(p, names[i].name) == 0) {
            k->sym = names[i].sym;
            k->mods = mods;
            return 1;
        }
    }
    if (strlen(p) == 1) {
        unsigned char c = (unsigned char)p[0];
        k->cp = c;
        if (mods & RXK_MOD_CTRL) {
            k->sym = RXK_CTL(toupper(c));
        } else if (mods & RXK_MOD_ALT) {
            k->sym = RXK_ALT(c);
        } else {
            k->sym = RXK_BASE;
        }
        k->mods = mods;
        return 1;
    }
    return 0;
}

static int next_event(revent *out)
{
    char line[1024];
    while (fgets(line, sizeof(line), g_state.ev)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        size_t n = strlen(s);
        while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
            s[--n] = '\0';
        }
        if (n == 0 || s[0] == '#') {
            continue;
        }
        char *sp = strchr(s, ' ');
        char *arg = NULL;
        if (sp) {
            *sp = '\0';
            arg = sp + 1;
            while (*arg == ' ') {
                arg++;
            }
        }
        if (strcmp(s, "key") == 0 && arg) {
            out->kind = EV_KEY;
            if (!key_from_token(arg, &out->u.k)) {
                return R_ERR_SYNTAX;
            }
            return R_OK;
        }
        if (strcmp(s, "text") == 0 && arg) {
            snprintf(g_state.pending, sizeof(g_state.pending), "%s", arg);
            g_state.pending_pos = 0;
            if (emit_pending(out)) {
                return R_OK;
            }
            continue;
        }
        if (strcmp(s, "mouse") == 0 && arg) {
            char kind[16] = { 0 };
            int x = 0, y = 0, btn = 1, dc = 0;
            if (sscanf(arg, "%15s %d %d %d %d", kind, &x, &y, &btn, &dc) < 4) {
                return R_ERR_SYNTAX;
            }
            out->kind = EV_MOUSE;
            out->u.m.x = (uint16_t)x;
            out->u.m.y = (uint16_t)y;
            out->u.m.button = (uint8_t)btn;
            out->u.m.dc = (uint8_t)(dc ? 1 : 0);
            if (strcmp(kind, "press") == 0) {
                out->u.m.kind = RM_PRESS;
            } else if (strcmp(kind, "release") == 0) {
                out->u.m.kind = RM_RELEASE;
            } else if (strcmp(kind, "drag") == 0) {
                out->u.m.kind = RM_DRAG;
            } else if (strcmp(kind, "wheelup") == 0) {
                out->u.m.kind = RM_WHEEL_UP;
            } else if (strcmp(kind, "wheeldn") == 0) {
                out->u.m.kind = RM_WHEEL_DN;
            } else if (strcmp(kind, "move") == 0) {
                out->u.m.kind = RM_MOVE;
            } else {
                return R_ERR_SYNTAX;
            }
            return R_OK;
        }
        if (strcmp(s, "resize") == 0 && arg) {
            int w = 0, h = 0;
            if (sscanf(arg, "%d %d", &w, &h) != 2) {
                return R_ERR_SYNTAX;
            }
            out->kind = EV_RESIZE;
            out->u.m.x = (uint16_t)w;
            out->u.m.y = (uint16_t)h;
            return R_OK;
        }
        if (strcmp(s, "eof") == 0) {
            out->kind = EV_EOF;
            return R_OK;
        }
        if (strcmp(s, "sleep") == 0) {
            continue; /* virtual time is irrelevant in replay */
        }
        return R_ERR_SYNTAX;
    }
    out->kind = EV_EOF;
    return R_OK;
}

static int sc_poll(revent *out, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!g_state.ev) {
        out->kind = EV_ERR;
        return R_ERR_IO_NOTFOUND;
    }
    if (emit_pending(out)) {
        return R_OK;
    }
    return next_event(out);
}

static void sc_bell(void)
{
    rx_log(RX_LOG_DEBUG, "bell");
}

static void sc_cbreak_on(void)
{
}

static void sc_cbreak_off(void)
{
}

static const term_backend script_backend = {
    .init = sc_init,
    .shutdown = sc_shutdown,
    .blit = sc_blit,
    .palette = sc_palette,
    .poll = sc_poll,
    .bell = sc_bell,
    .cbreak_on = sc_cbreak_on,
    .cbreak_off = sc_cbreak_off,
    .name = "script",
};

const term_backend *rx_term_script(void)
{
    return &script_backend;
}

unsigned long rx_term_script_blit_count(void)
{
    return g_state.n_blits;
}
