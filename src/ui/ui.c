/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file ui.c
 * @brief Interactive shell event loop (spec §3.3, §6.1): menubar row 0,
 *        workspace with the pane stack, status line on the last row.
 *        Owns the term backend lifecycle and wires the rx_ctx output /
 *        message-line hooks so interpreter output and errors land in the
 *        Command window and on the message line (§6.4, §6.6).
 */
#include "ui/ui_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rx_ui *g_ui;

rx_ui *rx_ui_current(void)
{
    return g_ui;
}

void ui_set_msg(rx_ui *ui, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->msg, sizeof(ui->msg), fmt, ap);
    va_end(ap);
    ui->dirty = 1;
}

void ui_msgline_hook(void *ud, const char *s)
{
    rx_ui *ui = (rx_ui *)ud;
    ui_set_msg(ui, "%s", s);
    ui->tb->bell(); /* §7.4: message line comes with a beep */
}

static void pal_trampoline(int idx, uint8_t fg, uint8_t bg)
{
    if (g_ui) {
        scr_set_pair(&g_ui->scr, idx, fg, bg);
    }
}

static void render_stack_rec(rx_ui *ui, rx_win *w)
{
    if (!w) {
        return;
    }
    render_stack_rec(ui, w->next);
    if (w->visible && w->render) {
        w->render(ui, w); /* stack head renders last: front window on top */
    }
}

void ui_render(rx_ui *ui)
{
    scr_fill(&ui->scr, 0, 1, ui->scr.w, ui->scr.h - 2, ' ', RXA_DESKTOP);
    render_stack_rec(ui, ui->wins);
    menubar_render(ui); /* menubar + open pulldown float above windows */
    statusbar_render(ui);
}

static void sweep_closed(rx_ui *ui)
{
    rx_win *w = ui->wins;
    while (w) {
        rx_win *next = w->next;
        if (w->want_close) {
            if (ui->mouse_cap == w) {
                ui->mouse_cap = NULL;
            }
            win_remove(ui, w);
            if (w->free_fn) {
                w->free_fn(w);
            }
        }
        w = next;
    }
}

static void handle_key(rx_ui *ui, const rkey *k)
{
    if (ui->menu_open >= 0 || ui->menu_kb) {
        if (menubar_key(ui, k)) {
            return;
        }
        if (k->sym == RXK_ESC) {
            menubar_close(ui);
            return;
        }
    }
    rx_win *top = win_top(ui);
    if (top && top->key && top->key(ui, top, k)) {
        return;
    }
    /* global keys (Appendix C) */
    if (k->sym == RXK_F10) {
        ui->menu_kb = 1;
        ui->menu_open = 0;
        ui->menu_sel = 0;
        ui->dirty = 1;
        return;
    }
    if (k->sym >= RXK_ALT('A') && k->sym <= RXK_ALT('Z')) {
        int c = k->sym - RXK_ALT('A') + 'A';
        for (int i = 0; i < (int)rx_menus_n; i++) {
            if (rx_menus[i].hotkey == c) {
                ui->menu_kb = 1;
                ui->menu_open = i;
                ui->menu_sel = 0;
                ui->dirty = 1;
                return;
            }
        }
        return;
    }
    if (k->sym == RXK_F2 && (k->mods & RXK_MOD_CTRL)) {
        /* Ctrl+F2: activate the Command window */
        if (ui->cmd) {
            ui->cmd->w.visible = 1;
            win_bring_top(ui, &ui->cmd->w);
            ui->dirty = 1;
        }
        return;
    }
}

static void handle_mouse(rx_ui *ui, const rmouse *m)
{
    /* row 0 always belongs to the menubar (click-open / drag-sweep, §5.4) */
    if (m->y == 0 || ui->menu_open >= 0) {
        if (menubar_mouse(ui, m)) {
            return;
        }
    }
    if (m->kind == RM_PRESS) {
        ui->mouse_cap = NULL;
        rx_win *w = win_at(ui, m->x, m->y);
        if (w && w->mouse && w->mouse(ui, w, m)) {
            ui->mouse_cap = w; /* capture: drags keep routing here (§6.2) */
            return;
        }
        menubar_close(ui);
        return;
    }
    if (ui->mouse_cap) {
        rx_win *w = ui->mouse_cap;
        if (m->kind == RM_RELEASE) {
            ui->mouse_cap = NULL;
        }
        if (w->mouse && w->mouse(ui, w, m)) {
            return;
        }
    }
}

static void handle_resize(rx_ui *ui)
{
    int w, h;
    ui->tb->size(&w, &h);
    if (w < 20) {
        w = 20;
    }
    if (h < 5) {
        h = 5;
    }
    if (w != ui->scr.w || h != ui->scr.h) {
        scr_resize(&ui->scr, w, h);
        if (ui->cmd) {
            cmdwin_layout(ui, ui->cmd);
        }
        ui->dirty = 1;
    }
}

int rx_ui_run(rx_ctx *ctx, const term_backend *tb)
{
    if (tb->init() != R_OK) {
        return R_ERR_NOUI;
    }
    rx_ui ui;
    memset(&ui, 0, sizeof(ui));
    ui.ctx = ctx;
    ui.tb = tb;
    g_ui = &ui;
    ui.menu_open = -1;
    ui.caps = 0;
    ui.num = 1;

    tb->palette(pal_trampoline);
    tb->cbreak_on();
    int w = 80, h = 24;
    tb->size(&w, &h);
    if (w < 20) {
        w = 20;
    }
    if (h < 5) {
        h = 5;
    }
    scr_init(&ui.scr, w, h);

    ui.cmd = cmdwin_new(&ui);
    ctx->out_fn = cmdwin_out_hook;
    ctx->out_ud = ui.cmd;
    ctx->msg_line = ui_msgline_hook;
    ctx->msg_ud = &ui;

    cmdwin_out(ui.cmd, "Reynard 0.1.0 — the clever fox of terminals\n",
               strlen("Reynard 0.1.0 — the clever fox of terminals\n"));
    cmdwin_out(ui.cmd, "F10 or Alt opens the menubar. Type a command below.\n",
               strlen("F10 or Alt opens the menubar. Type a command below.\n"));
    ui_set_msg(&ui, "Welcome to Reynard");

    ui_render(&ui);
    scr_blit(&ui.scr, tb);

    for (;;) {
        revent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = EV_ERR;
        int rc = tb->poll(&ev, 250);
        if (rc != R_OK) {
            break;
        }
        int active = 0;
        switch (ev.kind) {
        case EV_EOF:
            goto done;
        case EV_RESIZE:
            handle_resize(&ui);
            active = 1;
            break;
        case EV_KEY:
            handle_key(&ui, &ev.u.k);
            active = 1;
            break;
        case EV_MOUSE:
            handle_mouse(&ui, &ev.u.m);
            active = 1;
            break;
        default:
            break;
        }
        if (ctx->quit) {
            goto done;
        }
        sweep_closed(&ui);
        if (active || ui.dirty) {
            ui_render(&ui);
            ui.dirty = 0;
            scr_blit(&ui.scr, tb);
        }
    }
done:
    ctx->out_fn = NULL;
    ctx->out_ud = NULL;
    ctx->msg_line = NULL;
    ctx->msg_ud = NULL;
    while (ui.wins) {
        rx_win *w = ui.wins;
        win_remove(&ui, w);
        if (w->free_fn) {
            w->free_fn(w);
        }
    }
    cmdwin_free(ui.cmd);
    scr_free(&ui.scr);
    tb->cbreak_off();
    tb->shutdown();
    g_ui = NULL;
    return R_OK;
}
