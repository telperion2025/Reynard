/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_rpt.c
 * @brief REPORT FORM / LABEL FORM runtime and CREATE/MODIFY REPORT/LABEL
 *        commands (spec 7.7 + 9.3/9.4, M8).
 *
 * REPORT FORM x [ENVIRONMENT] [scope] [FOR/WHILE] [PLAIN|HEADING c]
 *   [NOEJECT] [TO PRINTER|FILE f|PREVIEW] [SUMMARY] [RANGE n1[,n2]]
 *   [NOCONSOLE] [PDSETUP]
 * Destination: TO FILE picks the emitter by extension (.txt text, .ps,
 * .pdf, .html/.htm, .csv); TO PRINTER spools the text emitter to the
 * SET PRINTER TO file (default reynard.prn); TO PREVIEW opens the preview
 * window (interactive); bare REPORT FORM previews interactively and prints
 * text in batch. LABEL FORM adds [SAMPLE]. Both run headless in -t for CI.
 */
#include <stdio.h>
#include <string.h>

#include "lang/rpt.h"
#include "reynard/dbf.h"
#include "util/rptfmt.h"
#include "util/util.h"

static int emit_kind_for_path(const char* path) {
    size_t l = strlen(path);
    if (l > 4 && rx_strcasecmp(path + l - 4, ".pdf") == 0) {
        return 2;
    }
    if (l > 3 && rx_strcasecmp(path + l - 3, ".ps") == 0) {
        return 1;
    }
    if (l > 5 && rx_strcasecmp(path + l - 5, ".html") == 0) {
        return 3;
    }
    if (l > 4 && rx_strcasecmp(path + l - 4, ".htm") == 0) {
        return 3;
    }
    if (l > 4 && rx_strcasecmp(path + l - 4, ".csv") == 0) {
        return 4;
    }
    return 0; /* text */
}

static int write_out(rx_ctx* ctx, const char* path, const char* data, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        return R_ERR_IO_OPEN;
    }
    int rc = fwrite(data, 1, n, f) == n ? R_OK : R_ERR_IO_WRITE;
    fclose(f);
    (void)ctx;
    return rc;
}

int rx_cmd_report(rx_ctx* ctx, rx_stmt* s, int is_label) {
    if (s->nstrs < 1 || !s->strs[0] || !s->strs[0][0]) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "Expected form name");
    }
    const char* name = s->strs[0];

    rpt_doc doc;
    int rc = rx_rpt_build(ctx, name, s, is_label, &doc);
    if (rc != R_OK) {
        return rc;
    }
    /* reload the form for the emitter (name + a few fields) */
    char path[600];
    rx_resolve_res(ctx, name, is_label ? "rlb" : "frm", path, sizeof(path));
    rpt_file rf;
    if (rptf_load(path, &rf) != R_OK) {
        rptf_init(&rf, name, is_label);
    }

    /* destination */
    const char* tofile = rpt_clause_word(s, "TOFILE");
    int toprinter = rpt_has(s, "TOPRINTER");
    int topreview = rpt_has(s, "TOPREVIEW");
    int kind = 0;
    const char* outpath = NULL;
    char prnpath[600] = "reynard.prn";
    if (tofile) {
        outpath = tofile;
        kind = emit_kind_for_path(tofile);
    } else if (toprinter) {
        outpath = prnpath; /* SET PRINTER TO lands here in M9 polish; v1: fixed spool */
        kind = 0;
    }

    strbuf out;
    sb_init(&out);
    rx_rpt_emit(&doc, &rf, kind, &out);

    int wrc = R_OK;
    if (outpath) {
        wrc = write_out(ctx, outpath, out.buf ? out.buf : "", out.len);
        if (wrc == R_OK && ctx->set.talk) {
            rx_outf(ctx, "Report written to %s.\n", outpath);
        }
    } else if (topreview && ctx->preview_fn) {
        /* preview window over the text rendering */
        wrc = ctx->preview_fn(ctx->preview_ud, rf.name, out.buf ? out.buf : "");
    } else {
        /* SCREEN default: batch prints text to stdout; interactive previews */
        if (ctx->preview_fn) {
            wrc = ctx->preview_fn(ctx->preview_ud, rf.name, out.buf ? out.buf : "");
        } else if (ctx->set.console) {
            rx_out(ctx, out.buf ? out.buf : "", out.len);
        }
    }
    sb_free(&out);
    rpt_doc_free(&doc);
    rptf_free(&rf);
    if (wrc != R_OK) {
        return rx_raise(ctx, wrc, s->line, s->col, "Cannot write report output");
    }
    return R_OK;
}

/* ---------------- CREATE/MODIFY REPORT/LABEL ---------------- */

int rx_cmd_rptb(rx_ctx* ctx, rx_stmt* s) {
    if (s->nstrs < 1 || !s->strs[0] || !s->strs[0][0]) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "Expected form name");
    }
    const char* name = s->strs[0];
    int is_label = (s->kind == ST_CREATE_LABEL || s->kind == ST_MODIFY_LABEL);
    int create = (s->kind == ST_CREATE_REPORT || s->kind == ST_CREATE_LABEL);
    if (ctx->rptb_fn) {
        return ctx->rptb_fn(ctx->rptb_ud, s);
    }
    /* batch: CREATE writes an empty form; MODIFY round-trips load/save */
    char path[600];
    rx_resolve_res(ctx, name, is_label ? "rlb" : "frm", path, sizeof(path));
    rpt_file rf;
    if (create) {
        rptf_init(&rf, name, is_label);
        if (!is_label) {
            rpt_band* d = rptf_band_add(&rf, RB_DETAIL, 1);
            (void)d;
            rptf_band_add(&rf, RB_TITLE, 1);
            rptf_band_add(&rf, RB_PAGEHEADER, 1);
            rptf_band_add(&rf, RB_PAGEFOOTER, 1);
        } else {
            rptf_band_add(&rf, RB_DETAIL, rf.lab_h);
        }
        int rc = rptf_save(path, &rf);
        rptf_free(&rf);
        if (rc != R_OK) {
            return rx_raise(ctx, rc, s->line, s->col, "Cannot create form");
        }
        if (ctx->set.talk) {
            rx_outf(ctx, "%s created.\n", path);
        }
        return R_OK;
    }
    int rc = rptf_load(path, &rf);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "Form not found: %s", path);
    }
    rc = rptf_save(path, &rf); /* canonical re-save (round-trip gate) */
    rptf_free(&rf);
    if (rc != R_OK) {
        return rx_raise(ctx, rc, s->line, s->col, "Cannot write form");
    }
    return R_OK;
}
