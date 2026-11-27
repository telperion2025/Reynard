/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file helpwin.c
 * @brief Help browser (spec 9.7, M9): topics live in a DBF (SET HELP TO
 *        <file.dbf>, default help.dbf) with columns TOPIC C, KEYWORDS M,
 *        BODY M, SEEALSO M. The browser lists topics, filters as you type
 *        (case-insensitive substring over TOPIC+KEYWORDS), Enter opens the
 *        topic body, Esc backs out / closes. ON KEY LABEL F1 overrides the
 *        default F1 dispatch (ui.c handle_key). Falls back to a built-in
 *        contents page when no help DBF is available.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lang/lang.h"
#include "ui/ui_internal.h"
#include "util/util.h"

static const char* const fallback[] = {
    "Reynard Help - Contents (built-in; no help.dbf found)",
    "",
    "  System    About, Macros, Filer, Calculator, Calendar, ASCII, Help",
    "  File      New, Open, Close, Save, Print, Quit",
    "  Edit      Undo, Cut, Copy, Paste, Find, Replace (Command window)",
    "  Database  Setup, Browse, Append/Copy/Sort, Pack, Reindex",
    "  Record    Append, Change, Goto, Locate, Replace, Delete, Recall",
    "  Program   Do, Compile, Generate",
    "  Window    Hide, Clear, Cycle, Command (Ctrl+F2), Zoom",
    "",
    "  Keys: F10 or Alt activates the menubar; arrows navigate;",
    "        Enter selects; Esc backs out. F1 opens this help.",
    "",
    "  Batch mode: reynard script.prg | reynard -c \"? 1+1\" | reynard -",
};

static int ci_contains(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (!nl) {
        return 1;
    }
    for (const char* h = hay; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) {
            return 1;
        }
    }
    return 0;
}

/* one filtered topic row */
typedef struct {
    size_t idx; /* record index in the help DBF */
    char topic[64];
} hrow;

typedef struct {
    hrow* rows;
    size_t nrows, cap;
    char filter[64];
    size_t sel;
    int scroll;
} hmodel;

static void hm_rebuild(rx_ctx* ctx, hmodel* m) {
    m->nrows = 0;
    size_t n = rx_help_count(ctx);
    for (size_t i = 0; i < n; i++) {
        char topic[64];
        if (rx_help_topic(ctx, i, topic, sizeof(topic)) != R_OK) {
            continue;
        }
        if (m->filter[0]) {
            /* case-insensitive substring over TOPIC and KEYWORDS */
            char kw[256];
            int hit = ci_contains(topic, m->filter);
            if (!hit && rx_help_keywords(ctx, i, kw, sizeof(kw)) == R_OK) {
                hit = ci_contains(kw, m->filter);
            }
            if (!hit) {
                continue;
            }
        }
        if (m->nrows == m->cap) {
            size_t nc = m->cap ? m->cap * 2 : 16;
            hrow* nr = realloc(m->rows, nc * sizeof(hrow));
            if (!nr) {
                RX_FATAL("out of memory");
            }
            m->rows = nr;
            m->cap = nc;
        }
        m->rows[m->nrows].idx = i;
        snprintf(m->rows[m->nrows].topic, sizeof(m->rows[m->nrows].topic), "%s", topic);
        m->nrows++;
    }
    if (m->sel >= m->nrows) {
        m->sel = m->nrows ? m->nrows - 1 : 0;
    }
}

/* render one line of text into the screen buffer */
static void hline(rx_ui* ui, int x, int y, int w, const char* s, uint16_t attr, int bold_sel) {
    (void)bold_sel;
    scr_textn_clip(&ui->scr, x, y, s, strlen(s), attr, w);
}

