/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file screenb.c
 * @brief Interactive CREATE/MODIFY SCREEN designer (spec 9.2, M7). The
 *        .SCR/.RSO model, persistence, and .PRG codegen live in util/scrfmt.c;
 *        this file is the modal canvas editor driven through ctx->screen_fn.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui_internal.h"
#include "util/scrfmt.h"

/* ============================ designer ============================ */

static void draw_box(rx_screen *scr, int r1, int c1, int r2, int c2, uint16_t attr)
{
    int y1 = r1 + 1, x1 = c1, y2 = r2 + 1, x2 = c2;
    if (y2 < y1) { int t = y1; y1 = y2; y2 = t; }
    if (x2 < x1) { int t = x1; x1 = x2; x2 = t; }
    if (y1 < 1 || y2 > scr->h - 2 || x1 < 0 || x2 >= scr->w) {
        return;
    }
    scr_fill(scr, x1, y1, 1, 1, 0x250C, attr); /* ┌ */
    scr_fill(scr, x2, y1, 1, 1, 0x2510, attr); /* ┐ */
    scr_fill(scr, x1, y2, 1, 1, 0x2514, attr); /* └ */
    scr_fill(scr, x2, y2, 1, 1, 0x2518, attr); /* ┘ */
    for (int x = x1 + 1; x < x2; x++) {
        scr_fill(scr, x, y1, 1, 1, 0x2500, attr); /* ─ */
        scr_fill(scr, x, y2, 1, 1, 0x2500, attr);
    }
    for (int y = y1 + 1; y < y2; y++) {
        scr_fill(scr, x1, y, 1, 1, 0x2502, attr); /* │ */
        scr_fill(scr, x2, y, 1, 1, 0x2502, attr);
    }
}

static void designer_render(rx_ui *ui, const scr_file *sf, int cy, int cx, int boxmode, int br, int bc,
                            const char *prompt, const getsay *ed)
{
    scr_fill(&ui->scr, 0, 1, ui->scr.w, ui->scr.h - 2, ' ', RXA_DESKTOP);
    for (size_t i = 0; i < sf->nitems; i++) {
        const scr_item *it = &sf->items[i];
        int y = it->row + 1, x = it->col;
        if (y < 1 || y >= ui->scr.h - 1) {
            continue;
        }
        switch (it->type) {
        case SCR_SAY:
            scr_text(&ui->scr, x, y, it->text, RXA_DESKTOP);
            break;
        case SCR_GET: {
            int xx = x;
            if (it->text[0]) {
                scr_text(&ui->scr, xx, y, it->text, RXA_DESKTOP);
                xx += (int)strlen(it->text);
            }
            char buf[80];
            snprintf(buf, sizeof(buf), "<%s>", it->var);
            scr_text(&ui->scr, xx, y, buf, RXA(RXC_BLACK, RXC_WHITE));
            break;
        }
        case SCR_BOX:
            draw_box(&ui->scr, it->row, it->col, it->row2, it->col2, RXA_DESKTOP);
            break;
        case SCR_LINE:
            if (it->row == it->row2) {
                for (int x2 = it->col; x2 <= it->col2; x2++) {
                    scr_fill(&ui->scr, x2, y, 1, 1, 0x2500, RXA_DESKTOP);
                }
            } else {
                for (int y2 = y; y2 <= it->row2 + 1; y2++) {
                    scr_fill(&ui->scr, x, y2, 1, 1, 0x2502, RXA_DESKTOP);
                }
            }
            break;
        default: {
            char buf[300];
            snprintf(buf, sizeof(buf), "[%s]", it->text);
            scr_text(&ui->scr, x, y, buf, RXA(RXC_YELLOW, RXC_BLUE));
            break;
        }
        }
    }
    if (boxmode) {
        draw_box(&ui->scr, br, bc, cy, cx, RXA(RXC_BLACK, RXC_BCYAN));
    }
    /* cursor: inverted cell */
    if (cy + 1 >= 1 && cy + 1 < ui->scr.h - 1 && cx >= 0 && cx < ui->scr.w) {
        rcell *c = &ui->scr.cells[(cy + 1) * ui->scr.w + cx];
        uint32_t ch = c->ch ? c->ch : ' ';
        scr_fill(&ui->scr, cx, cy + 1, 1, 1, ch, RXA(RXC_BLACK, RXC_BCYAN));
    }
    if (prompt) {
        char line[340];
        snprintf(line, sizeof(line), "%s%s", prompt, ed && ed->buf ? ed->buf : "");
        ui_set_msg(ui, "%s", line);
    } else {
        ui_set_msg(ui, "SCREEN %s: arrows move  S=say  G=get  B=box  X=del  ^W=save  Esc=cancel  (%zu items)",
                   sf->name, sf->nitems);
    }
    menubar_render(ui);
    statusbar_render(ui);
    ui->dirty = 1;
}

