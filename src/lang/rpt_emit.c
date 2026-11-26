/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file rpt_emit.c
 * @brief Report emitters (spec 9.3, M8): render a rpt_doc to text, PostScript,
 *        PDF (base-14 fonts, no embeds), HTML (tables), CSV (detail rows).
 *        All deterministic (no timestamps/random IDs; the PDF /ID is derived
 *        from the content hash), so CI can golden-compare text and
 *        hash-stable-compare the rest.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lang/rpt.h"
#include "util/util.h"

/* ---------------- text ---------------- */

static void emit_text(const rpt_doc *doc, strbuf *out)
{
    for (size_t pi = 0; pi < doc->npages; pi++) {
        const rpt_page *p = &doc->pages[pi];
        /* rasterize to a char grid */
        char *grid = calloc((size_t)doc->w * (size_t)doc->h, 1);
        if (!grid) {
            RX_FATAL("out of memory");
        }
        memset(grid, ' ', (size_t)doc->w * (size_t)doc->h);
        for (size_t i = 0; i < p->nops; i++) {
            const rpt_op *o = &p->ops[i];
            if (o->kind == RPO_TEXT && o->text) {
                const char *t = o->text;
                for (int x = o->x; *t && x < doc->w; x++, t++) {
                    if (o->y >= 0 && o->y < doc->h && x >= 0) {
                        grid[(size_t)o->y * doc->w + x] = *t;
                    }
                }
            } else if (o->kind == RPO_LINE) {
                int x1 = o->x, y1 = o->y, x2 = o->x2, y2 = o->y2;
                if (y1 == y2) {
                    for (int x = x1; x <= x2; x++) {
                        if (y1 >= 0 && y1 < doc->h && x >= 0 && x < doc->w) {
                            grid[(size_t)y1 * doc->w + x] = '-';
                        }
                    }
                } else {
                    for (int y = y1; y <= y2; y++) {
                        if (y >= 0 && y < doc->h && x1 >= 0 && x1 < doc->w) {
                            grid[(size_t)y * doc->w + x1] = '|';
                        }
                    }
                }
            } else if (o->kind == RPO_BOX) {
                int x1 = o->x, y1 = o->y, x2 = o->x2, y2 = o->y2;
                for (int x = x1; x <= x2; x++) {
                    if (y1 >= 0 && y1 < doc->h && x >= 0 && x < doc->w) {
                        grid[(size_t)y1 * doc->w + x] = '-';
                    }
                    if (y2 >= 0 && y2 < doc->h && x >= 0 && x < doc->w) {
                        grid[(size_t)y2 * doc->w + x] = '-';
                    }
                }
                for (int y = y1; y <= y2; y++) {
                    if (y >= 0 && y < doc->h && x1 >= 0 && x1 < doc->w) {
                        grid[(size_t)y * doc->w + x1] = '|';
                    }
                    if (y >= 0 && y < doc->h && x2 >= 0 && x2 < doc->w) {
                        grid[(size_t)y * doc->w + x2] = '|';
                    }
                }
            }
        }
        for (int y = 0; y < doc->h; y++) {
            /* rstrip */
            int xe = doc->w;
            while (xe > 0 && grid[(size_t)y * doc->w + xe - 1] == ' ') {
                xe--;
            }
            sb_put(out, &grid[(size_t)y * doc->w], (size_t)xe);
            sb_putc(out, '\n');
        }
        free(grid);
        if (pi + 1 < doc->npages) {
            sb_putc(out, '\f');
            sb_putc(out, '\n');
        }
    }
}

/* ---------------- CSV ---------------- */

static void csv_cell(strbuf *out, const char *s)
{
    /* rtrim field padding */
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') {
        n--;
    }
    char *t = malloc(n + 1);
    if (!t) {
        RX_FATAL("out of memory");
    }
    memcpy(t, s, n);
    t[n] = '\0';
    s = t;
    int need = strpbrk(s, ",\"\n\r") != NULL;
    if (!need) {
        sb_put(out, s, strlen(s));
        free((char *)s);
        return;
    }
    sb_putc(out, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '"') {
            sb_putc(out, '"');
        }
        sb_putc(out, *p);
    }
    sb_putc(out, '"');
    free((char *)s);
}

static void emit_csv(const rpt_doc *doc, strbuf *out)
{
    /* one row per text op group is wrong; CSV wants detail fields per record.
       The engine stacks detail records in one page; we emit each detail line's
       text ops as cells in op order. */
    for (size_t pi = 0; pi < doc->npages; pi++) {
        const rpt_page *p = &doc->pages[pi];
        /* group text ops by row */
        int last_y = -1;
        for (size_t i = 0; i < p->nops; i++) {
            const rpt_op *o = &p->ops[i];
            if (o->kind != RPO_TEXT || !o->text || o->band != RB_DETAIL) {
                continue; /* CSV exports detail rows only */
            }
            if (o->y != last_y) {
                if (last_y >= 0) {
                    sb_putc(out, '\n');
                }
                last_y = o->y;
            } else {
                sb_putc(out, ',');
            }
            csv_cell(out, o->text);
        }
        if (last_y >= 0) {
            sb_putc(out, '\n');
        }
    }
}

