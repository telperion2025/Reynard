/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file rpt.c
 * @brief Report/label engine (spec 9.3/9.4, M8): renders a .FRM/.RLB into
 *        pages of draw ops. Bands: Title, PageHeader, GroupHeader/Footer,
 *        Detail, PageFooter, Summary; report variables with SUM/COUNT/AVG/
 *        MIN/MAX reset per report/page/group; page breaks; PLAIN/HEADING/
 *        SUMMARY/RANGE clauses. Deterministic in -t (frozen DATE()).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lang/funcs.h"
#include "lang/rpt.h"
#include "reynard/dbf.h"
#include "util/util.h"

/* ---------------- doc ---------------- */

void rpt_doc_free(rpt_doc* d) {
    for (size_t i = 0; i < d->npages; i++) {
        for (size_t j = 0; j < d->pages[i].nops; j++) {
            free(d->pages[i].ops[j].text);
        }
        free(d->pages[i].ops);
    }
    free(d->pages);
    memset(d, 0, sizeof(*d));
}

rpt_page* rpt_doc_page(rpt_doc* d) {
    if (d->npages == d->cappages) {
        size_t nc = d->cappages ? d->cappages * 2 : 8;
        rpt_page* np = realloc(d->pages, nc * sizeof(rpt_page));
        if (!np) {
            RX_FATAL("out of memory");
        }
        d->pages = np;
        d->cappages = nc;
    }
    rpt_page* p = &d->pages[d->npages++];
    memset(p, 0, sizeof(*p));
    return p;
}

void rpt_page_op_band(rpt_page* p, int kind, int band, int x, int y, int x2, int y2,
                      const char* text) {
    if (p->nops == p->capops) {
        size_t nc = p->capops ? p->capops * 2 : 32;
        rpt_op* no = realloc(p->ops, nc * sizeof(rpt_op));
        if (!no) {
            RX_FATAL("out of memory");
        }
        p->ops = no;
        p->capops = nc;
    }
    rpt_op* o = &p->ops[p->nops++];
    o->kind = kind;
    o->band = band;
    o->x = x;
    o->y = y;
    o->x2 = x2;
    o->y2 = y2;
    o->text = text ? rx_strdup(text) : NULL;
}

void rpt_page_op(rpt_page* p, int kind, int x, int y, int x2, int y2, const char* text) {
    rpt_page_op_band(p, kind, 0, x, y, x2, y2, text);
}

/* ---------------- clause helpers ---------------- */

const char* rpt_clause_word(rx_stmt* s, const char* key) {
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, key) == 0) {
            return s->clauses[i].word;
        }
    }
    return NULL;
}

rx_clause* rpt_clause_e(rx_stmt* s, const char* key) {
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, key) == 0) {
            return &s->clauses[i];
        }
    }
    return NULL;
}

int rpt_has(rx_stmt* s, const char* key) {
    return rpt_clause_word(s, key) != NULL || rpt_clause_e(s, key) != NULL;
}

/* ---------------- engine state ---------------- */

typedef struct {
    rx_ctx* ctx;
    const rpt_file* rf;
    rpt_doc* doc;
    rpt_page* page;
    int cur_y; /* next free row on the page (0-based, includes margins) */
    int page_no;
    int plain;
    char* heading; /* HEADING text */
    int summary_only;
    uint32_t range_lo, range_hi; /* 0 = unset */
    /* group state */
    const rpt_band *gh, *gf;
    const char* group_expr; /* raw text */
    rx_expr* group_e;
    rval group_key; /* current group key value */
    int have_group;
    int per_record_page; /* labels: each record gets its own page */
    /* variables */
    rpt_var* vars;
    size_t nvars;
    double* v_sum;
    double* v_cnt;
    rval *v_min, *v_max;
    int* v_set;
} rpt_eng;

static int render_band(rpt_eng* e, const rpt_band* b, int y);
static void var_reset(rpt_eng* e, int level);

