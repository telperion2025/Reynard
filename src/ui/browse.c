/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file browse.c
 * @brief BROWSE / CHANGE grid window (spec §9.1, M4). A full-workspace window
 *        on the uniform pane stack, bound to one work area. Grid navigation,
 *        in-cell editing through the shared getsay core, :V validation on
 *        exit, Ctrl+T delete-mark, Ctrl+N append, FREEZE / PARTITION /
 *        LEDIT|REDIT, and mouse (click-to-position, wheel scroll, header drag
 *        reorder, border drag resize, double-click border size-to-widest,
 *        drag selection). Record edits take a record lock in shared mode
 *        (§8.5). Rendering touches only the visible page, so scrolling a
 *        100k-record table stays well under the §11.3 16ms/keystroke gate.
 */
#include "ui/ui_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reynard/dbf.h" /* public data-engine API (frozen at M6) */

#define BROWSE_MAXCOLS 64
#define BROWSE_MINW 3
#define BROWSE_MAXW 60

typedef struct rx_browse {
    rx_win w;
    rx_ctx *ctx;
    int area;      /**< work area this grid is bound to */
    rx_dbf *dbf;   /**< refreshed from the area at each render */
    rx_bcol *cols; /**< stolen from the BROWSE stmt (owns exprs) */
    size_t ncols;
    int colw[BROWSE_MAXCOLS];
    int colx[BROWSE_MAXCOLS]; /**< last rendered left edge per column */
    int append_mode, noappend, nodelete, nomodify;
    int ledit, redit;
    int partition;   /**< left-pane width in chars, 0 = none */
    int part_n;      /**< columns in the left pane */
    int part_x;      /**< right pane left edge */
    int lock_n;
    int width_cap;   /**< WIDTH n */
    int freeze_col;  /**< -1 or column index locked editable by FREEZE */
    rx_expr *when;   /**< WHEN clause (stolen) */
    int when_ok;
    rx_expr *valid;  /**< record-level VALID (stolen) */
    char verr[160];
    uint32_t cur;    /**< current record (1-based) */
    int col;         /**< cursor column */
    uint32_t top;    /**< first visible record */
    int hscroll;     /**< right-pane horizontal scroll (chars) */
    int editing;     /**< column being edited, -1 none */
    getsay ed;
    /* geometry cached at render for mouse hit-testing */
    int grid_x, hdr_y, rows_y, visrows;
    /* mouse drag state */
    int drag;        /**< 0 none, 1 header reorder, 2 col resize, 3 cell select */
    int drag_col;
    int sel_ar, sel_ac; /**< selection anchor (record, col) */
} rx_browse;

/* ---- clause helpers ---- */

static int clause_flag(const rx_stmt *s, const char *key)
{
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, key) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *clause_word(const rx_stmt *s, const char *key)
{
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, key) == 0) {
            return s->clauses[i].word;
        }
    }
    return NULL;
}

/* ---- value formatting / parsing ---- */

static void browse_cell_text(rx_ctx *ctx, rx_dbf *dbf, const rx_bcol *c, char *out, size_t outsz)
{
    out[0] = '\0';
    if (!c->is_field) {
        rval v;
        rx_rval_undef(&v);
        if (rx_eval(ctx, c->e, &v, &ctx->err) == R_OK) {
            strbuf sb;
            sb_init(&sb);
            rx_vm_display(ctx, &v, &sb);
            snprintf(out, outsz, "%s", sb.buf ? sb.buf : "");
            sb_free(&sb);
            rx_rval_free(&v);
        } else {
            snprintf(out, outsz, "?");
        }
        return;
    }
    rval v;
    rx_rval_undef(&v);
    if (rx_dbf_get(dbf, c->fldidx, &v) != R_OK) {
        snprintf(out, outsz, "?");
        return;
    }
    rx_fielddef fd;
    rx_dbf_field_def(dbf, c->fldidx, &fd);
    switch (fd.type) {
    case RFT_C:
        snprintf(out, outsz, "%s", v.t == RX_T_C && v.u.c.utf8 ? v.u.c.utf8 : "");
        break;
    case RFT_N:
    case RFT_F: {
        char num[64];
        rx_format_num(num, sizeof(num), v.u.n.d, fd.dec, 1);
        snprintf(out, outsz, "%s", num);
        break;
    }
    case RFT_I:
        snprintf(out, outsz, "%d", v.u.i);
        break;
    case RFT_D:
        if (v.u.d.y == 0) {
            snprintf(out, outsz, "%s", ctx->set.century ? "  /  /    " : "  /  /  ");
        } else if (ctx->set.century) {
            snprintf(out, outsz, "%02d/%02d/%04d", v.u.d.m, v.u.d.d, v.u.d.y);
        } else {
            snprintf(out, outsz, "%02d/%02d/%02d", v.u.d.m, v.u.d.d, v.u.d.y % 100);
        }
        break;
    case RFT_L:
        snprintf(out, outsz, "%c", v.u.l == L_T ? 'T' : v.u.l == L_F ? 'F' : '?');
        break;
    case RFT_M:
        snprintf(out, outsz, "Memo");
        break;
    default: {
        strbuf sb;
        sb_init(&sb);
        rx_vm_display(ctx, &v, &sb);
        snprintf(out, outsz, "%s", sb.buf ? sb.buf : "");
        sb_free(&sb);
        break;
    }
    }
    rx_rval_free(&v);
}

