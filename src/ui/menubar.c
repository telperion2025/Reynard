/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file menubar.c
 * @brief Menubar row 0 + pulldowns (spec §6.3, §5.4): click a title to
 *        open, drag across titles to sweep, click an item to select;
 *        release never triggers. Keyboard: F10/Alt activates, arrows
 *        navigate, Enter selects, Esc backs out (Appendix C).
 */
#include "ui/ui_internal.h"

#include <ctype.h>
#include <string.h>

static int title_x(const struct rx_ui *ui, int idx)
{
    int x = 1;
    for (int i = 0; i < idx && i < (int)g_menus_n; i++) {
        x += (int)strlen(g_menus[i].title) + 2;
    }
    (void)ui;
    return x;
}

int menubar_title_at(const struct rx_ui *ui, int x)
{
    for (int i = 0; i < (int)g_menus_n; i++) {
        int tx = title_x(ui, i);
        int len = (int)strlen(g_menus[i].title);
        if (x >= tx && x < tx + len) {
            return i;
        }
    }
    return -1;
}

void menubar_pulldown_geom(const struct rx_ui *ui, int *x, int *y, int *w, int *h)
{
    int m = ui->menu_open;
    const menu_def *md = &g_menus[m];
    int pw = 12;
    for (size_t i = 0; i < md->nitems; i++) {
        if (!md->items[i].sep && md->items[i].label) {
            int l = (int)strlen(md->items[i].label) + 4;
            if (l > pw) {
                pw = l;
            }
        }
    }
    int px = title_x(ui, m);
    if (px + pw > ui->scr.w) {
        px = ui->scr.w - pw;
    }
    if (px < 0) {
        px = 0;
    }
    *x = px;
    *y = 1;
    *w = pw;
    *h = (int)md->nitems + 2;
    if (*y + *h > ui->scr.h) {
        *h = ui->scr.h - *y;
    }
}

static void render_pulldown(struct rx_ui *ui)
{
    int m = ui->menu_open;
    const menu_def *md = &g_menus[m];
    int x, y, w, h;
    menubar_pulldown_geom(ui, &x, &y, &w, &h);
    scr_box(&ui->scr, x, y, w, h, NULL, RXA_PULL);
    scr_fill(&ui->scr, x + 1, y + 1, w - 2, h - 2, ' ', RXA_PULL); /* opaque over windows */
    for (size_t i = 0; i < md->nitems && (int)i + 1 < h - 1; i++) {
        const menu_item *it = &md->items[i];
        int row = y + 1 + (int)i;
        if (it->sep) {
            scr_hline(&ui->scr, x + 1, row, w - 2, 0x2500, RXA_PULL);
            continue;
        }
        int disabled = it->disabled && it->disabled(ui->ctx);
        int checked = it->checked && it->checked(ui->ctx);
        uint16_t attr = (int)i == ui->menu_sel ? RXA_PULL_SEL
                        : disabled              ? RXA_PULL_DIS
                                                : RXA_PULL;
        scr_fill(&ui->scr, x + 1, row, w - 2, 1, ' ', attr);
        if (checked) {
            scr_put(&ui->scr, x + 1, row, 0x221A, attr); /* √ */
        }
        if (it->label) {
            const char *lp = it->label;
            int cx = x + 2;
            int hot_done = 0;
            for (const char *p = lp; *p && cx < x + w - 1; p++) {
                uint16_t a = attr;
                if (it->hotkey && !hot_done && toupper((unsigned char)*p) == it->hotkey) {
                    a = (int)i == ui->menu_sel ? attr : RXA_PULL_HOT;
                    hot_done = 1;
                }
                scr_put(&ui->scr, cx, row, (uint32_t)(unsigned char)*p, a);
                cx++;
            }
        }
    }
}

void menubar_render(struct rx_ui *ui)
{
    scr_fill(&ui->scr, 0, 0, ui->scr.w, 1, ' ', RXA_MENU);
    for (int i = 0; i < (int)g_menus_n; i++) {
        int tx = title_x(ui, i);
        const char *t = g_menus[i].title;
        uint16_t attr = i == ui->menu_open ? RXA_MENU_OPEN
                        : (ui->menu_kb && i == 0) ? RXA_MENU_OPEN
                                                  : RXA_MENU;
        int cx = tx;
        for (const char *p = t; *p && cx < ui->scr.w; p++, cx++) {
            uint16_t a = attr;
            if (toupper((unsigned char)*p) == g_menus[i].hotkey) {
                a = i == ui->menu_open ? attr : RXA_MENU_HOT;
            }
            scr_put(&ui->scr, cx, 0, (uint32_t)(unsigned char)*p, a);
        }
    }
    if (ui->menu_open >= 0) {
        render_pulldown(ui);
    }
}

void menubar_close(struct rx_ui *ui)
{
    if (ui->menu_open >= 0 || ui->menu_kb) {
        ui->menu_open = -1;
        ui->menu_kb = 0;
        ui->menu_sel = 0;
        ui->dirty = 1;
    }
}

static void open_menu(struct rx_ui *ui, int idx)
{
    if (idx < 0 || idx >= (int)g_menus_n) {
        return;
    }
    ui->menu_open = idx;
    ui->menu_kb = 1;
    ui->menu_sel = 0;
    /* first selectable item */
    const menu_def *md = &g_menus[idx];
    while (ui->menu_sel < (int)md->nitems &&
           (md->items[ui->menu_sel].sep ||
            (md->items[ui->menu_sel].disabled && md->items[ui->menu_sel].disabled(ui->ctx)))) {
        ui->menu_sel++;
    }
    if (ui->menu_sel >= (int)md->nitems) {
        ui->menu_sel = 0;
    }
    ui->dirty = 1;
}