static void eng_new_page(rpt_eng* e) {
    /* page footer of the old page */
    if (e->page && e->rf->is_label == 0) {
        const rpt_band* pf = rptf_band(e->rf, RB_PAGEFOOTER);
        if (pf && !e->plain) {
            /* footer renders at the bottom of the page */
            int fy = e->rf->page_h - e->rf->margin_b - pf->height;
            (void)fy;
        }
    }
    int first = (e->page == NULL);
    e->page = rpt_doc_page(e->doc);
    e->page_no++;
    if (!first) {
        var_reset(e, 2); /* PAGE-level vars */
    }
    e->cur_y = e->rf->margin_t;
    if (!e->rf->is_label && e->page_no == 1) {
        /* Title band comes first on page 1 */
        const rpt_band* tb = rptf_band(e->rf, RB_TITLE);
        if (tb) {
            e->cur_y = render_band(e, tb, e->cur_y);
        }
    }
    if (!e->rf->is_label && !e->plain) {
        /* date left, "Page N" right on row 0 of the header area */
        rval dv;
        rx_rval_undef(&dv);
        if (rx_eval_str(e->ctx, "DTOC(DATE())", &dv, &e->ctx->err) == R_OK) {
            strbuf sb;
            sb_init(&sb);
            rx_vm_display(e->ctx, &dv, &sb);
            rpt_page_op(e->page, RPO_TEXT, e->rf->margin_l, e->cur_y, 0, 0, sb.buf ? sb.buf : "");
            sb_free(&sb);
            rx_rval_free(&dv);
        }
        char pn[32];
        snprintf(pn, sizeof(pn), "Page %d", e->page_no);
        int px = e->rf->page_w - e->rf->margin_r - (int)strlen(pn);
        if (px < 0) {
            px = 0;
        }
        rpt_page_op(e->page, RPO_TEXT, px, e->cur_y, 0, 0, pn);
        e->cur_y++;
        if (e->heading) {
            rpt_page_op(e->page, RPO_TEXT, e->rf->margin_l, e->cur_y, 0, 0, e->heading);
            e->cur_y++;
        }
        const rpt_band* ph = rptf_band(e->rf, RB_PAGEHEADER);
        if (ph) {
            e->cur_y = render_band(e, ph, e->cur_y);
        }
    }
}

/* render a band's objects at y, return next free row */
static int g_cur_band; /* band being rendered (for op tagging) */

static void render_obj(rpt_eng* e, const rpt_obj* o, int y) {
    int x = e->rf->margin_l + o->col;
    int yy = y + o->row;
    switch (o->type) {
    case RO_TEXT:
        rpt_page_op_band(e->page, RPO_TEXT, g_cur_band, x, yy, 0, 0, o->text);
        break;
    case RO_FIELD: {
        rval v;
        rx_rval_undef(&v);
        if (rx_eval_str(e->ctx, o->text, &v, &e->ctx->err) == R_OK) {
            strbuf sb;
            sb_init(&sb);
            rx_vm_display(e->ctx, &v, &sb);
            rpt_page_op_band(e->page, RPO_TEXT, g_cur_band, x, yy, 0, 0, sb.buf ? sb.buf : "");
            sb_free(&sb);
        }
        rx_rval_free(&v);
        break;
    }
    case RO_LINE:
        rpt_page_op_band(e->page, RPO_LINE, g_cur_band, x, yy, e->rf->margin_l + o->col2,
                         y + o->row2, NULL);
        break;
    case RO_BOX:
        rpt_page_op_band(e->page, RPO_BOX, g_cur_band, x, yy, e->rf->margin_l + o->col2,
                         y + o->row2, NULL);
        break;
    }
}

static int render_band(rpt_eng* e, const rpt_band* b, int y) {
    int saved = g_cur_band;
    g_cur_band = b->type;
    for (size_t i = 0; i < b->nobjs; i++) {
        render_obj(e, &b->objs[i], y);
    }
    g_cur_band = saved;
    return y + b->height;
}

static void eng_ensure(rpt_eng* e, int h) {
    if (e->rf->is_label) {
        return; /* labels: no page breaks */
    }
    int limit = e->rf->page_h - e->rf->margin_b;
    if (e->cur_y + h > limit && e->page) {
        eng_new_page(e);
        /* reprint group header on the new page when a group is open */
        if (e->gh && e->have_group) {
            e->cur_y = render_band(e, e->gh, e->cur_y);
        }
    }
}

