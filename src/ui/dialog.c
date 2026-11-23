/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file dialog.c
 * @brief Modal dialogs on the pane stack (spec §6.2/§6.3): About, Macros,
 *        Filer, Calculator, Calendar/Diary, ASCII Chart. Every control is a
 *        hitbox; Esc closes; border drag moves the dialog.
 */
#include "ui/ui_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct rx_dlg {
    rx_win w;
    char **lines;
    size_t nlines, cap;
    int scroll;
    int has_ed;
    getsay ed;
    int drag;
    int ddx, ddy;
} rx_dlg;

void dlg_add_line(rx_win *w, const char *line)
{
    rx_dlg *d = w->ud;
    if (d->nlines == d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 32;
        char **nl = realloc(d->lines, nc * sizeof(char *));
        if (!nl) {
            RX_FATAL("out of memory");
        }
        d->lines = nl;
        d->cap = nc;
    }
    d->lines[d->nlines++] = rx_strdup(line);
}

void dlg_set_editor(rx_win *w)
{
    rx_dlg *d = w->ud;
    d->has_ed = 1;
    gs_init(&d->ed, 0);
}

static void dlg_add(rx_dlg *d, const char *line)
{
    dlg_add_line(&d->w, line);
}

static void dlg_render(struct rx_ui *ui, rx_win *w)
{
    rx_dlg *d = w->ud;
    win_box(ui, w, RXA_DLG_BORDER, RXA_DLG_TITLE);
    int ix = w->x + 1, iy = w->y + 1;
    int iw = w->w - 2, ih = w->h - 2;
    if (iw <= 0 || ih <= 0) {
        return;
    }
    scr_fill(&ui->scr, ix, iy, iw, ih, ' ', RXA_DLG); /* opaque interior */
    int rows = d->has_ed ? ih - 1 : ih;
    for (int r = 0; r < rows; r++) {
        long li = (long)d->nlines - rows + r - d->scroll;
        if (li < 0 || li >= (long)d->nlines) {
            continue;
        }
        scr_textn(&ui->scr, ix, iy + r, d->lines[li], strlen(d->lines[li]), RXA_DLG);
    }
    if (d->has_ed) {
        gs_render(&d->ed, &ui->scr, ix, iy + ih - 1, iw, RXA_DLG);
    }
}

static void dlg_close(struct rx_ui *ui, rx_dlg *d)
{
    (void)ui;
    d->w.want_close = 1;
}

static int dlg_key(struct rx_ui *ui, rx_win *w, const rkey *k)
{
    rx_dlg *d = w->ud;
    switch (k->sym) {
    case RXK_ESC:
        dlg_close(ui, d);
        return 1;
    case RXK_ENTER:
        if (d->has_ed) {
            char *line = rx_strdup(d->ed.buf ? d->ed.buf : "");
            if (line[0]) {
                rval v;
                r_err err;
                rx_rval_undef(&v);
                if (rx_eval_str(ui->ctx, line, &v, &err) == R_OK) {
                    strbuf sb;
                    sb_init(&sb);
                    rx_vm_display(ui->ctx, &v, &sb);
                    char out[600];
                    snprintf(out, sizeof(out), "= %s", sb.buf ? sb.buf : "");
                    dlg_add(d, out);
                    sb_free(&sb);
                    rx_rval_free(&v);
                } else {
                    rx_raise(ui->ctx, err.code, err.line, err.col, "%s", err.msg);
                    ui->ctx->abort = 0;
                    ui->ctx->err_printed = 0;
                }
            }
            free(line);
            gs_clear(&d->ed);
            ui->dirty = 1;
            return 1;
        }
        dlg_close(ui, d);
        return 1;
    case RXK_UP:
        if (d->scroll + (w->h - 2) < (int)d->nlines) {
            d->scroll++;
        }
        ui->dirty = 1;
        return 1;
    case RXK_DOWN:
        if (d->scroll > 0) {
            d->scroll--;
        }
        ui->dirty = 1;
        return 1;
    case RXK_PGUP:
        d->scroll += w->h - 2 > 0 ? w->h - 2 : 1;
        if (d->scroll > (int)d->nlines) {
            d->scroll = (int)d->nlines;
        }
        ui->dirty = 1;
        return 1;
    case RXK_PGDN:
        d->scroll = 0;
        ui->dirty = 1;
        return 1;
    case RXK_BS:
        if (d->has_ed) {
            gs_backspace(&d->ed);
            ui->dirty = 1;
        }
        return 1;
    case RXK_DEL:
        if (d->has_ed) {
            gs_delete(&d->ed);
            ui->dirty = 1;
        }
        return 1;
    case RXK_LEFT:
        if (d->has_ed) {
            gs_left(&d->ed);
            ui->dirty = 1;
        }
        return 1;
    case RXK_RIGHT:
        if (d->has_ed) {
            gs_right(&d->ed);
            ui->dirty = 1;
        }
        return 1;
    case RXK_HOME:
        if (d->has_ed) {
            gs_home(&d->ed);
            ui->dirty = 1;
        }
        return 1;
    case RXK_END:
        if (d->has_ed) {
            gs_end(&d->ed);
            ui->dirty = 1;
        }
        return 1;
    case RXK_CTL('Y'):
        if (d->has_ed) {
            gs_kill_end(&d->ed);
            ui->dirty = 1;
        }
        return 1;
    default:
        if (d->has_ed && k->sym == RXK_BASE && k->cp >= 32) {
            gs_insert_cp(&d->ed, k->cp);
            ui->dirty = 1;
            return 1;
        }
        return 0;
    }
}

