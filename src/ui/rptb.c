/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file rptb.c
 * @brief Interactive CREATE/MODIFY REPORT/LABEL designer (spec 9.3/9.4, M8).
 *        Keys: 1..7 pick the band (reports), arrows move the cursor, T text,
 *        F field expression, L line, B box, X delete under cursor, mouse click
 *        moves, Ctrl+W saves .FRM/.RLB, Esc cancels. Same event pattern as the
 *        screen designer (screenb.c).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui_internal.h"
#include "util/rptfmt.h"

static const char* band_keys[] = {NULL, "1", "2", "3", "4", "5", "6", "7"};

static rpt_band* cur_band(rpt_file* rf, int is_label, int sel) {
    if (is_label) {
        rpt_band* b = rptf_band(rf, RB_DETAIL);
        if (!b) {
            b = rptf_band_add(rf, RB_DETAIL, rf->lab_h);
        }
        return b;
    }
    static const int types[8] = {0,         RB_TITLE,       RB_PAGEHEADER, RB_GROUPHEADER,
                                 RB_DETAIL, RB_GROUPFOOTER, RB_PAGEFOOTER, RB_SUMMARY};
    if (sel < 1 || sel > 7) {
        sel = 4;
    }
    rpt_band* b = rptf_band(rf, types[sel]);
    if (!b) {
        b = rptf_band_add(rf, types[sel], 1);
    }
    return b;
}

static void rptb_render(rx_ui* ui, rpt_file* rf, int is_label, int sel, int cy, int cx,
                        const char* prompt, const getsay* ed) {
    scr_fill(&ui->scr, 0, 1, ui->scr.w, ui->scr.h - 2, ' ', RXA_DESKTOP);
    rpt_band* b = cur_band(rf, is_label, sel);
    int maxh = is_label ? rf->lab_h : (rf->page_h > 20 ? 20 : rf->page_h);
    for (size_t i = 0; i < b->nobjs; i++) {
        rpt_obj* o = &b->objs[i];
        int y = o->row + 1, x = o->col;
        if (y < 1 || y >= ui->scr.h - 1) {
            continue;
        }
        switch (o->type) {
        case RO_TEXT:
            scr_text(&ui->scr, x, y, o->text, RXA_DESKTOP);
            break;
        case RO_FIELD: {
            char buf[300];
            snprintf(buf, sizeof(buf), "{%s}", o->text);
            scr_text(&ui->scr, x, y, buf, RXA(RXC_BLACK, RXC_WHITE));
            break;
        }
        case RO_LINE:
            for (int x2 = o->col; x2 <= o->col2 && x2 < ui->scr.w; x2++) {
                scr_fill(&ui->scr, x2, y, 1, 1, '-', RXA_DESKTOP);
            }
            break;
        case RO_BOX:
            for (int x2 = o->col; x2 <= o->col2 && x2 < ui->scr.w; x2++) {
                scr_fill(&ui->scr, x2, y, 1, 1, '-', RXA_DESKTOP);
                scr_fill(&ui->scr, x2, o->row2 + 1, 1, 1, '-', RXA_DESKTOP);
            }
            for (int y2 = o->row + 1; y2 <= o->row2 + 1 && y2 < ui->scr.h - 1; y2++) {
                scr_fill(&ui->scr, o->col, y2, 1, 1, '|', RXA_DESKTOP);
                if (o->col2 < ui->scr.w) {
                    scr_fill(&ui->scr, o->col2, y2, 1, 1, '|', RXA_DESKTOP);
                }
            }
            break;
        }
    }
    (void)maxh;
    if (cy + 1 >= 1 && cy + 1 < ui->scr.h - 1 && cx >= 0 && cx < ui->scr.w) {
        rcell* c = &ui->scr.cells[(cy + 1) * ui->scr.w + cx];
        uint32_t ch = c->ch ? c->ch : ' ';
        scr_fill(&ui->scr, cx, cy + 1, 1, 1, ch, RXA(RXC_BLACK, RXC_BCYAN));
    }
    if (prompt) {
        char line[340];
        snprintf(line, sizeof(line), "%s%s", prompt, ed && ed->buf ? ed->buf : "");
        ui_set_msg(ui, "%s", line);
    } else {
        ui_set_msg(ui,
                   "%s %s: band %s  arrows move  T=text F=field L=line B=box X=del 1-7=band "
                   "^W=save Esc=cancel",
                   is_label ? "LABEL" : "REPORT", rf->name, rptf_band_name(b->type));
    }
    menubar_render(ui);
    statusbar_render(ui);
    ui->dirty = 1;
}