static void help_render(rx_ui* ui, hmodel* m, int show_body, size_t body_idx) {
    int wx = 2, wy = 2;
    int ww = ui->scr.w - 4, wh = ui->scr.h - 5;
    if (ww < 20) {
        ww = 20;
    }
    if (wh < 6) {
        wh = 6;
    }
    scr_fill(&ui->scr, wx, wy, ww, wh, ' ', RXA_DLG);
    win_box(ui, &(rx_win){.x = wx, .y = wy, .w = ww, .h = wh}, RXA_DLG_BORDER, RXA_DLG_TITLE);
    /* title */
    char ttl[128];
    snprintf(ttl, sizeof(ttl), " Reynard Help %s", show_body ? "- Topic" : "- Contents");
    scr_textn_clip(&ui->scr, wx, wy, ttl, strlen(ttl), RXA_DLG_TITLE, ww);
    if (!show_body) {
        /* search line */
        char fl[80];
        snprintf(fl, sizeof(fl), " Search: %s", m->filter);
        scr_textn_clip(&ui->scr, wx + 1, wy + 1, fl, strlen(fl), RXA_DLG, ww - 2);
        int rows = wh - 4;
        for (int r = 0; r < rows; r++) {
            size_t ri = (size_t)r + m->scroll;
            if (ri >= m->nrows) {
                break;
            }
            char line[80];
            snprintf(line, sizeof(line), " %s%s", m->sel == ri ? ">" : " ", m->rows[ri].topic);
            hline(ui, wx + 1, wy + 2 + r, ww - 2, line,
                  m->sel == ri ? RXA(RXC_BLACK, RXC_CYAN) : RXA_DLG, m->sel == ri);
        }
        char hint[160];
        snprintf(hint, sizeof(hint),
                 " %zu topics  type=filter  Enter=view  arrows=move  Esc=close ", m->nrows);
        scr_textn_clip(&ui->scr, wx + 1, wy + wh - 1, hint, strlen(hint), RXA_DLG, ww - 2);
    } else {
        /* body view */
        strbuf body;
        sb_init(&body);
        char topic[64];
        rx_help_topic(ui->ctx, body_idx, topic, sizeof(topic));
        rx_help_body(ui->ctx, body_idx, &body);
        char see[128];
        rx_help_seealso(ui->ctx, body_idx, see, sizeof(see));
        int y = wy + 1;
        char hdr[96];
        snprintf(hdr, sizeof(hdr), " %s", topic);
        scr_textn_clip(&ui->scr, wx + 1, y++, hdr, strlen(hdr), RXA_DLG_TITLE, ww - 2);
        const char* p = body.buf ? body.buf : "";
        while (*p && y < wy + wh - 2) {
            const char* nl = strchr(p, '\n');
            size_t ln = nl ? (size_t)(nl - p) : strlen(p);
            scr_textn_clip(&ui->scr, wx + 1, y, p, ln, RXA_DLG, ww - 2);
            y++;
            p = nl ? nl + 1 : p + ln;
        }
        char sa[160];
        snprintf(sa, sizeof(sa), " See also: %s   (Esc=back)", see);
        scr_textn_clip(&ui->scr, wx + 1, wy + wh - 1, sa, strlen(sa), RXA_DLG, ww - 2);
        sb_free(&body);
    }
    menubar_render(ui);
    statusbar_render(ui);
    ui->dirty = 1;
    scr_blit(&ui->scr, ui->tb);
}