static void run_item(struct rx_ui *ui, int idx)
{
    if (idx < 0 || idx >= (int)g_menus[ui->menu_open].nitems) {
        return;
    }
    const menu_item *it = &g_menus[ui->menu_open].items[idx];
    if (it->sep) {
        return;
    }
    if (it->disabled && it->disabled(ui->ctx)) {
        return;
    }
    menubar_close(ui);
    if (it->cmd && it->cmd[0]) {
        rx_run_source(ui->ctx, it->cmd, "<menu>"); /* M9 9.5 app menu command */
    } else if (it->action) {
        it->action(ui->ctx);
    }
    ui->dirty = 1;
}

static int item_at_hotkey(struct rx_ui *ui, int c)
{
    const menu_def *md = &g_menus[ui->menu_open];
    for (size_t i = 0; i < md->nitems; i++) {
        if (!md->items[i].sep && md->items[i].hotkey == toupper(c)) {
            return (int)i;
        }
    }
    return -1;
}

int menubar_key(struct rx_ui *ui, const rkey *k)
{
    if (ui->menu_open < 0 && !ui->menu_kb) {
        /* activation keys */
        if (k->sym == RXK_F10) {
            open_menu(ui, 0);
            return 1;
        }
        if (k->sym >= RXK_ALT('A') && k->sym <= RXK_ALT('Z')) {
            int c = k->sym - RXK_ALT('A') + 'A';
            for (int i = 0; i < (int)g_menus_n; i++) {
                if (g_menus[i].hotkey == c) {
                    open_menu(ui, i);
                    return 1;
                }
            }
            return 1;
        }
        return 0;
    }
    if (ui->menu_open < 0) {
        return 0;
    }
    switch (k->sym) {
    case RXK_ESC:
        menubar_close(ui);
        return 1;
    case RXK_LEFT:
        open_menu(ui, (ui->menu_open + (int)g_menus_n - 1) % (int)g_menus_n);
        return 1;
    case RXK_RIGHT:
        open_menu(ui, (ui->menu_open + 1) % (int)g_menus_n);
        return 1;
    case RXK_UP:
    case RXK_DOWN: {
        const menu_def *md = &g_menus[ui->menu_open];
        int step = k->sym == RXK_DOWN ? 1 : -1;
        int sel = ui->menu_sel;
        for (int n = 0; n < (int)md->nitems; n++) {
            sel = (sel + step + (int)md->nitems) % (int)md->nitems;
            if (!md->items[sel].sep &&
                !(md->items[sel].disabled && md->items[sel].disabled(ui->ctx))) {
                ui->menu_sel = sel;
                ui->dirty = 1;
                return 1;
            }
        }
        return 1;
    }
    case RXK_HOME:
        open_menu(ui, ui->menu_open);
        return 1;
    case RXK_END: {
        const menu_def *md = &g_menus[ui->menu_open];
        ui->menu_sel = (int)md->nitems - 1;
        ui->dirty = 1;
        return 1;
    }
    case RXK_ENTER:
        run_item(ui, ui->menu_sel);
        return 1;
    default:
        if (k->sym == RXK_BASE && k->cp >= 'a' && k->cp <= 'z') {
            int idx = item_at_hotkey(ui, (int)k->cp);
            if (idx >= 0) {
                run_item(ui, idx);
                return 1;
            }
        }
        if (k->sym >= RXK_ALT('A') && k->sym <= RXK_ALT('Z')) {
            int c = k->sym - RXK_ALT('A') + 'A';
            int idx = item_at_hotkey(ui, c);
            if (idx >= 0) {
                run_item(ui, idx);
                return 1;
            }
            for (int i = 0; i < (int)g_menus_n; i++) {
                if (g_menus[i].hotkey == c) {
                    open_menu(ui, i);
                    return 1;
                }
            }
        }
        return 0;
    }
}

int menubar_mouse(struct rx_ui *ui, const rmouse *m)
{
    if (m->kind == RM_PRESS && m->button == 1) {
        if (m->y == 0) {
            int idx = menubar_title_at(ui, m->x);
            if (idx >= 0) {
                open_menu(ui, idx); /* click or drag-sweep target */
                return 1;
            }
            if (ui->menu_open >= 0) {
                menubar_close(ui);
                return 1;
            }
            return 0;
        }
        if (ui->menu_open >= 0) {
            int x, y, w, h;
            menubar_pulldown_geom(ui, &x, &y, &w, &h);
            if (m->x >= x && m->x < x + w && m->y >= y && m->y < y + h) {
                int idx = m->y - y - 1;
                if (idx >= 0 && idx < (int)g_menus[ui->menu_open].nitems) {
                    ui->menu_sel = idx;
                    run_item(ui, idx);
                }
                return 1;
            }
            menubar_close(ui);
            return 1;
        }
        return 0;
    }
    if (m->kind == RM_DRAG && m->button == 1 && ui->menu_open >= 0 && m->y == 0) {
        int idx = menubar_title_at(ui, m->x);
        if (idx >= 0 && idx != ui->menu_open) {
            open_menu(ui, idx); /* drag-sweep across titles (§5.4) */
        }
        return 1;
    }
    if (m->kind == RM_RELEASE) {
        return 0; /* release never triggers an action (§5.4) */
    }
    return 0;
}