/* modal line prompt on the message line; returns R_OK with text, 1 = cancelled */
static int designer_prompt(rx_ui *ui, const scr_file *sf, int cy, int cx, int boxmode, int br, int bc,
                           const char *prompt, char *out, size_t outsz)
{
    getsay ed;
    gs_init(&ed, 0);
    int cancelled = 0;
    for (;;) {
        designer_render(ui, sf, cy, cx, boxmode, br, bc, prompt, &ed);
        scr_blit(&ui->scr, ui->tb);
        revent ev;
        memset(&ev, 0, sizeof(ev));
        if (ui->tb->poll(&ev, 250) != R_OK) {
            cancelled = 1;
            break;
        }
        if (ev.kind == EV_EOF) {
            cancelled = 1;
            break;
        }
        if (ev.kind != EV_KEY) {
            continue;
        }
        const rkey *k = &ev.u.k;
        if (k->sym == RXK_ESC) {
            cancelled = 1;
            break;
        }
        if (k->sym == RXK_ENTER) {
            break;
        }
        switch (k->sym) {
        case RXK_BS:
            gs_backspace(&ed);
            break;
        case RXK_DEL:
            gs_delete(&ed);
            break;
        case RXK_LEFT:
            gs_left(&ed);
            break;
        case RXK_RIGHT:
            gs_right(&ed);
            break;
        case RXK_HOME:
            gs_home(&ed);
            break;
        case RXK_END:
            gs_end(&ed);
            break;
        case RXK_CTL('Y'):
            gs_kill_end(&ed);
            break;
        default:
            if (k->cp && k->cp >= 32) {
                gs_insert_cp(&ed, k->cp);
            }
            break;
        }
    }
    if (!cancelled) {
        snprintf(out, outsz, "%s", ed.buf ? ed.buf : "");
    }
    gs_free(&ed);
    return cancelled ? 1 : R_OK;
}

