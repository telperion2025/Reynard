/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmdwin.c
 * @brief The Command window (spec §6.4): a persistent floating window on the
 *        pane stack. All input goes through the getsay.c editor core; output
 *        from the interpreter (via the ctx->out_fn hook) scrolls in a
 *        per-window ring buffer (default 5000 lines). Border drag moves,
 *        corner drag resizes; release never triggers (§5.4).
 */
#include "ui/ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void scroll_push(rx_cmdwin* c, const char* line) {
    if (c->nlines == c->caplines) {
        if (c->caplines >= c->maxlines) {
            free(c->lines[c->head]);
            c->head = (c->head + 1) % c->caplines;
            c->nlines--;
        } else {
            size_t nc = c->caplines ? c->caplines * 2 : 128;
            if (nc > c->maxlines) {
                nc = c->maxlines;
            }
            char** nl = realloc(c->lines, nc * sizeof(char*));
            if (!nl) {
                RX_FATAL("out of memory");
            }
            c->lines = nl;
            c->caplines = nc;
        }
    }
    size_t idx = (c->head + c->nlines) % c->caplines;
    c->lines[idx] = rx_strdup(line);
    c->nlines++;
}

void cmdwin_out(rx_cmdwin* c, const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '\n') {
            c->pend[c->npend] = '\0';
            scroll_push(c, c->pend);
            c->npend = 0;
        } else if (ch != '\r') {
            if (c->npend + 1 < sizeof(c->pend)) {
                c->pend[c->npend++] = ch;
            }
        }
    }
}

int cmdwin_out_hook(void* ud, const char* s, size_t n) {
    cmdwin_out((rx_cmdwin*)ud, s, n);
    return R_OK;
}

void cmdwin_layout(struct rx_ui* ui, rx_cmdwin* c) {
    if (c->w.w == 0 || c->w.h == 0) {
        int h = ui->scr.h - 1 - 1; /* above the status line */
        int ch = 8;
        if (ch > h - 2) {
            ch = h - 2;
        }
        if (ch < c->w.min_h) {
            ch = c->w.min_h;
        }
        c->w.x = 2;
        c->w.y = ui->scr.h - 1 - ch;
        c->w.w = ui->scr.w - 4;
        c->w.h = ch;
    }
    win_clamp(ui, &c->w);
}

static void cmdwin_render(struct rx_ui* ui, rx_win* w) {
    rx_cmdwin* c = w->ud;
    win_box(ui, w, RXA_CMD_BORDER, RXA_CMD_TITLE);
    int ix = w->x + 1, iy = w->y + 1;
    int iw = w->w - 2, ih = w->h - 2;
    if (iw <= 0 || ih <= 0) {
        return;
    }
    scr_fill(&ui->scr, ix, iy, iw, ih, ' ', RXA_CMD); /* opaque interior */
    int input_row = iy + ih - 1;
    int nscroll = ih - 1;
    for (int r = 0; r < nscroll; r++) {
        long li = (long)c->nlines - nscroll + r;
        if (li < 0) {
            continue;
        }
        const char* line = c->lines[(c->head + (size_t)li) % c->caplines];
        scr_textn_clip(&ui->scr, ix, iy + r, line, strlen(line), RXA_CMD, iw);
    }
    gs_render(&c->ed, &ui->scr, ix, input_row, iw, RXA_CMD);
}