/* ---------------- HTML ---------------- */

static void html_esc_n(strbuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        switch (c) {
        case '<': sb_puts(out, "&lt;"); break;
        case '>': sb_puts(out, "&gt;"); break;
        case '&': sb_puts(out, "&amp;"); break;
        case '"': sb_puts(out, "&quot;"); break;
        default: sb_putc(out, c); break;
        }
    }
}

static void emit_html(const rpt_doc *doc, const rpt_file *rf, strbuf *out)
{
    sb_printf(out, "<!DOCTYPE html>\n<html>\n<head><meta charset=\"utf-8\"><title>%s</title></head>\n<body>\n",
              rf->name);
    sb_printf(out, "<h1>%s</h1>\n", rf->name);
    for (size_t pi = 0; pi < doc->npages; pi++) {
        const rpt_page *p = &doc->pages[pi];
        char *grid = calloc((size_t)doc->w * (size_t)doc->h, 1);
        if (!grid) {
            RX_FATAL("out of memory");
        }
        memset(grid, ' ', (size_t)doc->w * (size_t)doc->h);
        for (size_t i = 0; i < p->nops; i++) {
            const rpt_op *o = &p->ops[i];
            if (o->kind == RPO_TEXT && o->text) {
                const char *t = o->text;
                for (int x = o->x; *t && x < doc->w; x++, t++) {
                    if (o->y >= 0 && o->y < doc->h && x >= 0) {
                        grid[(size_t)o->y * doc->w + x] = *t;
                    }
                }
            }
        }
        sb_printf(out, "<h2>Page %zu</h2>\n<table border=\"1\">\n", pi + 1);
        for (int y = 0; y < doc->h; y++) {
            int xe = doc->w;
            while (xe > 0 && grid[(size_t)y * doc->w + xe - 1] == ' ') {
                xe--;
            }
            if (xe == 0 && y > 0) {
                continue; /* skip blank rows except structure */
            }
            sb_puts(out, "<tr><td><pre>");
            html_esc_n(out, &grid[(size_t)y * doc->w], (size_t)xe);
            sb_puts(out, "</pre></td></tr>\n");
        }
        sb_puts(out, "</table>\n");
        free(grid);
    }
    sb_puts(out, "</body>\n</html>\n");
}

/* ---------------- PostScript ---------------- */

/* cell metrics: 6pt wide, 12pt high; page in points */
#define CELL_W 6
#define CELL_H 12

static void ps_escape(strbuf *out, const char *s)
{
    sb_putc(out, '(');
    for (const char *p = s; *p; p++) {
        if (*p == '(' || *p == ')' || *p == '\\') {
            sb_putc(out, '\\');
        }
        sb_putc(out, *p);
    }
    sb_putc(out, ')');
}

static void emit_ps(const rpt_doc *doc, const rpt_file *rf, strbuf *out)
{
    int pw = doc->w * CELL_W;
    int ph = doc->h * CELL_H;
    sb_puts(out, "%!PS-Adobe-3.0\n");
    sb_printf(out, "%%%%BoundingBox: 0 0 %d %d\n", pw, ph);
    sb_printf(out, "%%%%Title: %s\n", rf->name);
    sb_puts(out, "/F1 /Helvetica findfont 9 scalefont setfont\n");
    for (size_t pi = 0; pi < doc->npages; pi++) {
        const rpt_page *p = &doc->pages[pi];
        sb_printf(out, "%%%%Page: %zu %zu\n", pi + 1, pi + 1);
        for (size_t i = 0; i < p->nops; i++) {
            const rpt_op *o = &p->ops[i];
            if (o->kind == RPO_TEXT && o->text) {
                sb_printf(out, "%d %d moveto ", o->x * CELL_W, ph - (o->y + 1) * CELL_H);
                ps_escape(out, o->text);
                sb_puts(out, " show\n");
            } else if (o->kind == RPO_LINE) {
                sb_printf(out, "newpath %d %d moveto %d %d lineto stroke\n", o->x * CELL_W,
                          ph - o->y * CELL_H, o->x2 * CELL_W, ph - o->y2 * CELL_H);
            } else if (o->kind == RPO_BOX) {
                sb_printf(out, "newpath %d %d moveto %d %d lineto %d %d lineto %d %d lineto closepath stroke\n",
                          o->x * CELL_W, ph - o->y * CELL_H, o->x2 * CELL_W, ph - o->y * CELL_H,
                          o->x2 * CELL_W, ph - o->y2 * CELL_H, o->x * CELL_W, ph - o->y2 * CELL_H);
            }
        }
        sb_puts(out, "showpage\n");
    }
    sb_puts(out, "%%%%EOF\n");
}

/* ---------------- PDF ---------------- */

