/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file rpt.h
 * @brief Report/label engine internals (spec 9.3/9.4, M8): the engine renders
 *        a .FRM/.RLB into a document of pages of draw ops; emitters
 *        (rpt_emit.c) render ops to text/PS/PDF/HTML/CSV.
 */
#ifndef REYNARD_RPT_H
#define REYNARD_RPT_H

#include "lang/lang.h"
#include "util/rptfmt.h"

/* draw op kinds */
enum { RPO_TEXT = 1, RPO_LINE, RPO_BOX };

typedef struct rpt_op {
    int kind;
    int x, y, x2, y2; /* cell coords, y down from page top */
    int band;         /* source band type (RB_*) */
    char* text;       /* owned, RPO_TEXT only */
} rpt_op;

typedef struct rpt_page {
    rpt_op* ops;
    size_t nops, capops;
} rpt_page;

typedef struct rpt_doc {
    rpt_page* pages;
    size_t npages, cappages;
    int w, h; /* page size in cells */
} rpt_doc;

void rpt_doc_free(rpt_doc* d);
rpt_page* rpt_doc_page(rpt_doc* d); /* append a page, return it */
void rpt_page_op(rpt_page* p, int kind, int x, int y, int x2, int y2, const char* text);
void rpt_page_op_band(rpt_page* p, int kind, int band, int x, int y, int x2, int y2,
                      const char* text);

/* engine: run REPORT/LABEL form `name` with parsed stmt clauses.
   Produces a document; caller emits. Returns R_OK or error code. */
int rx_rpt_build(rx_ctx* ctx, const char* name, rx_stmt* s, int is_label, rpt_doc* doc);

/* emitters (rpt_emit.c): render doc to strbuf. kind: 0 text,1 ps,2 pdf,3 html,4 csv */
int rx_rpt_emit(const rpt_doc* doc, const rpt_file* rf, int kind, strbuf* out);

/* clause helpers shared with cmds_rpt.c */
const char* rpt_clause_word(rx_stmt* s, const char* key);
rx_clause* rpt_clause_e(rx_stmt* s, const char* key);
int rpt_has(rx_stmt* s, const char* key);

#endif /* REYNARD_RPT_H */
