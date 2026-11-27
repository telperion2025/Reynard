/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file rptfmt.c
 * @brief Report (.FRM) / label (.RLB) text format implementation (spec
 *        9.3/9.4, M8). Canonical serializer + parser; see docs/formats.md
 *        section 7 for the grammar.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/rptfmt.h"

void rptf_init(rpt_file *rf, const char *name, int is_label)
{
    memset(rf, 0, sizeof(*rf));
    snprintf(rf->name, sizeof(rf->name), "%s", name ? name : (is_label ? "LABEL" : "REPORT"));
    rf->is_label = is_label;
    rf->page_w = 80;
    rf->page_h = 66;
    rf->margin_l = 0;
    rf->margin_r = 0;
    rf->margin_t = 0;
    rf->margin_b = 0;
    if (is_label) {
        snprintf(rf->stock, sizeof(rf->stock), "USER");
        rf->lab_h = 5;
        rf->lab_w = 35;
        rf->lab_sp_r = 0;
        rf->lab_sp_c = 2;
        rf->lab_m_l = 0;
        rf->lab_m_t = 0;
    }
}

void rptf_free(rpt_file *rf)
{
    for (size_t i = 0; i < rf->nbands; i++) {
        free(rf->bands[i].objs);
    }
    free(rf->bands);
    free(rf->vars);
    rf->bands = NULL;
    rf->vars = NULL;
    rf->nbands = rf->capbands = 0;
    rf->nvars = rf->capvars = 0;
}

const char *rptf_band_name(int t)
{
    switch (t) {
    case RB_TITLE: return "TITLE";
    case RB_PAGEHEADER: return "PAGEHEADER";
    case RB_GROUPHEADER: return "GROUPHEADER";
    case RB_DETAIL: return "DETAIL";
    case RB_GROUPFOOTER: return "GROUPFOOTER";
    case RB_PAGEFOOTER: return "PAGEFOOTER";
    case RB_SUMMARY: return "SUMMARY";
    default: return "?";
    }
}

int rptf_band_type(const char *s)
{
    if (!strcmp(s, "TITLE")) return RB_TITLE;
    if (!strcmp(s, "PAGEHEADER")) return RB_PAGEHEADER;
    if (!strcmp(s, "GROUPHEADER")) return RB_GROUPHEADER;
    if (!strcmp(s, "DETAIL")) return RB_DETAIL;
    if (!strcmp(s, "GROUPFOOTER")) return RB_GROUPFOOTER;
    if (!strcmp(s, "PAGEFOOTER")) return RB_PAGEFOOTER;
    if (!strcmp(s, "SUMMARY")) return RB_SUMMARY;
    return 0;
}

const char *rptf_agg_name(int a)
{
    switch (a) {
    case RV_SUM: return "SUM";
    case RV_COUNT: return "COUNT";
    case RV_AVG: return "AVG";
    case RV_MIN: return "MIN";
    case RV_MAX: return "MAX";
    default: return "NONE";
    }
}

int rptf_agg_from_name(const char *s)
{
    if (!strcmp(s, "SUM")) return RV_SUM;
    if (!strcmp(s, "COUNT")) return RV_COUNT;
    if (!strcmp(s, "AVG")) return RV_AVG;
    if (!strcmp(s, "MIN")) return RV_MIN;
    if (!strcmp(s, "MAX")) return RV_MAX;
    return RV_NONE;
}

rpt_band *rptf_band(const rpt_file *rf, int type)
{
    for (size_t i = 0; i < rf->nbands; i++) {
        if (rf->bands[i].type == type) {
            return (rpt_band *)&rf->bands[i];
        }
    }
    return NULL;
}

rpt_band *rptf_band_add(rpt_file *rf, int type, int height)
{
    if (rf->nbands == rf->capbands) {
        size_t nc = rf->capbands ? rf->capbands * 2 : 8;
        rpt_band *nb = realloc(rf->bands, nc * sizeof(rpt_band));
        if (!nb) {
            RX_FATAL("out of memory");
        }
        rf->bands = nb;
        rf->capbands = nc;
    }
    rpt_band *b = &rf->bands[rf->nbands++];
    memset(b, 0, sizeof(*b));
    b->type = type;
    b->height = height;
    return b;
}

