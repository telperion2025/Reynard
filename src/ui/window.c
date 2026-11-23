/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file window.c
 * @brief Uniform pane stack (spec §6.2): every window (Command window,
 *        dialogs) lives on one stack; the head is the front. Provides
 *        hit-testing, border-drag zones, clamping and the shared border
 *        renderer.
 */
#include "ui/ui_internal.h"

#include <string.h>

void win_push(struct rx_ui *ui, rx_win *w)
{
    w->next = ui->wins;
    ui->wins = w;
    ui->dirty = 1;
}

void win_remove(struct rx_ui *ui, rx_win *w)
{
    rx_win **pp = &ui->wins;
    while (*pp) {
        if (*pp == w) {
            *pp = w->next;
            w->next = NULL;
            ui->dirty = 1;
            return;
        }
        pp = &(*pp)->next;
    }
}

void win_bring_top(struct rx_ui *ui, rx_win *w)
{
    win_remove(ui, w);
    win_push(ui, w);
}

rx_win *win_top(struct rx_ui *ui)
{
    return ui->wins;
}

rx_win *win_at(struct rx_ui *ui, int x, int y)
{
    for (rx_win *w = ui->wins; w; w = w->next) {
        if (w->visible && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
            return w;
        }
    }
    return NULL;
}

int win_border_zone(const rx_win *w, int x, int y)
{
    int on_left = x == w->x;
    int on_right = x == w->x + w->w - 1;
    int on_top = y == w->y;
    int on_bottom = y == w->y + w->h - 1;
    if (!(on_left || on_right || on_top || on_bottom)) {
        return WIN_ZONE_NONE;
    }
    if ((on_left || on_right) && (on_top || on_bottom)) {
        return WIN_ZONE_SIZE; /* corner drag resizes */
    }
    return WIN_ZONE_MOVE;
}

void win_box(struct rx_ui *ui, const rx_win *w, uint16_t border, uint16_t title_attr)
{
    scr_box(&ui->scr, w->x, w->y, w->w, w->h, NULL, border);
    if (w->title[0]) {
        char t[80];
        snprintf(t, sizeof(t), " %s ", w->title);
        scr_textn(&ui->scr, w->x + 2, w->y, t, strlen(t), title_attr);
    }
}

void win_clamp(struct rx_ui *ui, rx_win *w)
{
    if (w->w < w->min_w) {
        w->w = w->min_w;
    }
    if (w->h < w->min_h) {
        w->h = w->min_h;
    }
    if (w->x < 0) {
        w->x = 0;
    }
    if (w->y < 1) {
        w->y = 1; /* row 0 is the menubar */
    }
    /* shrink to fit at the current position when possible, else move */
    if (w->x + w->w > ui->scr.w) {
        int nw = ui->scr.w - w->x;
        if (nw >= w->min_w) {
            w->w = nw;
        } else {
            w->x = ui->scr.w - w->w;
        }
    }
    if (w->y + w->h > ui->scr.h - 1) {
        int nh = ui->scr.h - 1 - w->y;
        if (nh >= w->min_h) {
            w->h = nh;
        } else {
            w->y = ui->scr.h - 1 - w->h;
        }
    }
    if (w->x < 0) {
        w->x = 0;
    }
    if (w->y < 1) {
        w->y = 1;
    }
}