static int rptb_prompt(rx_ui* ui, rpt_file* rf, int is_label, int sel, int cy, int cx,
                       const char* prompt, char* out, size_t outsz) {
    getsay ed;
    gs_init(&ed, 0);
    int cancelled = 0;
    for (;;) {
        rptb_render(ui, rf, is_label, sel, cy, cx, prompt, &ed);
        scr_blit(&ui->scr, ui->tb);
        revent ev;
        memset(&ev, 0, sizeof(ev));
        if (ui->tb->poll(&ev, 250) != R_OK || ev.kind == EV_EOF) {
            cancelled = 1;
            break;
        }
        if (ev.kind != EV_KEY) {
            continue;
        }
        const rkey* k = &ev.u.k;
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

int ui_rptb_designer(void* ud, struct rx_stmt* s) {
    rx_ui* ui = ud;
    const char* name = s->nstrs > 0 ? s->strs[0] : "REPORT";
    int is_label = (s->kind == ST_CREATE_LABEL || s->kind == ST_MODIFY_LABEL);
    int create = (s->kind == ST_CREATE_REPORT || s->kind == ST_CREATE_LABEL);
    char path[600];
    snprintf(path, sizeof(path), "%s.%s", name, is_label ? "rlb" : "frm");
    rpt_file rf;
    if (create) {
        rptf_init(&rf, name, is_label);
        if (!is_label) {
            rptf_band_add(&rf, RB_TITLE, 1);
            rptf_band_add(&rf, RB_PAGEHEADER, 1);
            rptf_band_add(&rf, RB_DETAIL, 1);
            rptf_band_add(&rf, RB_PAGEFOOTER, 1);
        } else {
            rptf_band_add(&rf, RB_DETAIL, rf.lab_h);
        }
    } else {
        if (rptf_load(path, &rf) != R_OK) {
            rptf_free(&rf);
            return R_ERR_IO_NOTFOUND;
        }
    }
    int sel = is_label ? 4 : 4;
    int cy = 1, cx = 1;
    int done = 0, saved = 0;
    int boxmode = 0, br = 0, bc = 0;
    while (!done) {
        rptb_render(ui, &rf, is_label, sel, cy, cx, NULL, NULL);
        scr_blit(&ui->scr, ui->tb);
        revent ev;
        memset(&ev, 0, sizeof(ev));
        int rc = ui->tb->poll(&ev, 250);
        if (rc != R_OK || ev.kind == EV_EOF) {
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
            if (m->kind == RM_PRESS && m->button == 1 && m->y >= 1 && m->y < ui->scr.h - 1) {
                cy = m->y - 1;
                cx = m->x;
            }
            continue;
        }
        if (ev.kind != EV_KEY) {
            continue;
        }
        const rkey* k = &ev.u.k;
        if (k->sym == RXK_ESC) {
            done = 1;
            break;
        }
        if (k->sym == RXK_CTL('W')) {
            if (rptf_save(path, &rf) == R_OK) {
                saved = 1;
                done = 1;
            }
            break;
        }
        switch (k->sym) {
        case RXK_UP:
            if (cy > 0)
                cy--;
            break;
        case RXK_DOWN:
            if (cy < ui->scr.h - 3)
                cy++;
            break;
        case RXK_LEFT:
            if (cx > 0)
                cx--;
            break;
        case RXK_RIGHT:
            if (cx < ui->scr.w - 1)
                cx++;
            break;
        default:
            if (k->cp >= '1' && k->cp <= '7' && !is_label) {
                sel = k->cp - '0';
            } else if (k->cp == 't' || k->cp == 'T') {
                char txt[256];
                if (rptb_prompt(ui, &rf, is_label, sel, cy, cx, "Text: ", txt, sizeof(txt)) ==
                    R_OK) {
                    rpt_obj* o = rptf_obj_add(cur_band(&rf, is_label, sel));
                    o->type = RO_TEXT;
                    o->row = cy;
                    o->col = cx;
                    snprintf(o->text, sizeof(o->text), "%.255s", txt);
                }
            } else if (k->cp == 'f' || k->cp == 'F') {
                char ex[256];
                if (rptb_prompt(ui, &rf, is_label, sel, cy, cx, "Field expr: ", ex, sizeof(ex)) ==
                    R_OK) {
                    rpt_obj* o = rptf_obj_add(cur_band(&rf, is_label, sel));
                    o->type = RO_FIELD;
                    o->row = cy;
                    o->col = cx;
                    snprintf(o->text, sizeof(o->text), "%.255s", ex);
                }
            } else if (k->cp == 'l' || k->cp == 'L') {
                rpt_obj* o = rptf_obj_add(cur_band(&rf, is_label, sel));
                o->type = RO_LINE;
                o->row = cy;
                o->col = cx;
                o->row2 = cy;
                o->col2 = cx + 9;
            } else if (k->cp == 'b' || k->cp == 'B') {
                if (!boxmode) {
                    boxmode = 1;
                    br = cy;
                    bc = cx;
                } else {
                    rpt_obj* o = rptf_obj_add(cur_band(&rf, is_label, sel));
                    o->type = RO_BOX;
                    o->row = br;
                    o->col = bc;
                    o->row2 = cy;
                    o->col2 = cx;
                    boxmode = 0;
                }
            } else if (k->cp == 'x' || k->cp == 'X') {
                rpt_band* b = cur_band(&rf, is_label, sel);
                for (size_t i = b->nobjs; i-- > 0;) {
                    rpt_obj* o = &b->objs[i];
                    if (o->row == cy && cx >= o->col && cx < o->col + (int)strlen(o->text) + 3) {
                        memmove(&b->objs[i], &b->objs[i + 1], (b->nobjs - i - 1) * sizeof(rpt_obj));
                        b->nobjs--;
                        break;
                    }
                }
            }
            break;
        }
    }
    rptf_free(&rf);
    ui->dirty = 1;
    scr_damage(&ui->scr, 0, 1, ui->scr.w, ui->scr.h - 2);
    if (saved) {
        ui_set_msg(ui, "Form saved: %s", path);
    }
    (void)band_keys;
    return R_OK;
}