void dlg_help(struct rx_ui* ui) {
    rx_ctx* ctx = ui->ctx;
    /* resolve the help DBF: SET HELP TO file, else help.dbf on cwd/PATH */
    char path[700];
    if (ctx->set.helpfile[0]) {
        snprintf(path, sizeof(path), "%s", ctx->set.helpfile);
    } else {
        rx_resolve_res(ctx, "help", "dbf", path, sizeof(path));
    }
    int have_db = rx_help_open(ctx, path) == R_OK;

    hmodel m;
    memset(&m, 0, sizeof(m));
    if (have_db) {
        hm_rebuild(ctx, &m);
    }
    int show_body = 0;
    size_t body_idx = 0;
    int done = 0;
    while (!done) {
        if (have_db) {
            help_render(ui, &m, show_body, body_idx);
        } else {
            /* fallback static contents */
            int wx = 2, wy = 2, ww = 64, wh = 20;
            if (ww > ui->scr.w - 2) {
                ww = ui->scr.w - 2;
            }
            if (wh > ui->scr.h - 4) {
                wh = ui->scr.h - 4;
            }
            scr_fill(&ui->scr, wx, wy, ww, wh, ' ', RXA_DLG);
            win_box(ui, &(rx_win){.x = wx, .y = wy, .w = ww, .h = wh}, RXA_DLG_BORDER,
                    RXA_DLG_TITLE);
            scr_textn_clip(&ui->scr, wx, wy, " Reynard Help", 13, RXA_DLG_TITLE, ww);
            for (int r = 0; r < wh - 2 && r < (int)(sizeof(fallback) / sizeof(fallback[0])); r++) {
                scr_textn_clip(&ui->scr, wx + 1, wy + 1 + r, fallback[r], strlen(fallback[r]),
                               RXA_DLG, ww - 2);
            }
            menubar_render(ui);
            statusbar_render(ui);
            ui->dirty = 1;
            scr_blit(&ui->scr, ui->tb);
        }
        revent ev;
        memset(&ev, 0, sizeof(ev));
        if (ui->tb->poll(&ev, 250) != R_OK) {
            break;
        }
        if (ev.kind == EV_EOF) {
            break;
        }
        if (ev.kind == EV_RESIZE) {
            int w = ui->scr.w, h = ui->scr.h;
            ui->tb->size(&w, &h);
            scr_resize(&ui->scr, w, h);
            continue;
        }
        if (ev.kind != EV_KEY) {
            continue;
        }
        const rkey* k = &ev.u.k;
        if (k->sym == RXK_ESC) {
            if (show_body) {
                show_body = 0;
            } else {
                done = 1;
            }
            continue;
        }
        if (!have_db) {
            continue;
        }
        if (show_body) {
            continue; /* any other key backs out of the topic */
        }
        if (k->sym == RXK_ENTER && m.nrows > 0) {
            body_idx = m.rows[m.sel].idx;
            show_body = 1;
            continue;
        }
        if (k->sym == RXK_UP && m.sel > 0) {
            m.sel--;
        } else if (k->sym == RXK_DOWN && m.sel + 1 < m.nrows) {
            m.sel++;
        } else if (k->sym == RXK_PGUP && m.sel > 0) {
            m.sel = m.sel > 8 ? m.sel - 8 : 0;
        } else if (k->sym == RXK_PGDN && m.sel + 1 < m.nrows) {
            m.sel = m.sel + 8 < m.nrows - 1 ? m.sel + 8 : m.nrows - 1;
        } else if (k->sym == RXK_DEL || k->sym == 127 || k->sym == 8) {
            size_t l = strlen(m.filter);
            if (l) {
                m.filter[l - 1] = '\0';
            }
            hm_rebuild(ctx, &m);
        } else if (k->sym == RXK_BASE && k->cp >= 32 && k->cp < 127 &&
                   strlen(m.filter) < sizeof(m.filter) - 1) {
            size_t l = strlen(m.filter);
            m.filter[l] = (char)k->cp;
            m.filter[l + 1] = '\0';
            hm_rebuild(ctx, &m);
        }
        /* keep the selection visible */
        int rows = ui->scr.h - 5 - 4;
        if (rows < 1) {
            rows = 1;
        }
        if ((int)m.sel < m.scroll) {
            m.scroll = (int)m.sel;
        }
        if ((int)m.sel >= m.scroll + rows) {
            m.scroll = (int)m.sel - rows + 1;
        }
    }
    if (have_db) {
        rx_help_close(ctx);
    }
    free(m.rows);
    /* clear the desktop area we painted */
    ui->dirty = 1;
}