int ui_screen_designer(void *ud, struct rx_stmt *s)
{
    rx_ui *ui = ud;
    rx_ctx *ctx = ui->ctx;
    const char *name = s->nstrs > 0 ? s->strs[0] : "SCREEN";
    int create = s->kind == ST_CREATE_SCREEN;
    char scrpath[600], rsopath[600];
    snprintf(scrpath, sizeof(scrpath), "%s.scr", name);
    snprintf(rsopath, sizeof(rsopath), "%s.rso", name);
    scr_file sfv;
    scr_file *sf = &sfv;
    if (create) {
        scrf_init(sf, name);
    } else {
        int rc = scrf_load(scrpath, sf);
        if (rc != R_OK) {
            rc = scrf_load_rso(rsopath, sf);
        }
        if (rc != R_OK) {
            return R_ERR_IO_NOTFOUND;
        }
    }
    int cy = 2, cx = 2;
    int done = 0, saved = 0;
    int boxmode = 0, br = 0, bc = 0;
    while (!done) {
        designer_render(ui, sf, cy, cx, boxmode, br, bc, NULL, NULL);
        scr_blit(&ui->scr, ui->tb);
        revent ev;
        memset(&ev, 0, sizeof(ev));
        int rc = ui->tb->poll(&ev, 250);
        if (rc != R_OK) {
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
            const rmouse *m = &ev.u.m;
            if (m->kind == RM_PRESS && m->button == 1 && m->y >= 1 && m->y < ui->scr.h - 1) {
                cy = m->y - 1;
                cx = m->x;
            }
            continue;
        }
        if (ev.kind != EV_KEY) {
            continue;
        }
        const rkey *k = &ev.u.k;
        if (k->sym == RXK_ESC) {
            done = 1;
            break;
        }
        if (k->sym == RXK_CTL('W')) {
            int rc2 = scrf_save(scrpath, sf);
            if (rc2 == R_OK) {
                rc2 = scrf_compile(rsopath, sf);
            }
            if (rc2 != R_OK) {
                ui_set_msg(ui, "Save failed (%d)", rc2);
                continue;
            }
            saved = 1;
            done = 1;
            break;
        }
        switch (k->sym) {
        case RXK_UP:
            if (cy > 0) cy--;
            break;
        case RXK_DOWN:
            if (cy < ui->scr.h - 3) cy++;
            break;
        case RXK_LEFT:
            if (cx > 0) cx--;
            break;
        case RXK_RIGHT:
            if (cx < ui->scr.w - 1) cx++;
            break;
        default:
            if (k->cp == 's' || k->cp == 'S') {
                char txt[256];
                if (designer_prompt(ui, sf, cy, cx, boxmode, br, bc, "SAY text: ", txt, sizeof(txt)) == R_OK) {
                    scr_item *it = scrf_add(sf);
                    it->type = SCR_SAY;
                    it->row = cy;
                    it->col = cx;
                    snprintf(it->text, sizeof(it->text), "%s", txt);
                }
            } else if (k->cp == 'g' || k->cp == 'G') {
                char v[64];
                if (designer_prompt(ui, sf, cy, cx, boxmode, br, bc, "GET var: ", v, sizeof(v)) == R_OK) {
                    scr_item *it = scrf_add(sf);
                    it->type = SCR_GET;
                    it->row = cy;
                    it->col = cx;
                    snprintf(it->var, sizeof(it->var), "%s", v);
                    for (char *c = it->var; *c; c++) {
                        *c = (char)toupper((unsigned char)*c);
                    }
                    it->h = 1;
                    it->w = 10;
                }
            } else if (k->cp == 'b' || k->cp == 'B') {
                if (!boxmode) {
                    boxmode = 1;
                    br = cy;
                    bc = cx;
                } else {
                    scr_item *it = scrf_add(sf);
                    it->type = SCR_BOX;
                    it->row = br;
                    it->col = bc;
                    it->row2 = cy;
                    it->col2 = cx;
                    boxmode = 0;
                }
            } else if (k->cp == 'x' || k->cp == 'X') {
                /* delete the topmost item covering the cursor */
                for (size_t i = sf->nitems; i-- > 0;) {
                    scr_item *it = &sf->items[i];
                    int hit = 0;
                    if (it->type == SCR_BOX || it->type == SCR_LINE) {
                        int y1 = it->row, y2 = it->row2, x1 = it->col, x2 = it->col2;
                        if (y2 < y1) { int t = y1; y1 = y2; y2 = t; }
                        if (x2 < x1) { int t = x1; x1 = x2; x2 = t; }
                        hit = (cy == y1 || cy == y2 || (cy > y1 && cy < y2)) &&
                              (cx == x1 || cx == x2 || (cx > x1 && cx < x2));
                    } else {
                        hit = (it->row == cy && cx >= it->col && cx < it->col + (int)strlen(it->text[0] ? it->text : it->var) + 4);
                    }
                    if (hit) {
                        memmove(&sf->items[i], &sf->items[i + 1], (sf->nitems - i - 1) * sizeof(scr_item));
                        sf->nitems--;
                        break;
                    }
                }
            }
            break;
        }
    }
    scrf_free(sf);
    ui->dirty = 1;
    /* restore the desktop (clear designer drawings) */
    scr_damage(&ui->scr, 0, 1, ui->scr.w, ui->scr.h - 2);
    if (saved) {
        ui_set_msg(ui, "Screen %s saved (%s, %s)", name, scrpath, rsopath);
    }
    (void)ctx;
    return R_OK;
}
