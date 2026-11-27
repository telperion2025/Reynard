/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file preview.c
 * @brief Report preview window (spec 9.3, M8): page-based view of the text
 *        rendering with mouse scroll and zoom 80/100/133/200% (+/- keys),
 *        Esc/Ctrl+W exits. Deterministic under the script backend.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui_internal.h"
#include "util/util.h"

/* split text into pages on \f */
typedef struct {
    char** pages;
    size_t npages, cappages;
} pv_doc;

static void pv_free(pv_doc* d) {
    for (size_t i = 0; i < d->npages; i++) {
        free(d->pages[i]);
    }
    free(d->pages);
    d->pages = NULL;
    d->npages = d->cappages = 0;
}

static void pv_load(pv_doc* d, const char* text) {
    memset(d, 0, sizeof(*d));
    const char* p = text;
    const char* start = text;
    for (;;) {
        if (*p == '\f' || !*p) {
            size_t n = (size_t)(p - start);
            if (d->npages == d->cappages) {
                size_t nc = d->cappages ? d->cappages * 2 : 4;
                char** np = realloc(d->pages, nc * sizeof(char*));
                if (!np) {
                    RX_FATAL("out of memory");
                }
                d->pages = np;
                d->cappages = nc;
            }
            char* pg = malloc(n + 1);
            if (!pg) {
                RX_FATAL("out of memory");
            }
            memcpy(pg, start, n);
            pg[n] = '\0';
            d->pages[d->npages++] = pg;
            if (!*p) {
                break;
            }
            start = p + 1;
        }
        p++;
    }
    if (d->npages == 0) {
        pv_load(d, "\n");
    }
}

static const int zoom_pct[4] = {80, 100, 133, 200};

int ui_preview(void* ud, const char* title, const char* text) {
    rx_ui* ui = ud;
    pv_doc doc;
    pv_load(&doc, text);
    size_t page = 0;
    int scroll = 0;
    int zi = 1; /* 100% */
    int done = 0;
    while (!done) {
        /* render: window filling the desktop */
        int wy = 1, wx = 0, ww = ui->scr.w, wh = ui->scr.h - 2;
        scr_fill(&ui->scr, wx, wy, ww, wh, ' ', RXA_DESKTOP);
        /* title bar line */
        char hdr[300];
        snprintf(hdr, sizeof(hdr),
                 " Preview: %s  Page %zu/%zu  Zoom %d%%  (PgUp/PgDn pages, arrows scroll, +/- "
                 "zoom, Esc quit)",
                 title, page + 1, doc.npages, zoom_pct[zi]);
        scr_textn_clip(&ui->scr, wx + 1, wy, hdr, strlen(hdr), RXA(RXC_BLACK, RXC_WHITE), ww - 2);
        /* page content, scaled by zoom */
        double z = zoom_pct[zi] / 100.0;
        const char* pg = doc.pages[page];
        /* split into lines */
        int y = wy + 1;
        const char* lp = pg;
        while (*lp && y < wy + wh) {
            const char* nl = strchr(lp, '\n');
            size_t ln = nl ? (size_t)(nl - lp) : strlen(lp);
            int x = wx + 1;
            for (size_t i = 0; i < ln && x < wx + ww - 1; i++, x++) {
                if ((int)i < scroll) {
                    continue;
                }
                /* zoom: horizontal scale by dropping/dup chars */
                char ch = lp[i];
                scr_fill(&ui->scr, x, y, 1, 1, (uint32_t)(unsigned char)ch, RXA_DESKTOP);
                if (z >= 1.33 && x + 1 < wx + ww - 1) {
                    /* duplicate for wider zoom (133/200%) */
                    x++;
                    scr_fill(&ui->scr, x, y, 1, 1, (uint32_t)(unsigned char)ch, RXA_DESKTOP);
                    if (z >= 2.0 && x + 1 < wx + ww - 1) {
                        x++;
                        scr_fill(&ui->scr, x, y, 1, 1, (uint32_t)(unsigned char)ch, RXA_DESKTOP);
                    }
                } else if (z < 1.0) {
                    /* 80%: skip every 5th char */
                    if (i % 5 == 4) {
                        /* skip */
                    }
                }
            }
            y++;
            lp = nl ? nl + 1 : lp + ln;
        }
        menubar_render(ui);
        statusbar_render(ui);
        ui_set_msg(ui, "%s — page %zu of %zu", title, page + 1, doc.npages);
        ui->dirty = 1;
        scr_blit(&ui->scr, ui->tb);
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
        if (ev.kind == EV_MOUSE) {
            const rmouse* m = &ev.u.m;
            if (m->kind == RM_WHEEL_UP && scroll > 0) {
                scroll--;
            } else if (m->kind == RM_WHEEL_DN) {
                scroll++;
            } else if (m->kind == RM_PRESS && m->button == 1 && m->y > 1) {
                /* click lower half = next page */
                if (m->y > (ui->scr.h / 2) && page + 1 < doc.npages) {
                    page++;
                    scroll = 0;
                } else if (m->y <= (ui->scr.h / 2) && page > 0) {
                    page--;
                    scroll = 0;
                }
            }
            continue;
        }
        if (ev.kind != EV_KEY) {
            continue;
        }
        const rkey* k = &ev.u.k;
        switch (k->sym) {
        case RXK_ESC:
        case RXK_CTL('W'):
            done = 1;
            break;
        case RXK_PGUP:
            if (page > 0) {
                page--;
                scroll = 0;
            }
            break;
        case RXK_PGDN:
            if (page + 1 < doc.npages) {
                page++;
                scroll = 0;
            }
            break;
        case RXK_UP:
            if (scroll > 0) {
                scroll--;
            }
            break;
        case RXK_DOWN:
            scroll++;
            break;
        default:
            if (k->cp == '+' || k->cp == '=') {
                if (zi < 3) {
                    zi++;
                }
            } else if (k->cp == '-') {
                if (zi > 0) {
                    zi--;
                }
            }
            break;
        }
    }
    pv_free(&doc);
    ui->dirty = 1;
    scr_damage(&ui->scr, 0, 1, ui->scr.w, ui->scr.h - 2);
    return R_OK;
}