static int cmdwin_key(struct rx_ui* ui, rx_win* w, const rkey* k) {
    rx_cmdwin* c = w->ud;
    if (ui->paused && !(k->sym == RXK_CTL('S'))) {
        return 1; /* paused: swallow input except the resume key */
    }
    switch (k->sym) {
    case RXK_ENTER: {
        char* line = rx_strdup(c->ed.buf ? c->ed.buf : "");
        if (line[0]) {
            cmdwin_out(c, line, strlen(line));
            cmdwin_out(c, "\n", 1);
            gs_hist_add(&c->ed, line);
            rx_run_source(ui->ctx, line, "<command>");
            /* an interactive error shows on the message line; it must not
               poison the next command or the exit code */
            ui->ctx->abort = 0;
            ui->ctx->err.code = R_OK;
            ui->ctx->err_printed = 0;
        } else {
            cmdwin_out(c, "\n", 1);
        }
        free(line);
        gs_clear(&c->ed);
        gs_reset_hist_nav(&c->ed);
        ui->dirty = 1;
        return 1;
    }
    case RXK_ESC:
        if (ui->ctx->set.escape) {
            gs_clear(&c->ed);
            gs_reset_hist_nav(&c->ed);
            ui_set_msg(ui, "");
            ui->dirty = 1;
        }
        return 1;
    case RXK_BS:
        gs_backspace(&c->ed);
        ui->dirty = 1;
        return 1;
    case RXK_DEL:
        gs_delete(&c->ed);
        ui->dirty = 1;
        return 1;
    case RXK_LEFT:
        gs_left(&c->ed);
        ui->dirty = 1;
        return 1;
    case RXK_RIGHT:
        gs_right(&c->ed);
        ui->dirty = 1;
        return 1;
    case RXK_HOME:
        gs_home(&c->ed);
        ui->dirty = 1;
        return 1;
    case RXK_END:
        gs_end(&c->ed);
        ui->dirty = 1;
        return 1;
    case RXK_UP:
    case RXK_DOWN:
        if (k->sym == RXK_UP) {
            gs_hist_up(&c->ed);
        } else {
            gs_hist_down(&c->ed);
        }
        ui->dirty = 1;
        return 1;
    case RXK_TAB:
        gs_complete(&c->ed);
        ui->dirty = 1;
        return 1;
    case RXK_CTL('Y'):
        gs_kill_end(&c->ed);
        ui->dirty = 1;
        return 1;
    case RXK_CTL('S'):
        ui->paused = !ui->paused;
        ui_set_msg(ui, ui->paused ? "Paused (Ctrl+S to resume)" : "");
        ui->dirty = 1;
        return 1;
    default:
        if (k->sym == RXK_BASE && k->cp >= 32) {
            gs_insert_cp(&c->ed, k->cp);
            ui->dirty = 1;
            return 1;
        }
        return 0;
    }
}

static int cmdwin_mouse(struct rx_ui* ui, rx_win* w, const rmouse* m) {
    rx_cmdwin* c = w->ud;
    int zone = win_border_zone(w, m->x, m->y);
    switch (m->kind) {
    case RM_PRESS:
        win_bring_top(ui, w);
        if (m->button == 1 && win_close_hit(w, m->x, m->y)) {
            w->visible = 0; /* Command window hides; Ctrl+F2 / Window menu bring it back */
        } else if (m->button == 1 && zone != WIN_ZONE_NONE) {
            c->drag = zone;
            c->ddx = m->x - w->x;
            c->ddy = m->y - w->y;
        }
        ui->dirty = 1;
        return 1;
    case RM_DRAG:
        if (c->drag != WIN_ZONE_NONE && m->button == 1) {
            if (c->drag == WIN_ZONE_MOVE) {
                w->x = m->x - c->ddx;
                w->y = m->y - c->ddy;
            } else {
                w->w = m->x - w->x + 1;
                w->h = m->y - w->y + 1;
            }
            win_clamp(ui, w);
            ui->dirty = 1;
            return 1;
        }
        return 0;
    case RM_RELEASE:
        c->drag = WIN_ZONE_NONE; /* release never triggers an action (§5.4) */
        return 1;
    default:
        return zone != WIN_ZONE_NONE;
    }
}

rx_cmdwin* cmdwin_new(struct rx_ui* ui) {
    rx_cmdwin* c = calloc(1, sizeof(*c));
    if (!c) {
        RX_FATAL("out of memory");
    }
    snprintf(c->w.title, sizeof(c->w.title), "Command");
    c->w.min_w = 20;
    c->w.min_h = 4;
    c->w.visible = 1;
    c->maxlines = CMDWIN_MAXLINES;
    gs_init(&c->ed, GS_MAXHIST);
    cmdwin_layout(ui, c);
    c->w.render = cmdwin_render;
    c->w.key = cmdwin_key;
    c->w.mouse = cmdwin_mouse;
    c->w.ud = c;
    win_push(ui, &c->w);
    return c;
}

void cmdwin_free(rx_cmdwin* c) {
    gs_hist_save(&c->ed);
    gs_free(&c->ed);
    for (size_t i = 0; i < c->nlines; i++) {
        free(c->lines[(c->head + i) % c->caplines]);
    }
    free(c->lines);
    free(c);
}