static int dlg_mouse(struct rx_ui *ui, rx_win *w, const rmouse *m)
{
    rx_dlg *d = w->ud;
    int zone = win_border_zone(w, m->x, m->y);
    switch (m->kind) {
    case RM_PRESS:
        win_bring_top(ui, w);
        if (m->button == 1 && win_close_hit(w, m->x, m->y)) {
            dlg_close(ui, d);
        } else if (m->button == 1 && zone != WIN_ZONE_NONE) {
            d->drag = zone;
            d->ddx = m->x - w->x;
            d->ddy = m->y - w->y;
        }
        ui->dirty = 1;
        return 1;
    case RM_DRAG:
        if (d->drag != WIN_ZONE_NONE && m->button == 1) {
            if (d->drag == WIN_ZONE_MOVE) {
                w->x = m->x - d->ddx;
                w->y = m->y - d->ddy;
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
        d->drag = WIN_ZONE_NONE;
        return 1;
    default:
        return 0;
    }
}

static void dlg_free(rx_win *w)
{
    rx_dlg *d = w->ud;
    if (d->has_ed) {
        gs_free(&d->ed);
    }
    for (size_t i = 0; i < d->nlines; i++) {
        free(d->lines[i]);
    }
    free(d->lines);
    free(d);
}

rx_win *dlg_new(struct rx_ui *ui, const char *title, int w, int h)
{
    rx_dlg *d = calloc(1, sizeof(*d));
    if (!d) {
        RX_FATAL("out of memory");
    }
    snprintf(d->w.title, sizeof(d->w.title), "%s", title);
    d->w.min_w = 10;
    d->w.min_h = 4;
    d->w.visible = 1;
    d->w.w = w;
    d->w.h = h;
    d->w.x = (ui->scr.w - w) / 2;
    d->w.y = (ui->scr.h - h) / 2;
    if (d->w.y < 1) {
        d->w.y = 1;
    }
    win_clamp(ui, &d->w);
    d->w.render = dlg_render;
    d->w.key = dlg_key;
    d->w.mouse = dlg_mouse;
    d->w.free_fn = dlg_free;
    d->w.ud = d;
    win_push(ui, &d->w);
    return &d->w;
}

static rx_dlg *dlg_lines(struct rx_ui *ui, const char *title, int ww, int hh,
                        const char *const *lines, size_t n)
{
    rx_win *w = dlg_new(ui, title, ww, hh);
    rx_dlg *d = w->ud;
    for (size_t i = 0; i < n; i++) {
        dlg_add(d, lines[i]);
    }
    return d;
}

void dlg_about(struct rx_ui *ui)
{
    static const char *const lines[] = {
        "Reynard 0.1.0 — the clever fox of terminals",
        "A FoxPro 2.6a reimplementation for Unix terminals.",
        "",
        "Copyright (c) 2025 The Reynard contributors.",
        "2-clause BSD license — see reynard --dump-license.",
        "",
        "Press Esc to close.",
    };
    dlg_lines(ui, "About Reynard", 52, 9, lines, sizeof(lines) / sizeof(lines[0]));
}

void dlg_macros(struct rx_ui *ui)
{
    static const char *const lines[] = {
        "Macros",
        "",
        "No macros are defined.",
        "Macro recording lands with the M9 project manager.",
        "",
        "Press Esc to close.",
    };
    dlg_lines(ui, "Macros", 48, 8, lines, sizeof(lines) / sizeof(lines[0]));
}

static int namecmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void dlg_filer(struct rx_ui *ui)
{
    rx_win *w = dlg_new(ui, "Filer", 56, 16);
    rx_dlg *d = w->ud;
    DIR *dir = opendir(".");
    if (dir) {
        char **names = NULL;
        size_t nn = 0, cap = 0;
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (e->d_name[0] == '.') {
                continue;
            }
            if (nn == cap) {
                cap = cap ? cap * 2 : 64;
                names = realloc(names, cap * sizeof(char *));
                if (!names) {
                    RX_FATAL("out of memory");
                }
            }
            names[nn++] = rx_strdup(e->d_name);
        }
        closedir(dir);
        qsort(names, nn, sizeof(char *), namecmp);
        for (size_t i = 0; i < nn; i++) {
            struct stat st;
            char line[300];
            if (stat(names[i], &st) == 0 && S_ISREG(st.st_mode)) {
                snprintf(line, sizeof(line), "%-40s %8lld", names[i], (long long)st.st_size);
            } else {
                snprintf(line, sizeof(line), "%-40s <dir>", names[i]);
            }
            dlg_add(d, line);
            free(names[i]);
        }
        free(names);
    }
    if (d->nlines == 0) {
        dlg_add(d, "(empty directory)");
    }
    dlg_add(d, "");
    dlg_add(d, "Up/Down scroll — Esc closes.");
}

void dlg_calc(struct rx_ui *ui)
{
    rx_win *w = dlg_new(ui, "Calculator", 44, 12);
    rx_dlg *d = w->ud;
    dlg_set_editor(w);
    dlg_add(d, "Enter an expression, Enter evaluates:");
    dlg_add(d, "");
    dlg_add(d, "(results appear above the input line)");
    dlg_add(d, "");
    dlg_add(d, "Esc closes.");
}

static int days_in_month(int y, int m)
{
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2) {
        int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return leap ? 29 : 28;
    }
    return dm[m - 1];
}