rpt_obj *rptf_obj_add(rpt_band *b)
{
    if (b->nobjs == b->capobjs) {
        size_t nc = b->capobjs ? b->capobjs * 2 : 8;
        rpt_obj *no = realloc(b->objs, nc * sizeof(rpt_obj));
        if (!no) {
            RX_FATAL("out of memory");
        }
        b->objs = no;
        b->capobjs = nc;
    }
    rpt_obj *o = &b->objs[b->nobjs++];
    memset(o, 0, sizeof(*o));
    return o;
}

/* ---------------- serializer ---------------- */

static void put_q(strbuf *sb, const char *s)
{
    sb_putc(sb, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            sb_putc(sb, '\\');
        }
        sb_putc(sb, *p);
    }
    sb_putc(sb, '"');
}

int rptf_serialize(const rpt_file *rf, strbuf *sb)
{
    sb_printf(sb, "REYNARD %s 1\n", rf->is_label ? "LABEL" : "REPORT");
    sb_printf(sb, "NAME %s\n", rf->name);
    if (rf->is_label) {
        sb_printf(sb, "STOCK %s\n", rf->stock);
        sb_printf(sb, "SIZE %d,%d SPACING %d,%d MARGINS %d,%d\n", rf->lab_h, rf->lab_w, rf->lab_sp_r,
                  rf->lab_sp_c, rf->lab_m_l, rf->lab_m_t);
    } else {
        sb_printf(sb, "PAGE %d,%d\n", rf->page_w, rf->page_h);
        sb_printf(sb, "MARGIN %d,%d,%d,%d\n", rf->margin_l, rf->margin_r, rf->margin_t, rf->margin_b);
    }
    for (size_t i = 0; i < rf->nbands; i++) {
        const rpt_band *b = &rf->bands[i];
        if (b->type == RB_GROUPHEADER || b->type == RB_GROUPFOOTER) {
            sb_printf(sb, "BAND %s %d \"%s\"\n", rptf_band_name(b->type), b->height, b->group);
        } else {
            sb_printf(sb, "BAND %s %d\n", rptf_band_name(b->type), b->height);
        }
        for (size_t j = 0; j < b->nobjs; j++) {
            const rpt_obj *o = &b->objs[j];
            switch (o->type) {
            case RO_TEXT:
                sb_printf(sb, "OBJ TEXT %d,%d ", o->row, o->col);
                put_q(sb, o->text);
                sb_putc(sb, '\n');
                break;
            case RO_FIELD:
                sb_printf(sb, "OBJ FIELD %d,%d %s\n", o->row, o->col, o->text);
                break;
            case RO_LINE:
                sb_printf(sb, "OBJ LINE %d,%d TO %d,%d\n", o->row, o->col, o->row2, o->col2);
                break;
            case RO_BOX:
                sb_printf(sb, "OBJ BOX %d,%d TO %d,%d\n", o->row, o->col, o->row2, o->col2);
                break;
            }
        }
    }
    for (size_t i = 0; i < rf->nvars; i++) {
        const rpt_var *v = &rf->vars[i];
        const char *rs = v->reset == 3 ? "GROUP" : v->reset == 2 ? "PAGE" : "REPORT";
        sb_printf(sb, "VAR %s %s %s RESET %s", v->name, rptf_agg_name(v->agg), v->expr, rs);
        if (v->init[0]) {
            sb_printf(sb, " INITIAL %s", v->init);
        }
        sb_putc(sb, '\n');
    }
    sb_puts(sb, "END\n");
    return R_OK;
}

/* ---------------- parser ---------------- */

typedef struct {
    char tok[320];
} rtok;

static size_t rtok_split(char *line, rtok *toks, size_t max)
{
    size_t n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        char *out = toks[n].tok;
        size_t ol = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && ol < sizeof(toks[n].tok) - 1) {
                if (*p == '\\' && p[1]) {
                    p++;
                }
                out[ol++] = *p++;
            }
            if (*p == '"') {
                p++;
            }
        } else {
            while (*p && *p != ' ' && *p != '\t' && ol < sizeof(toks[n].tok) - 1) {
                out[ol++] = *p++;
            }
        }
        out[ol] = '\0';
        n++;
    }
    return n;
}