/* parse editor text into an rval per the field type; 1 ok, 0 invalid input */
static int browse_parse_value(rx_ctx *ctx, rx_dbf *dbf, size_t fldidx, const char *txt, rval *out)
{
    rx_fielddef fd;
    rx_dbf_field_def(dbf, fldidx, &fd);
    rx_rval_undef(out);
    switch (fd.type) {
    case RFT_C:
        out->t = RX_T_C;
        out->u.c.n = strlen(txt);
        out->u.c.utf8 = rx_strdup(txt);
        return 1;
    case RFT_N:
    case RFT_F: {
        char *end = NULL;
        double d = strtod(txt, &end);
        if (end == txt || (end && *end != '\0')) {
            return 0;
        }
        out->t = RX_T_N;
        out->u.n.d = d;
        out->u.n.dec = fd.dec;
        return 1;
    }
    case RFT_I: {
        char *end = NULL;
        long v = strtol(txt, &end, 10);
        if (end == txt || (end && *end != '\0')) {
            return 0;
        }
        out->t = RX_T_I;
        out->u.i = (int32_t)v;
        return 1;
    }
    case RFT_D: {
        r_err err;
        memset(&err, 0, sizeof(err));
        int fid = rx_func_id("CTOD", &err);
        if (fid < 0) {
            return 0;
        }
        rval av;
        rx_rval_undef(&av);
        av.t = RX_T_C;
        av.u.c.n = strlen(txt);
        av.u.c.utf8 = (char *)txt; /* borrowed for the call */
        rval v;
        rx_rval_undef(&v);
        int rc = rx_func_call(ctx, fid, 1, &av, &v, &err);
        if (rc == R_OK && v.t == RX_T_D) {
            *out = v;
            return 1;
        }
        rx_rval_free(&v);
        return 0;
    }
    case RFT_L: {
        char ch = (char)toupper((unsigned char)txt[0]);
        if (txt[0] == '\0') {
            return 0;
        }
        if (ch == 'T' || ch == 'Y') {
            out->t = RX_T_L;
            out->u.l = L_T;
            return 1;
        }
        if (ch == 'F' || ch == 'N') {
            out->t = RX_T_L;
            out->u.l = L_F;
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

/* ---- navigation ---- */

static void browse_eval_when(rx_browse *b)
{
    b->when_ok = 1;
    if (!b->when) {
        return;
    }
    rx_wa_select(b->area);
    rval v;
    rx_rval_undef(&v);
    if (rx_eval(b->ctx, b->when, &v, &b->ctx->err) == R_OK) {
        b->when_ok = (v.t == RX_T_L && v.u.l == L_T);
    }
    rx_rval_free(&v);
}

static void browse_goto(rx_ui *ui, rx_browse *b, uint32_t recno)
{
    uint32_t n;
    rx_dbf_recount(b->dbf, &n);
    if (n == 0) {
        recno = 1;
    } else {
        if (recno < 1) {
            recno = 1;
        }
        if (recno > n) {
            recno = n;
        }
    }
    rx_wa_select(b->area); /* the browse's area is the current one while focused */
    rx_nav_goto(b->ctx, recno);
    uint32_t cur;
    rx_dbf_recno(b->dbf, &cur);
    b->cur = cur ? cur : 1;
    browse_eval_when(b);
    if (b->cur < b->top) {
        b->top = b->cur;
    }
    if (b->visrows > 0 && b->cur >= b->top + (uint32_t)b->visrows) {
        b->top = b->cur - (uint32_t)b->visrows + 1;
    }
    ui->dirty = 1;
}

static void browse_move(rx_ui *ui, rx_browse *b, int32_t delta)
{
    uint32_t n;
    rx_dbf_recount(b->dbf, &n);
    if (n == 0) {
        return;
    }
    int32_t r = (int32_t)b->cur + delta;
    if (b->ctx->set.deleted) {
        /* SET DELETED ON: skip marked records while moving */
        while (r >= 1 && r <= (int32_t)n) {
            rx_wa_select(b->area);
            rx_nav_goto(b->ctx, (uint32_t)r);
            int del;
            rx_dbf_deleted(b->dbf, &del);
            if (!del) {
                break;
            }
            r += delta > 0 ? 1 : -1;
        }
    }
    if (r < 1) {
        r = 1;
    }
    if (r > (int32_t)n) {
        r = (int32_t)n;
    }
    browse_goto(ui, b, (uint32_t)r);
}

static void browse_move_col(rx_ui *ui, rx_browse *b, int delta)
{
    int c = b->col + delta;
    if (c < 0) {
        c = 0;
    }
    if (c >= (int)b->ncols) {
        c = (int)b->ncols - 1;
    }
    b->col = c;
    /* keep the cursor column visible in the right pane */
    if (b->partition > 0 && c >= b->part_n) {
        int x = b->colx[c] - b->hscroll;
        if (x < 0) {
            b->hscroll += x;
        }
        if (x + b->colw[c] > b->w.w - 2 - (b->part_x - b->grid_x - 1)) {
            b->hscroll += x + b->colw[c] - (b->w.w - 2 - (b->part_x - b->grid_x - 1));
        }
    }
    ui->dirty = 1;
}

/* ---- editing ---- */

static int browse_editable(rx_browse *b, int col)
{
    rx_bcol *c = &b->cols[col];
    if (!c->is_field || c->rflag || b->nomodify) {
        return 0;
    }
    if (b->freeze_col >= 0 && col != b->freeze_col) {
        return 0;
    }
    if (b->when && !b->when_ok) {
        return 0;
    }
    rx_fielddef fd;
    rx_dbf_field_def(b->dbf, c->fldidx, &fd);
    if (fd.type == RFT_M || fd.type == RFT_G || fd.type == RFT_P) {
        return 0; /* memo/general cells are read-only in browse v1 */
    }
    return 1;
}

static void browse_begin_edit(rx_ui *ui, rx_browse *b, uint32_t cp)
{
    gs_clear(&b->ed);
    if (cp >= 32) {
        gs_insert_cp(&b->ed, cp); /* typing replaces the cell content (FoxPro) */
    }
    b->editing = b->col;
    ui->dirty = 1;
}

/* commit the editor into the field; 1 = leave edit mode, 0 = stay editing */
static int browse_commit(rx_ui *ui, rx_browse *b)
{
    int col = b->editing;
    rx_bcol *c = &b->cols[col];
    const char *txt = b->ed.buf ? b->ed.buf : "";
    rval nv;
    if (!browse_parse_value(b->ctx, b->dbf, c->fldidx, txt, &nv)) {
        ui_set_msg(ui, "Invalid value for %s", c->header);
        return 0; /* stay in the editor */
    }
    if (!rx_dbf_is_exclusive(b->dbf)) {
        int rc = rx_rlock(b->dbf, b->cur);
        if (rc != R_OK) {
            ui_set_msg(ui, "Record is in use by another process");
            rx_rval_free(&nv);
            b->editing = -1;
            ui->dirty = 1;
            return 1;
        }
    }
    rval old;
    rx_rval_undef(&old);
    rx_dbf_get(b->dbf, c->fldidx, &old);
    rx_dbf_put(b->dbf, c->fldidx, &nv);
    int bad = 0;
    const char *vmsg = NULL;
    if (c->valid) { /* field :V — the field already holds the new value */
        rval vr;
        rx_rval_undef(&vr);
        if (rx_eval(b->ctx, c->valid, &vr, &b->ctx->err) != R_OK || vr.t != RX_T_L ||
            vr.u.l != L_T) {
            bad = 1;
            vmsg = c->verr[0] ? c->verr : "VALID clause failed";
        }
        rx_rval_free(&vr);
    }
    if (!bad && b->valid) { /* record-level VALID */
        rval vr;
        rx_rval_undef(&vr);
        if (rx_eval(b->ctx, b->valid, &vr, &b->ctx->err) != R_OK || vr.t != RX_T_L ||
            vr.u.l != L_T) {
            bad = 1;
            vmsg = b->verr[0] ? b->verr : "VALID clause failed";
        }
        rx_rval_free(&vr);
    }
    if (bad) {
        rx_dbf_put(b->dbf, c->fldidx, &old); /* revert */
        rx_flush(b->dbf);
        ui_set_msg(ui, "%s", vmsg);
    } else {
        rx_flush(b->dbf);
    }
    rx_rval_free(&old);
    rx_rval_free(&nv);
    b->editing = -1;
    ui->dirty = 1;
    return 1;
}

static void browse_toggle_delete(rx_ui *ui, rx_browse *b)
{
    if (b->nodelete) {
        return;
    }
    if (!rx_dbf_is_exclusive(b->dbf)) {
        int rc = rx_rlock(b->dbf, b->cur);
        if (rc != R_OK) {
            ui_set_msg(ui, "Record is in use by another process");
            return;
        }
    }
    int del;
    rx_dbf_deleted(b->dbf, &del);
    rx_dbf_set_deleted(b->dbf, !del);
    rx_flush(b->dbf);
    ui->dirty = 1;
}

static void browse_append(rx_ui *ui, rx_browse *b)
{
    if (b->noappend) {
        return;
    }
    int rc = R_OK;
    if (!rx_dbf_is_exclusive(b->dbf)) {
        rc = rx_flock(b->dbf);
    }
    uint32_t recno = 0;
    if (rc == R_OK) {
        rc = rx_dbf_append(b->dbf, &recno);
        if (!rx_dbf_is_exclusive(b->dbf)) {
            rx_funlock(b->dbf);
        }
    }
    if (rc != R_OK) {
        ui_set_msg(ui, "Record is in use by another process");
        return;
    }
    rx_flush(b->dbf);
    browse_goto(ui, b, recno);
    b->col = 0;
    while (b->col < (int)b->ncols && !browse_editable(b, b->col)) {
        b->col++;
    }
    browse_begin_edit(ui, b, 0); /* new record starts in edit mode */
}

/* ---- layout + render ---- */

static int browse_auto_width(rx_browse *b, size_t i)
{
    rx_bcol *c = &b->cols[i];
    int w;
    if (c->is_field) {
        rx_fielddef fd;
        rx_dbf_field_def(b->dbf, c->fldidx, &fd);
        switch (fd.type) {
        case RFT_D:
            w = b->ctx->set.century ? 10 : 8;
            break;
        case RFT_L:
            w = 1;
            break;
        case RFT_M:
            w = 4;
            break;
        default:
            w = fd.len;
            break;
        }
    } else {
        w = 10; /* computed column default */
    }
    int hw = (int)strlen(c->header);
    if (hw > w) {
        w = hw;
    }
    if (w < BROWSE_MINW) {
        w = BROWSE_MINW;
    }
    int cap = b->width_cap > 0 ? b->width_cap : BROWSE_MAXW;
    if (w > cap) {
        w = cap;
    }
    return w;
}

static void browse_layout(rx_ui *ui, rx_browse *b)
{
    rx_win *w = &b->w;
    b->grid_x = w->x + 1;
    b->hdr_y = w->y + 1;
    b->rows_y = w->y + 3;
    b->visrows = w->h - 4;
    if (b->visrows < 1) {
        b->visrows = 1;
    }
    for (size_t i = 0; i < b->ncols && i < BROWSE_MAXCOLS; i++) {
        b->colw[i] = b->cols[i].width > 0 ? b->cols[i].width : browse_auto_width(b, i);
        if (b->colw[i] < BROWSE_MINW) {
            b->colw[i] = BROWSE_MINW;
        }
        if (b->colw[i] > BROWSE_MAXW) {
            b->colw[i] = BROWSE_MAXW;
        }
    }
    /* PARTITION: split the column list into a fixed left pane and a
       horizontally scrollable right pane */
    b->part_n = 0;
    b->part_x = 0;
    if (b->partition > 0 && b->ncols > 1) {
        int x = 1; /* marker column */
        size_t n = 0;
        while (n < b->ncols && n < BROWSE_MAXCOLS && x + b->colw[n] <= b->partition) {
            x += b->colw[n] + 1;
            n++;
        }
        if (n < 1) {
            n = 1;
        }
        if (n >= b->ncols) {
            n = b->ncols - 1;
        }
        b->part_n = (int)n;
        b->part_x = b->grid_x + 1 + b->partition;
    }
    int x = b->grid_x + 1;
    int rx = 0;
    for (size_t i = 0; i < b->ncols && i < BROWSE_MAXCOLS; i++) {
        if (b->part_n > 0 && (int)i >= b->part_n) {
            b->colx[i] = b->part_x + rx - b->hscroll;
            rx += b->colw[i] + 1;
        } else {
            b->colx[i] = x;
            x += b->colw[i] + 1;
        }
    }
    (void)ui;
}

static uint16_t inv_attr(uint16_t a)
{
    return (uint16_t)(((a & 0x0F) << 4) | ((a >> 4) & 0x0F));
}

static int in_selection(rx_browse *b, uint32_t rec, int col)
{
    if (b->drag != 3 && b->sel_ar == 0) {
        return 0;
    }
    uint32_t r0 = (uint32_t)(b->sel_ar < (int)b->cur ? b->sel_ar : (int)b->cur);
    uint32_t r1 = (uint32_t)(b->sel_ar < (int)b->cur ? (int)b->cur : b->sel_ar);
    int c0 = b->sel_ac < b->col ? b->sel_ac : b->col;
    int c1 = b->sel_ac < b->col ? b->col : b->sel_ac;
    return rec >= r0 && rec <= r1 && col >= c0 && col <= c1;
}

static void browse_render(struct rx_ui *ui, rx_win *w)
{
    rx_browse *b = w->ud;
    if (!rx_wa_used(b->area)) {
        w->want_close = 1; /* CLOSE closed the underlying table */
        return;
    }
    rx_wa_dbf(b->area, &b->dbf);
    browse_layout(ui, b);
    win_box(ui, w, RXA_BROWSE_BORDER, RXA_BROWSE_TITLE);
    int iw = w->w - 2, ih = w->h - 2;
    if (iw <= 0 || ih <= 0) {
        return;
    }
    scr_fill(&ui->scr, w->x + 1, w->y + 1, iw, ih, ' ', RXA_BROWSE);

    uint32_t nrec;
    rx_dbf_recount(b->dbf, &nrec);
    if (b->top < 1) {
        b->top = 1;
    }
    if (b->top > nrec && nrec > 0) {
        b->top = nrec;
    }

    /* header row + separator */
    int sep_y = b->hdr_y + 1;
    scr_fill(&ui->scr, w->x + 1, b->hdr_y, iw, 1, ' ', RXA_BROWSE_HDR);
    scr_fill(&ui->scr, w->x + 1, sep_y, iw, 1, 0x2500, RXA_BROWSE_HDR);
    for (size_t i = 0; i < b->ncols && i < BROWSE_MAXCOLS; i++) {
        int cx = b->colx[i];
        if (cx >= w->x + iw) {
            continue;
        }
        uint16_t a = (int)i == b->col ? RXA_BROWSE_HDR_ACT : RXA_BROWSE_HDR;
        scr_textn_clip(&ui->scr, cx, b->hdr_y, b->cols[i].header, strlen(b->cols[i].header), a,
                       b->colw[i]);
        if (cx + b->colw[i] < w->x + iw) {
            scr_put(&ui->scr, cx + b->colw[i], b->hdr_y, 0x2502, RXA_BROWSE_HDR);
            scr_put(&ui->scr, cx + b->colw[i], sep_y, 0x253C, RXA_BROWSE_HDR);
        }
    }

    /* record rows */
    for (int r = 0; r < b->visrows; r++) {
        uint32_t rec = b->top + (uint32_t)r;
        if (rec > nrec) {
            break;
        }
        int ry = b->rows_y + r;
        rx_wa_select(b->area);
        rx_dbf_goto(b->dbf, rec);
        int del;
        rx_dbf_deleted(b->dbf, &del);
        char mark = del ? '*' : (rec == b->cur ? '>' : ' ');
        scr_put(&ui->scr, b->grid_x, ry, (uint32_t)(unsigned char)mark, RXA_BROWSE);
        for (size_t i = 0; i < b->ncols && i < BROWSE_MAXCOLS; i++) {
            int cx = b->colx[i];
            if (cx >= w->x + iw) {
                continue;
            }
            char txt[600];
            if ((int)i == b->editing && rec == b->cur) {
                snprintf(txt, sizeof(txt), "%s", b->ed.buf ? b->ed.buf : "");
            } else {
                browse_cell_text(b->ctx, b->dbf, &b->cols[i], txt, sizeof(txt));
            }
            uint16_t a = RXA_BROWSE;
            if (rec == b->cur && (int)i == b->col) {
                a = inv_attr(RXA_BROWSE);
            } else if (in_selection(b, rec, (int)i)) {
                a = RXA_BROWSE_SEL;
            }
            scr_fill(&ui->scr, cx, ry, b->colw[i], 1, ' ', a);
            scr_textn_clip(&ui->scr, cx, ry, txt, strlen(txt), a, b->colw[i]);
            if (cx + b->colw[i] < w->x + iw) {
                scr_put(&ui->scr, cx + b->colw[i], ry, 0x2502, RXA_BROWSE);
            }
        }
    }
    /* restore the work area pointer to the cursor record */
    rx_wa_select(b->area);
    rx_nav_goto(b->ctx, b->cur);
}

/* ---- input ---- */

static int browse_key(struct rx_ui *ui, rx_win *w, const rkey *k)
{
    rx_browse *b = w->ud;
    if (b->editing >= 0) {
        switch (k->sym) {
        case RXK_ENTER:
        case RXK_TAB:
        case RXK_UP:
        case RXK_DOWN:
            /* commit, then move (arrows within the text stay in the editor) */
            if (!browse_commit(ui, b)) {
                return 1; /* invalid input: stay in the editor */
            }
            if (k->sym == RXK_ENTER || k->sym == RXK_DOWN) {
                browse_move(ui, b, 1);
            } else if (k->sym == RXK_UP) {
                browse_move(ui, b, -1);
            } else {
                browse_move_col(ui, b, 1);
            }
            return 1;
        case RXK_ESC:
            if (ui->ctx->set.escape) {
                b->editing = -1;
                ui->dirty = 1;
            }
            return 1;
        case RXK_BS:
            gs_backspace(&b->ed);
            ui->dirty = 1;
            return 1;
        case RXK_DEL:
            gs_delete(&b->ed);
            ui->dirty = 1;
            return 1;
        case RXK_LEFT:
            gs_left(&b->ed);
            ui->dirty = 1;
            return 1;
        case RXK_RIGHT:
            gs_right(&b->ed);
            ui->dirty = 1;
            return 1;
        case RXK_HOME:
            gs_home(&b->ed);
            ui->dirty = 1;
            return 1;
        case RXK_END:
            gs_end(&b->ed);
            ui->dirty = 1;
            return 1;
        case RXK_CTL('Y'):
            gs_kill_end(&b->ed);
            ui->dirty = 1;
            return 1;
        default:
            if (k->sym == RXK_BASE && k->cp >= 32) {
                gs_insert_cp(&b->ed, k->cp);
                ui->dirty = 1;
            }
            return 1;
        }
    }
    switch (k->sym) {
    case RXK_ESC:
    case RXK_CTL('W'):
        w->want_close = 1;
        ui->dirty = 1;
        return 1;
    case RXK_UP:
        browse_move(ui, b, -1);
        return 1;
    case RXK_DOWN:
    case RXK_ENTER:
        browse_move(ui, b, 1);
        return 1;
    case RXK_LEFT:
        browse_move_col(ui, b, -1);
        return 1;
    case RXK_RIGHT:
        browse_move_col(ui, b, 1);
        return 1;
    case RXK_TAB:
        if (b->col + 1 >= (int)b->ncols) {
            b->col = 0;
            browse_move(ui, b, 1);
        } else {
            browse_move_col(ui, b, 1);
        }
        return 1;
    case RXK_BACKTAB:
        if (b->col - 1 < 0) {
            b->col = (int)b->ncols - 1;
            browse_move(ui, b, -1);
        } else {
            browse_move_col(ui, b, -1);
        }
        return 1;
    case RXK_PGUP:
        browse_move(ui, b, -b->visrows);
        return 1;
    case RXK_PGDN:
        browse_move(ui, b, b->visrows);
        return 1;
    case RXK_HOME:
        b->col = 0;
        ui->dirty = 1;
        return 1;
    case RXK_END:
        b->col = (int)b->ncols - 1;
        ui->dirty = 1;
        return 1;
    case RXK_CTL('H'):
        browse_goto(ui, b, 1);
        b->col = 0;
        return 1;
    case RXK_CTL('E'):
        browse_goto(ui, b, 0xFFFFFFFFu);
        b->col = (int)b->ncols - 1;
        return 1;
    case RXK_CTL('L'):
    case RXK_CTL('P'): /* Ctrl+PgDn arrives as Ctrl+P in most terminals */
        b->hscroll += 4;
        ui->dirty = 1;
        return 1;
    case RXK_CTL('R'):
    case RXK_CTL('K'): /* Ctrl+PgUp */
        b->hscroll -= 4;
        if (b->hscroll < 0) {
            b->hscroll = 0;
        }
        ui->dirty = 1;
        return 1;
    case RXK_CTL('T'):
        browse_toggle_delete(ui, b);
        return 1;
    case RXK_CTL('N'):
        browse_append(ui, b);
        return 1;
    default:
        if (k->sym == RXK_BASE && k->cp >= 32 && browse_editable(b, b->col)) {
            browse_begin_edit(ui, b, k->cp);
            return 1;
        }
        return 0;
    }
}

/* ---- mouse ---- */

static int browse_col_at(rx_browse *b, int x, int *edge)
{
    for (size_t i = 0; i < b->ncols && i < BROWSE_MAXCOLS; i++) {
        int cx = b->colx[i];
        if (x >= cx && x < cx + b->colw[i]) {
            if (edge) {
                *edge = (x == cx + b->colw[i] - 1);
            }
            return (int)i;
        }
        if (x == cx + b->colw[i]) { /* separator = resize border */
            if (edge) {
                *edge = 1;
            }
            return (int)i;
        }
    }
    if (edge) {
        *edge = 0;
    }
    return -1;
}

static void browse_size_widest(rx_ui *ui, rx_browse *b, int col)
{
    int w = (int)strlen(b->cols[col].header);
    uint32_t nrec;
    rx_dbf_recount(b->dbf, &nrec);
    uint32_t r0 = b->top;
    uint32_t r1 = b->top + (uint32_t)b->visrows;
    if (r1 > nrec) {
        r1 = nrec;
    }
    for (uint32_t r = r0; r >= 1 && r <= r1; r++) {
        rx_wa_select(b->area);
        rx_dbf_goto(b->dbf, r);
        char txt[600];
        browse_cell_text(b->ctx, b->dbf, &b->cols[col], txt, sizeof(txt));
        int l = (int)strlen(txt);
        if (l > w) {
            w = l;
        }
    }
    if (w < BROWSE_MINW) {
        w = BROWSE_MINW;
    }
    if (w > BROWSE_MAXW) {
        w = BROWSE_MAXW;
    }
    b->colw[col] = w;
    rx_wa_select(b->area);
    rx_nav_goto(b->ctx, b->cur);
    ui->dirty = 1;
}

static int browse_mouse(struct rx_ui *ui, rx_win *w, const rmouse *m)
{
    rx_browse *b = w->ud;
    int in_border = m->x == w->x || m->x == w->x + w->w - 1 || m->y == w->y ||
                    m->y == w->y + w->h - 1;
    switch (m->kind) {
    case RM_PRESS:
        win_bring_top(ui, w);
        rx_wa_select(b->area);
        if (in_border) {
            return 1; /* the full-screen grid is not moved by its border */
        }
        if (m->y == b->hdr_y) {
            int edge;
            int c = browse_col_at(b, m->x, &edge);
            if (c >= 0 && edge && m->dc) {
                browse_size_widest(ui, b, c); /* double-click divider */
            } else if (c >= 0 && edge) {
                b->drag = 2;
                b->drag_col = c;
            } else if (c >= 0) {
                b->drag = 1;
                b->drag_col = c;
            }
            ui->dirty = 1;
            return 1;
        }
        if (m->y >= b->rows_y && m->y < b->rows_y + b->visrows) {
            uint32_t rec = b->top + (uint32_t)(m->y - b->rows_y);
            int c = browse_col_at(b, m->x, NULL);
            if (c >= 0) {
                b->col = c;
            }
            browse_goto(ui, b, rec);
            b->drag = 3;
            b->sel_ar = (int)b->cur;
            b->sel_ac = b->col;
            ui->dirty = 1;
            return 1;
        }
        return 1;
    case RM_DRAG:
        if (m->button != 1) {
            return 0;
        }
        if (b->drag == 1 && b->drag_col >= 0) { /* header reorder */
            int c = browse_col_at(b, m->x, NULL);
            if (c >= 0 && c != b->drag_col) {
                rx_bcol tc = b->cols[b->drag_col];
                int cw = b->colw[b->drag_col];
                if (c > b->drag_col) {
                    for (int i = b->drag_col; i < c; i++) {
                        b->cols[i] = b->cols[i + 1];
                        b->colw[i] = b->colw[i + 1];
                    }
                } else {
                    for (int i = b->drag_col; i > c; i--) {
                        b->cols[i] = b->cols[i - 1];
                        b->colw[i] = b->colw[i - 1];
                    }
                }
                b->cols[c] = tc;
                b->colw[c] = cw;
                b->drag_col = c;
                if (b->col == (int)(b->ncols)) {
                    b->col--;
                }
                ui->dirty = 1;
            }
            return 1;
        }
        if (b->drag == 2 && b->drag_col >= 0) { /* column resize */
            int nw = m->x - b->colx[b->drag_col];
            if (nw < BROWSE_MINW) {
                nw = BROWSE_MINW;
            }
            if (nw > BROWSE_MAXW) {
                nw = BROWSE_MAXW;
            }
            b->colw[b->drag_col] = nw;
            ui->dirty = 1;
            return 1;
        }
        if (b->drag == 3) { /* drag selection */
            int c = browse_col_at(b, m->x, NULL);
            if (c >= 0) {
                b->col = c;
            }
            if (m->y >= b->rows_y && m->y < b->rows_y + b->visrows) {
                browse_goto(ui, b, b->top + (uint32_t)(m->y - b->rows_y));
            }
            ui->dirty = 1;
            return 1;
        }
        return 0;
    case RM_RELEASE:
        b->drag = 0; /* release never triggers an action (§5.4) */
        return 1;
    case RM_WHEEL_UP:
        if (b->top > 1) {
            b->top -= 3;
            if (b->top < 1) {
                b->top = 1;
            }
            if (b->cur < b->top) {
                browse_goto(ui, b, b->top);
            }
            ui->dirty = 1;
        }
        return 1;
    case RM_WHEEL_DN: {
        uint32_t n;
        rx_dbf_recount(b->dbf, &n);
        if (b->top + (uint32_t)b->visrows <= n) {
            b->top += 3;
            if (b->cur >= b->top + (uint32_t)b->visrows) {
                browse_goto(ui, b, b->top + (uint32_t)b->visrows - 1);
            }
            ui->dirty = 1;
        }
        return 1;
    }
    default:
        return in_border;
    }
}

/* ---- lifecycle ---- */

static void browse_free(rx_win *w)
{
    rx_browse *b = w->ud;
    for (size_t i = 0; i < b->ncols; i++) {
        rx_expr_free(b->cols[i].e);
        rx_expr_free(b->cols[i].valid);
    }
    free(b->cols);
    rx_expr_free(b->when);
    rx_expr_free(b->valid);
    gs_free(&b->ed);
    free(b);
}

int ui_browse_open(void *ud, rx_stmt *s)
{
    rx_ui *ui = ud;
    rx_dbf *dbf;
    if (rx_wa_dbf(0, &dbf) != R_OK) {
        return R_ERR_DBF_NOTOPEN;
    }
    rx_browse *b = calloc(1, sizeof(*b));
    if (!b) {
        RX_FATAL("out of memory");
    }
    b->ctx = ui->ctx;
    b->area = rx_wa_current();
    b->dbf = dbf;
    b->editing = -1;
    b->freeze_col = -1;
    b->hscroll = 0;
    gs_init(&b->ed, 0);

    rx_stmt_steal_bcols(s, &b->cols, &b->ncols);
    if (b->ncols == 0) { /* default: one column per field */
        size_t nf;
        rx_dbf_field_count(dbf, &nf);
        if (nf > BROWSE_MAXCOLS) {
            nf = BROWSE_MAXCOLS;
        }
        b->cols = calloc(nf ? nf : 1, sizeof(rx_bcol));
        if (!b->cols) {
            RX_FATAL("out of memory");
        }
        b->ncols = nf;
        for (size_t i = 0; i < nf; i++) {
            rx_fielddef fd;
            rx_dbf_field_def(dbf, i, &fd);
            b->cols[i].is_field = 1;
            b->cols[i].fldidx = i;
            snprintf(b->cols[i].name, sizeof(b->cols[i].name), "%s", fd.name);
            snprintf(b->cols[i].header, sizeof(b->cols[i].header), "%s", fd.name);
        }
    }
    /* FREEZE with no FIELDS clause: the default column set was built above,
       so resolve the FREEZE field against it here */
    if (b->freeze_col < 0) {
        const char *fz = clause_word(s, "FREEZE");
        if (fz && *fz) {
            size_t idx;
            if (rx_dbf_field_index(dbf, fz, &idx) == R_OK) {
                for (size_t i = 0; i < b->ncols; i++) {
                    if (b->cols[i].is_field && b->cols[i].fldidx == idx) {
                        b->cols[i].frozen = 1;
                        b->freeze_col = (int)i;
                        break;
                    }
                }
            }
        }
    }
    for (size_t i = 0; i < b->ncols; i++) {
        if (b->cols[i].frozen) {
            b->freeze_col = (int)i;
        }
    }

    const char *title = clause_word(s, "TITLE");
    const char *partition = clause_word(s, "PARTITION");
    const char *lockw = clause_word(s, "LOCK");
    const char *widthw = clause_word(s, "WIDTH");
    if (partition) {
        b->partition = atoi(partition);
    }
    if (lockw) {
        b->lock_n = atoi(lockw);
    }
    if (widthw) {
        b->width_cap = atoi(widthw);
    }
    b->append_mode = clause_flag(s, "APPENDMODE");
    b->noappend = clause_flag(s, "NOAPPEND");
    b->nodelete = clause_flag(s, "NODELETE");
    b->nomodify = clause_flag(s, "NOEDIT") || clause_flag(s, "NOMODIFY");
    b->ledit = clause_flag(s, "LEDIT");
    b->redit = clause_flag(s, "REDIT");
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "WHEN") == 0 && s->clauses[i].e) {
            b->when = s->clauses[i].e;
            s->clauses[i].e = NULL;
        } else if (strcmp(s->clauses[i].key, "VALID") == 0 && s->clauses[i].e) {
            b->valid = s->clauses[i].e;
            s->clauses[i].e = NULL;
        } else if (strcmp(s->clauses[i].key, "VERR") == 0) {
            snprintf(b->verr, sizeof(b->verr), "%s", s->clauses[i].word);
        }
    }

    const char *alias;
    rx_dbf_alias(dbf, &alias);
    snprintf(b->w.title, sizeof(b->w.title), "%s", title && *title ? title : alias);
    b->w.min_w = 10;
    b->w.min_h = 4;
    b->w.visible = 1;
    b->w.fullscreen = 1;
    b->w.x = 0;
    b->w.y = 1;
    b->w.w = ui->scr.w;
    b->w.h = ui->scr.h - 2;
    win_clamp(ui, &b->w);
    b->w.render = browse_render;
    b->w.key = browse_key;
    b->w.mouse = browse_mouse;
    b->w.free_fn = browse_free;
    b->w.ud = b;
    uint32_t cur;
    rx_dbf_recno(dbf, &cur);
    b->cur = cur ? cur : 1;
    b->top = b->cur;
    b->col = 0;
    browse_eval_when(b);
    win_push(ui, &b->w);
    if (b->append_mode) {
        browse_append(ui, b);
    }
    ui->dirty = 1;
    return R_OK;
}