void dlg_calendar(struct rx_ui *ui)
{
    r_err err;
    memset(&err, 0, sizeof(err));
    int fid = rx_func_id("DATE", &err);
    rval v;
    rx_rval_undef(&v);
    int y = 2000, m = 1, day = 1;
    if (fid >= 0 && rx_func_call(ui->ctx, fid, 0, NULL, &v, &err) == R_OK && v.t == RX_T_D) {
        if (v.u.d.y) {
            y = v.u.d.y;
            m = v.u.d.m;
            day = v.u.d.d;
        }
    }
    if (v.t != RX_T_U) {
        rx_rval_free(&v);
    }
    static const char *const mnames[] = { "",     "January", "February", "March",   "April",
                                           "May",  "June",    "July",     "August",  "September",
                                           "October", "November", "December" };
    rx_win *w = dlg_new(ui, "Calendar/Diary", 34, 12);
    rx_dlg *d = w->ud;
    char line[80];
    snprintf(line, sizeof(line), "%s %d", m > 0 && m <= 12 ? mnames[m] : "?", y);
    dlg_add(d, line);
    dlg_add(d, " Su Mo Tu We Th Fr Sa");
    int first = (int)((rx_julian(y, m, 1) + 1) % 7); /* 0 = Sunday */
    int dim = days_in_month(y, m);
    char week[80];
    int col = 0;
    memset(week, ' ', sizeof(week));
    week[sizeof(week) - 1] = '\0';
    for (int i = 0; i < first; i++) {
        week[col * 3] = ' ';
        week[col * 3 + 1] = ' ';
        col++;
    }
    for (int dd = 1; dd <= dim; dd++) {
        char cell[8];
        cell[0] = dd >= 10 ? (char)('0' + dd / 10) : ' ';
        cell[1] = (char)('0' + dd % 10);
        cell[2] = '\0';
        memcpy(week + col * 3, cell, 2);
        col++;
        if (col == 7 || dd == dim) {
            week[col * 3] = '\0';
            dlg_add(d, week);
            col = 0;
            memset(week, ' ', sizeof(week));
        }
    }
    dlg_add(d, "");
    snprintf(line, sizeof(line), "Today: %02d/%02d/%04d", m, day, y);
    dlg_add(d, line);
    dlg_add(d, "Esc closes.");
}

void dlg_ascii(struct rx_ui *ui)
{
    rx_win *w = dlg_new(ui, "ASCII Chart", 40, 20);
    rx_dlg *d = w->ud;
    char head[80];
    snprintf(head, sizeof(head), "    ");
    for (int c = 0; c < 16; c++) {
        char hx[8];
        snprintf(hx, sizeof(hx), "%X ", c);
        strncat(head, hx, sizeof(head) - strlen(head) - 1);
    }
    dlg_add(d, head);
    for (int r = 0; r < 16; r++) {
        char row[80];
        snprintf(row, sizeof(row), "%02X  ", r * 16);
        for (int c = 0; c < 16; c++) {
            uint8_t b = (uint8_t)(r * 16 + c);
            char *utf = NULL;
            size_t un = 0;
            char cell[8];
            if (b < 32 || b == 127) {
                snprintf(cell, sizeof(cell), ". ");
            } else if (cp_to_utf8(437, &b, 1, &utf, &un) == 0 && un < 5) {
                memcpy(cell, utf, un);
                cell[un] = ' ';
                cell[un + 1] = '\0';
                free(utf);
            } else {
                snprintf(cell, sizeof(cell), "? ");
            }
            strncat(row, cell, sizeof(row) - strlen(row) - 1);
        }
        dlg_add(d, row);
    }
    dlg_add(d, "");
    dlg_add(d, "Code page 437 glyphs. Esc closes.");
}