/* rest of line after token i (raw expr) */
static void rpt_rest(char *line, rtok *toks, size_t n, size_t i, char *out, size_t cap)
{
    /* walk the original line past the i-th token */
    char *p = line;
    for (size_t k = 0; k <= i && p; k++) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                p++;
            }
            if (*p) {
                p++;
            }
        } else {
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
        }
        if (k == i) {
            break;
        }
    }
    (void)toks;
    (void)n;
    while (*p == ' ') {
        p++;
    }
    snprintf(out, cap, "%.*s", (int)(cap - 1), p);
}

int rptf_load_text(const char *text, rpt_file *rf)
{
    int is_label = 0;
    char name[64] = "";
    rptf_init(rf, "REPORT", 0);
    char *copy = rx_strdup(text);
    char *save = NULL;
    rpt_band *cur = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) {
            line[--l] = '\0';
        }
        if (!l || line[0] == '#') {
            continue;
        }
        if (strncmp(line, "REYNARD REPORT", 14) == 0 || strncmp(line, "REYNARD LABEL", 13) == 0) {
            is_label = (line[8] == 'L');
            continue;
        }
        if (strncmp(line, "NAME ", 5) == 0) {
            snprintf(name, sizeof(name), "%.63s", line + 5);
            continue;
        }
        if (strncmp(line, "PAGE ", 5) == 0) {
            sscanf(line + 5, "%d,%d", &rf->page_w, &rf->page_h);
            continue;
        }
        if (strncmp(line, "MARGIN ", 7) == 0) {
            sscanf(line + 7, "%d,%d,%d,%d", &rf->margin_l, &rf->margin_r, &rf->margin_t, &rf->margin_b);
            continue;
        }
        if (strncmp(line, "STOCK ", 6) == 0) {
            snprintf(rf->stock, sizeof(rf->stock), "%.31s", line + 6);
            continue;
        }
        if (strncmp(line, "SIZE ", 5) == 0) {
            int sp_r = 0, sp_c = 0, m_l = 0, m_t = 0;
            if (sscanf(line + 5, "%d,%d SPACING %d,%d MARGINS %d,%d", &rf->lab_h, &rf->lab_w, &sp_r,
                       &sp_c, &m_l, &m_t) >= 2) {
                rf->lab_sp_r = sp_r;
                rf->lab_sp_c = sp_c;
                rf->lab_m_l = m_l;
                rf->lab_m_t = m_t;
            }
            continue;
        }
        if (strncmp(line, "BAND ", 5) == 0) {
            rtok tk[8];
            size_t n = rtok_split(line + 5, tk, 8);
            int bt = rptf_band_type(tk[0].tok);
            if (!bt || n < 2) {
                free(copy);
                return R_ERR_SYNTAX;
            }
            cur = rptf_band_add(rf, bt, atoi(tk[1].tok));
            if ((bt == RB_GROUPHEADER || bt == RB_GROUPFOOTER) && n >= 3) {
                snprintf(cur->group, sizeof(cur->group), "%.159s", tk[2].tok);
            }
            continue;
        }
        if (strncmp(line, "OBJ ", 4) == 0) {
            if (!cur) {
                free(copy);
                return R_ERR_SYNTAX;
            }
            rtok tk[8];
            size_t n = rtok_split(line + 4, tk, 8);
            if (n < 2) {
                free(copy);
                return R_ERR_SYNTAX;
            }
            rpt_obj *o = rptf_obj_add(cur);
            if (!strcmp(tk[0].tok, "TEXT")) {
                o->type = RO_TEXT;
                sscanf(tk[1].tok, "%d,%d", &o->row, &o->col);
                if (n >= 3) {
                    snprintf(o->text, sizeof(o->text), "%.255s", tk[2].tok);
                }
            } else if (!strcmp(tk[0].tok, "FIELD")) {
                o->type = RO_FIELD;
                sscanf(tk[1].tok, "%d,%d", &o->row, &o->col);
                rpt_rest(line, tk, n, 2, o->text, sizeof(o->text));
            } else if (!strcmp(tk[0].tok, "LINE")) {
                o->type = RO_LINE;
                sscanf(tk[1].tok, "%d,%d", &o->row, &o->col);
                if (n >= 4 && !strcmp(tk[2].tok, "TO")) {
                    sscanf(tk[3].tok, "%d,%d", &o->row2, &o->col2);
                }
            } else if (!strcmp(tk[0].tok, "BOX")) {
                o->type = RO_BOX;
                sscanf(tk[1].tok, "%d,%d", &o->row, &o->col);
                if (n >= 4 && !strcmp(tk[2].tok, "TO")) {
                    sscanf(tk[3].tok, "%d,%d", &o->row2, &o->col2);
                }
            } else {
                free(copy);
                return R_ERR_SYNTAX;
            }
            continue;
        }
        if (strncmp(line, "VAR ", 4) == 0) {
            rtok tk[8];
            size_t n = rtok_split(line + 4, tk, 8);
            if (n < 5 || strcmp(tk[3].tok, "RESET") != 0) {
                free(copy);
                return R_ERR_SYNTAX;
            }
            if (rf->nvars == rf->capvars) {
                size_t nc = rf->capvars ? rf->capvars * 2 : 8;
                rpt_var *nv = realloc(rf->vars, nc * sizeof(rpt_var));
                if (!nv) {
                    RX_FATAL("out of memory");
                }
                rf->vars = nv;
                rf->capvars = nc;
            }
            rpt_var *v = &rf->vars[rf->nvars++];
            memset(v, 0, sizeof(*v));
            snprintf(v->name, sizeof(v->name), "%.63s", tk[0].tok);
            v->agg = rptf_agg_from_name(tk[1].tok);
            /* expr is raw text up to the RESET keyword (may contain spaces) */
            char tail[320];
            rpt_rest(line, tk, n, 2, tail, sizeof(tail));
            char *rs = strstr(tail, " RESET ");
            const char *resetw = "REPORT";
            char *init = NULL;
            if (rs) {
                *rs = '\0';
                size_t tl = strlen(tail);
                while (tl && tail[tl - 1] == ' ') {
                    tail[--tl] = '\0';
                }
                resetw = rs + 7;
                char *in = strstr(resetw, " INITIAL ");
                if (in) {
                    init = in + 9;
                    *in = '\0';
                    /* reset word ends here */
                    char *sp = strchr(resetw, ' ');
                    if (sp) {
                        *sp = '\0';
                    }
                }
            }
            snprintf(v->expr, sizeof(v->expr), "%.159s", tail);
            v->reset = !strcmp(resetw, "GROUP") ? 3 : !strcmp(resetw, "PAGE") ? 2 : 1;
            if (init) {
                snprintf(v->init, sizeof(v->init), "%.95s", init);
            }
            continue;
        }
        if (strcmp(line, "END") == 0) {
            break;
        }
        free(copy);
        return R_ERR_SYNTAX;
    }
    free(copy);
    rf->is_label = is_label;
    if (name[0]) {
        snprintf(rf->name, sizeof(rf->name), "%s", name);
    }
    return R_OK;
}

static char *rpt_read_all(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    strbuf sb;
    sb_init(&sb);
    char tmp[4096];
    size_t got;
    while ((got = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        sb_put(&sb, tmp, got);
    }
    fclose(f);
    if (n) {
        *n = sb.len;
    }
    if (!sb.buf) {
        sb.buf = rx_strdup("");
    }
    sb.buf[sb.len] = '\0';
    return sb.buf;
}

int rptf_load(const char *path, rpt_file *rf)
{
    size_t n = 0;
    char *text = rpt_read_all(path, &n);
    if (!text) {
        return R_ERR_IO_NOTFOUND;
    }
    int rc = rptf_load_text(text, rf);
    free(text);
    return rc;
}

int rptf_save(const char *path, const rpt_file *rf)
{
    strbuf sb;
    sb_init(&sb);
    rptf_serialize(rf, &sb);
    FILE *f = fopen(path, "w");
    if (!f) {
        sb_free(&sb);
        return R_ERR_IO_OPEN;
    }
    int rc = fwrite(sb.buf ? sb.buf : "", 1, sb.len, f) == sb.len ? R_OK : R_ERR_IO_WRITE;
    fclose(f);
    sb_free(&sb);
    return rc;
}
