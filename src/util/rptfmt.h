/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file rptfmt.h
 * @brief Report (.FRM) and label (.RLB) text formats (spec 9.3/9.4, M8).
 *        Canonical line-based text (like .SCR): the writer emits exactly
 *        what the parser accepts in a fixed order, so load/save round-trips
 *        byte-identically. Layouts in docs/formats.md section 7.
 */
#ifndef REYNARD_RPTFMT_H
#define REYNARD_RPTFMT_H

#include <stddef.h>
#include "util/util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* report bands */
enum {
    RB_TITLE = 1,
    RB_PAGEHEADER,
    RB_GROUPHEADER, /* row holds group expr text in expr */
    RB_DETAIL,
    RB_GROUPFOOTER,
    RB_PAGEFOOTER,
    RB_SUMMARY
};

/* object types inside a band */
enum {
    RO_TEXT = 1, /* text */
    RO_FIELD,    /* expr (raw text) */
    RO_LINE,     /* r,c TO r2,c2 */
    RO_BOX       /* r1,c1 TO r2,c2 */
};

/* report variable aggregates */
enum {
    RV_NONE = 0,
    RV_SUM,
    RV_COUNT,
    RV_AVG,
    RV_MIN,
    RV_MAX
};

typedef struct rpt_obj {
    int type;
    int row, col, row2, col2;
    char text[256]; /* TEXT literal or FIELD expression raw text */
} rpt_obj;

typedef struct rpt_band {
    int type;
    int height;
    char group[160]; /* group expr raw text for GROUPHEADER/GROUPFOOTER */
    rpt_obj *objs;
    size_t nobjs, capobjs;
} rpt_band;

typedef struct rpt_var {
    char name[64];
    int agg;
    char expr[160];
    int reset; /* 1=REPORT 2=PAGE 3=GROUP */
    char init[96];
} rpt_var;

typedef struct rpt_file {
    int is_label; /* 0=.FRM report, 1=.RLB label */
    char name[64];
    /* report page setup */
    int page_w, page_h;
    int margin_l, margin_r, margin_t, margin_b;
    /* label setup */
    char stock[32];
    int lab_h, lab_w, lab_sp_r, lab_sp_c, lab_m_l, lab_m_t;
    rpt_band *bands;
    size_t nbands, capbands;
    rpt_var *vars;
    size_t nvars, capvars;
} rpt_file;

void rptf_init(rpt_file *rf, const char *name, int is_label);
void rptf_free(rpt_file *rf);
rpt_band *rptf_band(const rpt_file *rf, int type);       /* find or add band */
rpt_band *rptf_band_add(rpt_file *rf, int type, int height);
rpt_obj *rptf_obj_add(rpt_band *b);
const char *rptf_band_name(int t);
int rptf_band_type(const char *s);
const char *rptf_agg_name(int a);
int rptf_agg_from_name(const char *s);
int rptf_serialize(const rpt_file *rf, strbuf *sb);
int rptf_load_text(const char *text, rpt_file *rf);
int rptf_load(const char *path, rpt_file *rf);
int rptf_save(const char *path, const rpt_file *rf);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_RPTFMT_H */