/* value compare: numeric when both numeric, else display-string compare */
static int rpt_rval_cmp(rx_ctx* ctx, const rval* a, const rval* b) {
    double da, db;
    if (rv_to_num(a, &da) && rv_to_num(b, &db)) {
        return da < db ? -1 : da > db ? 1 : 0;
    }
    strbuf sa, sb;
    sb_init(&sa);
    sb_init(&sb);
    rx_vm_display(ctx, a, &sa);
    rx_vm_display(ctx, b, &sb);
    int c = strcmp(sa.buf ? sa.buf : "", sb.buf ? sb.buf : "");
    sb_free(&sa);
    sb_free(&sb);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

/* ---------------- variables ---------------- */

static void var_reset(rpt_eng* e, int level) {
    for (size_t i = 0; i < e->nvars; i++) {
        rpt_var* v = &e->vars[i];
        /* level 1 = report start resets everything; otherwise only vars whose
           reset point is exactly this level (2=PAGE, 3=GROUP) */
        if (level != 1 && v->reset != level) {
            continue;
        }
        e->v_sum[i] = 0;
        e->v_cnt[i] = 0;
        e->v_set[i] = 0;
        rx_rval_free(&e->v_min[i]);
        rx_rval_free(&e->v_max[i]);
        rx_rval_undef(&e->v_min[i]);
        rx_rval_undef(&e->v_max[i]);
        /* INITIAL */
        if (v->init[0]) {
            rval iv;
            rx_rval_undef(&iv);
            if (rx_eval_str(e->ctx, v->init, &iv, &e->ctx->err) == R_OK) {
                if (v->agg == RV_SUM || v->agg == RV_AVG) {
                    double d = 0;
                    rv_to_num(&iv, &d);
                    e->v_sum[i] = d;
                    e->v_set[i] = 1;
                }
                rx_rval_free(&iv);
            }
        }
        /* publish as memvar */
        rval mv;
        rx_rval_undef(&mv);
        switch (v->agg) {
        case RV_SUM:
            mv.t = RX_T_N;
            mv.u.n.d = e->v_sum[i];
            mv.u.n.dec = e->ctx->set.decimals;
            break;
        case RV_COUNT:
            mv.t = RX_T_N;
            mv.u.n.d = e->v_cnt[i];
            mv.u.n.dec = 0;
            break;
        default:
            break;
        }
        if (mv.t != RX_T_U || v->agg == RV_SUM || v->agg == RV_COUNT) {
            rx_memvar_set(e->ctx, v->name, &mv);
            rx_rval_free(&mv);
        }
    }
}

static void var_accum(rpt_eng* e, uint32_t recno) {
    (void)recno;
    for (size_t i = 0; i < e->nvars; i++) {
        rpt_var* v = &e->vars[i];
        if (v->agg == RV_NONE) {
            continue;
        }
        rval x;
        rx_rval_undef(&x);
        if (rx_eval_str(e->ctx, v->expr, &x, &e->ctx->err) != R_OK) {
            rx_rval_free(&x);
            continue;
        }
        double d = 0;
        int isnum = rv_to_num(&x, &d);
        switch (v->agg) {
        case RV_SUM:
            if (isnum) {
                e->v_sum[i] += d;
                e->v_set[i] = 1;
            }
            break;
        case RV_COUNT:
            e->v_cnt[i] += 1;
            e->v_set[i] = 1;
            break;
        case RV_AVG:
            if (isnum) {
                e->v_sum[i] += d;
                e->v_cnt[i] += 1;
                e->v_set[i] = 1;
            }
            break;
        case RV_MIN:
            if (!e->v_set[i] || rpt_rval_cmp(e->ctx, &x, &e->v_min[i]) < 0) {
                rx_rval_free(&e->v_min[i]);
                rx_rval_deep(&e->v_min[i], &x);
                e->v_set[i] = 1;
            }
            break;
        case RV_MAX:
            if (!e->v_set[i] || rpt_rval_cmp(e->ctx, &x, &e->v_max[i]) > 0) {
                rx_rval_free(&e->v_max[i]);
                rx_rval_deep(&e->v_max[i], &x);
                e->v_set[i] = 1;
            }
            break;
        default:
            break;
        }
        rx_rval_free(&x);
    }
}

/* publish current aggregate values as memvars (call before rendering a band) */
static void var_publish(rpt_eng* e) {
    for (size_t i = 0; i < e->nvars; i++) {
        rpt_var* v = &e->vars[i];
        rval mv;
        rx_rval_undef(&mv);
        switch (v->agg) {
        case RV_SUM:
            mv.t = RX_T_N;
            mv.u.n.d = e->v_sum[i];
            mv.u.n.dec = e->ctx->set.decimals;
            break;
        case RV_COUNT:
            mv.t = RX_T_N;
            mv.u.n.d = e->v_cnt[i];
            mv.u.n.dec = 0;
            break;
        case RV_AVG:
            mv.t = RX_T_N;
            mv.u.n.d = e->v_cnt[i] ? e->v_sum[i] / e->v_cnt[i] : 0;
            mv.u.n.dec = e->ctx->set.decimals;
            break;
        case RV_MIN:
            if (e->v_set[i]) {
                rx_rval_deep(&mv, &e->v_min[i]);
            }
            break;
        case RV_MAX:
            if (e->v_set[i]) {
                rx_rval_deep(&mv, &e->v_max[i]);
            }
            break;
        default:
            break;
        }
        if (mv.t != RX_T_U) {
            rx_memvar_set(e->ctx, v->name, &mv);
            rx_rval_free(&mv);
        }
    }
}

/* ---------------- iterate callback ---------------- */

typedef struct {
    rpt_eng* e;
    const rpt_band* detail;
    int rc;
} rpt_iter_ud;

static int rpt_rec_cb(rx_ctx* ctx, rx_dbf* dbf, uint32_t recno, void* ud) {
    (void)ctx;
    (void)dbf;
    rpt_iter_ud* u = ud;
    rpt_eng* e = u->e;
    if (e->range_lo && (recno < e->range_lo || (e->range_hi && recno > e->range_hi))) {
        return R_OK; /* RANGE n1,n2 */
    }
    if (e->per_record_page && e->page && e->page->nops > 0) {
        eng_new_page(e); /* labels: one label per page */
    }
    /* group change? */
    if (e->gh && e->group_e) {
        rval key;
        rx_rval_undef(&key);
        if (rx_eval(e->ctx, e->group_e, &key, &e->ctx->err) == R_OK) {
            if (e->have_group && rpt_rval_cmp(e->ctx, &key, &e->group_key) != 0) {
                /* group footer for the old group, then reset group vars */
                var_publish(e);
                if (e->gf) {
                    eng_ensure(e, e->gf->height);
                    e->cur_y = render_band(e, e->gf, e->cur_y);
                }
                var_reset(e, 3); /* GROUP-level vars */
                if (e->gh) {
                    eng_ensure(e, e->gh->height);
                    e->cur_y = render_band(e, e->gh, e->cur_y);
                }
            } else if (!e->have_group) {
                e->have_group = 1;
                if (e->gh) {
                    eng_ensure(e, e->gh->height);
                    e->cur_y = render_band(e, e->gh, e->cur_y);
                }
            }
            rx_rval_free(&e->group_key);
            e->group_key = key;
        } else {
            rx_rval_free(&key);
        }
    }
    var_accum(e, recno);
    if (!e->summary_only) {
        eng_ensure(e, u->detail->height);
        var_publish(e);
        e->cur_y = render_band(e, u->detail, e->cur_y);
    }
    return R_OK;
}

/* ---------------- main build ---------------- */

int rx_rpt_build(rx_ctx* ctx, const char* name, rx_stmt* s, int is_label, rpt_doc* doc) {
    char path[600];
    rx_resolve_res(ctx, name, is_label ? "rlb" : "frm", path, sizeof(path));
    rpt_file rf;
    int rc = rptf_load(path, &rf);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_IO_NOTFOUND, s ? s->line : 0, s ? s->col : 0,
                        "Report/label file not found: %s", path);
    }
    memset(doc, 0, sizeof(*doc));
    doc->w = rf.page_w;
    doc->h = rf.page_h;

    rpt_eng e;
    memset(&e, 0, sizeof(e));
    e.ctx = ctx;
    e.rf = &rf;
    e.doc = doc;
    e.plain = rpt_has(s, "PLAIN");
    e.summary_only = rpt_has(s, "SUMMARY");
    rx_clause* hc = rpt_clause_e(s, "HEADING");
    if (hc) {
        rval v;
        rx_rval_undef(&v);
        if (rx_eval(ctx, hc->e, &v, &ctx->err) == R_OK) {
            strbuf sb;
            sb_init(&sb);
            rx_vm_display(ctx, &v, &sb);
            e.heading = rx_strdup(sb.buf ? sb.buf : "");
            sb_free(&sb);
        }
        rx_rval_free(&v);
    }
    rx_clause* r1 = rpt_clause_e(s, "RANGE");
    if (r1) {
        rval v;
        rx_rval_undef(&v);
        if (rx_eval(ctx, r1->e, &v, &ctx->err) == R_OK) {
            double d = 0;
            rv_to_num(&v, &d);
            e.range_lo = (uint32_t)d;
        }
        rx_rval_free(&v);
        rx_clause* r2 = rpt_clause_e(s, "RANGE2");
        if (r2) {
            rval v2;
            rx_rval_undef(&v2);
            if (rx_eval(ctx, r2->e, &v2, &ctx->err) == R_OK) {
                double d = 0;
                rv_to_num(&v2, &d);
                e.range_hi = (uint32_t)d;
            }
            rx_rval_free(&v2);
        }
    }
    e.gh = rptf_band(&rf, RB_GROUPHEADER);
    e.gf = rptf_band(&rf, RB_GROUPFOOTER);
    if (e.gh) {
        e.group_expr = e.gh->group;
        e.group_e = NULL;
        if (e.group_expr[0]) {
            rx_expr* ex = NULL;
            if (rx_parse_expr_str(e.group_expr, &ex, &ctx->err) == R_OK) {
                rx_code code;
                memset(&code, 0, sizeof(code));
                /* rx_eval on a parsed expr: use a tiny wrapper stmt-free path */
                e.group_e = ex; /* keep the AST; evaluated via rx_eval */
                (void)code;
            }
        }
    }
    /* variables */
    e.nvars = rf.nvars;
    e.vars = rf.vars;
    e.v_sum = calloc(e.nvars ? e.nvars : 1, sizeof(double));
    e.v_cnt = calloc(e.nvars ? e.nvars : 1, sizeof(double));
    e.v_min = calloc(e.nvars ? e.nvars : 1, sizeof(rval));
    e.v_max = calloc(e.nvars ? e.nvars : 1, sizeof(rval));
    e.v_set = calloc(e.nvars ? e.nvars : 1, sizeof(int));
    if (!e.v_sum || !e.v_cnt || !e.v_min || !e.v_max || !e.v_set) {
        RX_FATAL("out of memory");
    }
    for (size_t i = 0; i < e.nvars; i++) {
        rx_rval_undef(&e.v_min[i]);
        rx_rval_undef(&e.v_max[i]);
    }
    rx_rval_undef(&e.group_key);

    /* labels: render as a single "page" of label cells, repeated per record */
    if (is_label) {
        doc->w = rf.lab_w;
        doc->h = rf.lab_h;
        e.per_record_page = 1;
        rpt_page* pg = rpt_doc_page(doc);
        e.page = pg;
        e.cur_y = 0;
        const rpt_band* det = rptf_band(&rf, RB_DETAIL);
        if (!det) {
            det = rptf_band(&rf, RB_TITLE); /* labels often use one band */
        }
        if (det) {
            rpt_iter_ud ud = {&e, det, R_OK};
            rx_dbf* dbf;
            if (rx_wa_dbf(0, &dbf) == R_OK) {
                rc = rx_iterate(ctx, s, dbf, 1, rpt_rec_cb, &ud, NULL);
            } else {
                rc = rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
            }
        }
        free(e.v_sum);
        free(e.v_cnt);
        free(e.v_min);
        free(e.v_max);
        free(e.v_set);
        free(e.heading);
        rx_expr_free(e.group_e);
        rx_rval_free(&e.group_key);
        rptf_free(&rf);
        return rc;
    }

    /* report: page 1 opens (title band renders inside eng_new_page) */
    eng_new_page(&e);
    var_reset(&e, 1); /* REPORT-level vars (all) */
    const rpt_band* det = rptf_band(&rf, RB_DETAIL);
    if (det) {
        rpt_iter_ud ud = {&e, det, R_OK};
        rx_dbf* dbf;
        if (rx_wa_dbf(0, &dbf) == R_OK) {
            rc = rx_iterate(ctx, s, dbf, 1, rpt_rec_cb, &ud, NULL);
        } else {
            rc = rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
        }
    }
    /* trailing group footer + page footer + summary */
    if (e.gf && e.have_group) {
        var_publish(&e);
        eng_ensure(&e, e.gf->height);
        e.cur_y = render_band(&e, e.gf, e.cur_y);
    }
    var_publish(&e);
    const rpt_band* sf = rptf_band(&rf, RB_SUMMARY);
    if (sf) {
        eng_ensure(&e, sf->height);
        e.cur_y = render_band(&e, sf, e.cur_y);
    }
    const rpt_band* pf = rptf_band(&rf, RB_PAGEFOOTER);
    if (pf && !e.plain) {
        eng_ensure(&e, pf->height);
        e.cur_y = render_band(&e, pf, e.cur_y);
    }

    free(e.v_sum);
    free(e.v_cnt);
    for (size_t i = 0; i < e.nvars; i++) {
        rx_rval_free(&e.v_min[i]);
        rx_rval_free(&e.v_max[i]);
    }
    free(e.v_min);
    free(e.v_max);
    free(e.v_set);
    free(e.heading);
    rx_expr_free(e.group_e);
    rx_rval_free(&e.group_key);
    rptf_free(&rf);
    return rc;
}