static void pdf_escape(strbuf *out, const char *s)
{
    sb_putc(out, '(');
    for (const char *p = s; *p; p++) {
        if (*p == '(' || *p == ')' || *p == '\\') {
            sb_putc(out, '\\');
        }
        sb_putc(out, *p);
    }
    sb_putc(out, ')');
}

static void emit_pdf(const rpt_doc *doc, const rpt_file *rf, strbuf *out)
{
    int pw = doc->w * CELL_W;
    int ph = doc->h * CELL_H;
    /* object 1: catalog. 2: pages. 3: font. then per page: page obj + content obj */
    size_t npages = doc->npages ? doc->npages : 1;
    size_t total_objs = 3 + npages * 2;
    size_t *offs = calloc(total_objs + 1, sizeof(size_t));
    if (!offs) {
        RX_FATAL("out of memory");
    }

    sb_puts(out, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");

    /* 1 catalog */
    offs[1] = out->len;
    sb_printf(out, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    /* 2 pages */
    offs[2] = out->len;
    sb_printf(out, "2 0 obj\n<< /Type /Pages /Kids [");
    for (size_t i = 0; i < npages; i++) {
        sb_printf(out, "%zu 0 R ", 4 + i * 2);
    }
    sb_printf(out, "] /Count %zu >>\nendobj\n", npages);
    /* 3 font */
    offs[3] = out->len;
    sb_printf(out, "3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n");

    for (size_t pi = 0; pi < npages; pi++) {
        const rpt_page *p = &doc->pages[pi];
        size_t page_obj = 4 + pi * 2;
        size_t cont_obj = page_obj + 1;
        /* content stream */
        strbuf cs;
        sb_init(&cs);
        sb_puts(&cs, "BT /F1 9 Tf\n");
        for (size_t i = 0; i < p->nops; i++) {
            const rpt_op *o = &p->ops[i];
            if (o->kind == RPO_TEXT && o->text) {
                sb_printf(&cs, "%d %d Td ", o->x * CELL_W, ph - (o->y + 1) * CELL_H);
                pdf_escape(&cs, o->text);
                sb_puts(&cs, " Tj\n");
            } else if (o->kind == RPO_LINE) {
                sb_printf(&cs, "ET\n%d %d m %d %d l S\nBT\n", o->x * CELL_W, ph - o->y * CELL_H,
                          o->x2 * CELL_W, ph - o->y2 * CELL_H);
            } else if (o->kind == RPO_BOX) {
                sb_printf(&cs, "ET\n%d %d m %d %d l %d %d l %d %d l h S\nBT\n", o->x * CELL_W,
                          ph - o->y * CELL_H, o->x2 * CELL_W, ph - o->y * CELL_H, o->x2 * CELL_W,
                          ph - o->y2 * CELL_H, o->x * CELL_W, ph - o->y2 * CELL_H);
            }
        }
        sb_puts(&cs, "ET\n");
        offs[cont_obj] = out->len;
        sb_printf(out, "%zu 0 obj\n<< /Length %zu >>\nstream\n", cont_obj, cs.len);
        sb_put(out, cs.buf ? cs.buf : "", cs.len);
        sb_puts(out, "\nendstream\nendobj\n");
        sb_free(&cs);
        offs[page_obj] = out->len;
        sb_printf(out, "%zu 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Resources << /Font << /F1 3 0 R >> >> /Contents %zu 0 R >>\nendobj\n",
                  page_obj, pw, ph, cont_obj);
    }

    /* xref */
    size_t xref_pos = out->len;
    sb_printf(out, "xref\n0 %zu\n", total_objs + 1);
    sb_puts(out, "0000000000 65535 f \n");
    for (size_t i = 1; i <= total_objs; i++) {
        sb_printf(out, "%010zu 00000 n \n", offs[i]);
    }
    /* deterministic /ID from content hash */
    uint8_t h[32];
    rx_sha256(out->buf, out->len, h);
    sb_printf(out, "trailer\n<< /Size %zu /Root 1 0 R /ID [<%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x> <%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x>] >>\nstartxref\n%zu\n%%%%EOF\n",
              total_objs + 1,
              h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15],
              h[16], h[17], h[18], h[19], h[20], h[21], h[22], h[23], h[24], h[25], h[26], h[27], h[28], h[29], h[30], h[31],
              h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15],
              h[16], h[17], h[18], h[19], h[20], h[21], h[22], h[23], h[24], h[25], h[26], h[27], h[28], h[29], h[30], h[31],
              xref_pos);
    free(offs);
    (void)rf;
}

/* ---------------- entry ---------------- */

int rx_rpt_emit(const rpt_doc *doc, const rpt_file *rf, int kind, strbuf *out)
{
    switch (kind) {
    case 1:
        emit_ps(doc, rf, out);
        break;
    case 2:
        emit_pdf(doc, rf, out);
        break;
    case 3:
        emit_html(doc, rf, out);
        break;
    case 4:
        emit_csv(doc, out);
        break;
    default:
        emit_text(doc, out);
        break;
    }
    return R_OK;
}
