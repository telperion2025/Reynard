/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_env.c
 * @brief Environment commands: ?/??, assignment, STORE, SET, QUIT, CLEAR,
 *        BROWSE (NOUI in batch). Spec §7.1 environment family.
 */
#include "lang/funcs.h"
#include "lang/lang.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "reynard/dbf.h"
#include "util/util.h"

int rx_cmd_print(rx_ctx* ctx, rx_stmt* s, int newline) {
    for (size_t i = 0; i < s->nexpr; i++) {
        rval v;
        rx_rval_undef(&v);
        int rc = rx_eval(ctx, s->exprs[i], &v, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        if (i > 0) {
            rx_out(ctx, " ", 1); /* comma separates ? expressions with one space */
        }
        strbuf sb;
        sb_init(&sb);
        rx_vm_display(ctx, &v, &sb);
        if (sb.buf) {
            rx_out(ctx, sb.buf, sb.len);
        }
        sb_free(&sb);
        rx_rval_free(&v);
    }
    if (newline) {
        rx_out(ctx, "\n", 1);
    }
    return R_OK;
}

int rx_cmd_assign(rx_ctx* ctx, rx_stmt* s) {
    if (s->nidx > 0) {
        /* M5 array element: a[i] = v or a[i,j] = v (index exprs first, value last) */
        double di = 1, dj = 1;
        for (size_t i = 0; i < s->nidx; i++) {
            rval iv;
            rx_rval_undef(&iv);
            int rc = rx_eval(ctx, s->exprs[i], &iv, &ctx->err);
            if (rc != R_OK) {
                return rc;
            }
            double d;
            int ok = rv_to_num(&iv, &d);
            rx_rval_free(&iv);
            if (!ok) {
                return rx_raise(ctx, R_ERR_TYPE, s->line, s->col, "Array index must be numeric");
            }
            if (i == 0) {
                di = d;
            } else {
                dj = d;
            }
        }
        rval v;
        rx_rval_undef(&v);
        int rc = rx_eval(ctx, s->exprs[s->nidx], &v, &ctx->err);
        if (rc != R_OK) {
            return rc;
        }
        rc = rx_array_set(ctx, s->target, (size_t)(di < 1 ? 1 : di), (size_t)(dj < 1 ? 1 : dj), &v);
        rx_rval_free(&v);
        if (rc == R_ERR_TYPE) {
            return rx_raise(ctx, R_ERR_TYPE, s->line, s->col, "Variable is not an array");
        }
        if (rc == R_ERR_RANGE) {
            return rx_raise(ctx, R_ERR_RANGE, s->line, s->col, "Array index out of range");
        }
        return rc;
    }
    rval v;
    rx_rval_undef(&v);
    int rc = rx_eval(ctx, s->exprs[0], &v, &ctx->err);
    if (rc != R_OK) {
        return rc;
    }
    rc = rx_memvar_set(ctx, s->target, &v);
    rx_rval_free(&v);
    return rc;
}

int rx_cmd_store(rx_ctx* ctx, rx_stmt* s) {
    rval v;
    rx_rval_undef(&v);
    int rc = rx_eval(ctx, s->exprs[0], &v, &ctx->err);
    if (rc != R_OK) {
        return rc;
    }
    for (size_t i = 0; i < s->nstrs; i++) {
        rc = rx_memvar_set(ctx, s->strs[i], &v);
        if (rc != R_OK) {
            break;
        }
    }
    rx_rval_free(&v);
    return rc;
}

/* Appendix B full v1.0 keyword list: accepted by the parser now, semantics
   land per milestone (see docs/language.md status table). */
static const char* set_known[] = {
    "ALTERNATE",  "ANSI",      "BELL",       "BLINK",      "BLOCKSIZE",  "BORDER",   "CARRY",
    "CENTURY",    "CLEAR",     "CLOCK",      "COLOR",      "COMPATIBLE", "CONFIRM",  "CONSOLE",
    "CURRENCY",   "CURSOR",    "DATE",       "DEBUG",      "DECIMALS",   "DEFAULT",  "DELETED",
    "DELIMITERS", "DEVICE",    "DISPLAY",    "DOHISTORY",  "ECHO",       "ESCAPE",   "EXACT",
    "EXCLUSIVE",  "FIELDS",    "FILTER",     "FIXED",      "FORMAT",     "FULLPATH", "FUNCTION",
    "HEADINGS",   "HELP",      "HOURS",      "INTENSITY",  "KEY",        "KEYCOMP",  "LOCK",
    "LOGERRORS",  "MACKEY",    "MARGIN",     "MARK",       "MEMOWIDTH",  "MESSAGE",  "MOUSE",
    "MULTILOCKS", "NEAR",      "NOTIFY",     "ODOMETER",   "OPTIMIZE",   "ORDER",    "PALETTE",
    "PATH",       "PDSETUP",   "POINT",      "PRINTER",    "PROCEDURE",  "REFRESH",  "REPROCESS",
    "RESOURCE",   "SAFETY",    "SCOREBOARD", "SEPARATOR",  "SHADOWS",    "SKIP",     "SPACE",
    "STATUS",     "STEP",      "STICKY",     "SYSFORMATS", "SYSMENU",    "TALK",     "TEXTMERGE",
    "TOPIC",      "TYPEAHEAD", "UDFPARMS",   "UNIQUE",     "VIEW",
};

static int set_known_p(const char* w) {
    for (size_t i = 0; i < sizeof(set_known) / sizeof(set_known[0]); i++) {
        if (rx_strcasecmp(w, set_known[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int parse_on_off(const char* w, int* out) {
    if (rx_strcasecmp(w, "ON") == 0) {
        *out = 1;
        return 1;
    }
    if (rx_strcasecmp(w, "OFF") == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

/* ---- SET RELATION / SET SKIP (spec §8.1, M4) ---- */

int rx_rel_alias_area(const char* alias) {
    for (int i = 1; i <= 255; i++) {
        if (rx_wa_used(i)) {
            const char* a;
            rx_wa_alias(i, &a);
            if (rx_strcasecmp(a, alias) == 0) {
                return i;
            }
        }
    }
    return -1;
}

void rx_rel_apply(rx_ctx* ctx, int driver, int via_skip) {
    if (ctx->nrels == 0) {
        return;
    }
    int saved = rx_wa_current();
    for (size_t i = 0; i < ctx->nrels; i++) {
        rx_rel* r = &ctx->rels[i];
        if (r->driver != driver) {
            continue;
        }
        if (via_skip && r->target >= 1 && r->target <= 255 && ctx->skip_to[r->target]) {
            continue; /* SET SKIP TO: one-to-many — target holds its position on skips */
        }
        rx_dbf* target;
        if (rx_wa_dbf(r->target, &target) != R_OK) {
            continue;
        }
        rx_wa_select(driver);
        rval v;
        rx_rval_undef(&v);
        if (rx_eval(ctx, r->e, &v, &ctx->err) != R_OK) {
            rx_rval_free(&v);
            continue;
        }
        if (rx_index_order_active(target)) {
            /* M6 §8.4: index-keyed relation — SEEK the key in the target order */
            uint32_t recno = 0;
            int found = 0;
            if (rx_index_seek(target, &v, ctx->set.exact, &recno, &found) == R_OK && found) {
                rx_dbf_goto(target, recno);
            } else {
                uint32_t n;
                rx_dbf_recount(target, &n);
                if (n) {
                    rx_dbf_goto(target, n);
                    rx_dbf_skip(target, 1); /* key past end: EOF */
                } else {
                    rx_dbf_goto(target, 1);
                }
            }
            rx_rval_free(&v);
        } else {
            double d = 0;
            rv_to_num(&v, &d);
            rx_rval_free(&v);
            uint32_t n;
            rx_dbf_recount(target, &n);
            long k = (long)d;
            if (k < 1) {
                k = 1; /* key below range: target at BOF */
            }
            if (n && k > (long)n) {
                k = (long)n + 1; /* key past end: target at EOF */
            }
            rx_dbf_goto(target, (uint32_t)k);
        }
    }
    rx_wa_select(saved);
}

static int rel_clause_present(rx_stmt* s) {
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "REL") == 0 || strcmp(s->clauses[i].key, "REL_OFF") == 0 ||
            strcmp(s->clauses[i].key, "REL_CLEAR") == 0 || strcmp(s->clauses[i].key, "SKIP") == 0 ||
            strcmp(s->clauses[i].key, "SKIP_CLEAR") == 0 ||
            strcmp(s->clauses[i].key, "REL_ADD") == 0) {
            return 1;
        }
    }
    return 0;
}

static int rx_cmd_set_relations(rx_ctx* ctx, rx_stmt* s) {
    int additive = 0;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "REL_ADD") == 0) {
            additive = 1;
        }
    }
    int driver = rx_wa_current();
    for (size_t i = 0; i < s->nclause; i++) {
        rx_clause* c = &s->clauses[i];
        if (strcmp(c->key, "REL_CLEAR") == 0) {
            /* SET RELATION TO (no args): drop all relations driven here */
            size_t w = 0;
            for (size_t j = 0; j < ctx->nrels; j++) {
                if (ctx->rels[j].driver == driver) {
                    rx_expr_free(ctx->rels[j].e);
                    free(ctx->rels[j].text);
                } else {
                    ctx->rels[w++] = ctx->rels[j];
                }
            }
            ctx->nrels = w;
        } else if (strcmp(c->key, "REL_OFF") == 0) {
            int area = rx_rel_alias_area(c->word);
            if (area < 0) {
                return rx_raise(ctx, R_ERR_FIELD, s->line, s->col, "Alias not found");
            }
            size_t w = 0;
            for (size_t j = 0; j < ctx->nrels; j++) {
                if (ctx->rels[j].driver == driver && ctx->rels[j].target == area) {
                    rx_expr_free(ctx->rels[j].e);
                    free(ctx->rels[j].text);
                } else {
                    ctx->rels[w++] = ctx->rels[j];
                }
            }
            ctx->nrels = w;
        } else if (strcmp(c->key, "REL") == 0) {
            int area = rx_rel_alias_area(c->word);
            if (area < 0) {
                rx_expr_free(c->e);
                free(c->text);
                c->e = NULL;
                c->text = NULL;
                return rx_raise(ctx, R_ERR_FIELD, s->line, s->col, "Alias not found");
            }
            if (!additive && ctx->nrels == 0) {
                /* first relation without ADDITIVE: FoxPro clears existing ones */
            }
            if (!additive) {
                size_t w = 0;
                for (size_t j = 0; j < ctx->nrels; j++) {
                    if (ctx->rels[j].driver == driver) {
                        rx_expr_free(ctx->rels[j].e);
                        free(ctx->rels[j].text);
                    } else {
                        ctx->rels[w++] = ctx->rels[j];
                    }
                }
                ctx->nrels = w;
                additive = 1; /* only clear once per statement */
            }
            if (ctx->nrels == ctx->caprels) {
                size_t nc = ctx->caprels ? ctx->caprels * 2 : 4;
                rx_rel* nr = realloc(ctx->rels, nc * sizeof(rx_rel));
                if (!nr) {
                    RX_FATAL("out of memory");
                }
                ctx->rels = nr;
                ctx->caprels = nc;
            }
            ctx->rels[ctx->nrels].driver = driver;
            ctx->rels[ctx->nrels].e = c->e;       /* take ownership */
            ctx->rels[ctx->nrels].text = c->text; /* M6: RELATION() text */
            c->e = NULL;
            c->text = NULL;
            ctx->rels[ctx->nrels].target = area;
            ctx->nrels++;
            c->e = NULL;
        } else if (strcmp(c->key, "SKIP_CLEAR") == 0) {
            memset(ctx->skip_to, 0, sizeof(ctx->skip_to));
        } else if (strcmp(c->key, "SKIP") == 0) {
            int area = rx_rel_alias_area(c->word);
            if (area < 0) {
                return rx_raise(ctx, R_ERR_FIELD, s->line, s->col, "Alias not found");
            }
            ctx->skip_to[area] = 1;
        }
    }
    return R_OK;
}

int rx_cmd_set(rx_ctx* ctx, rx_stmt* s) {
    int has_filter_clause = 0;
    int has_idx_clause = 0, has_ord_clause = 0;
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "FILTER") == 0) {
            has_filter_clause = 1;
        }
        if (strcmp(s->clauses[i].key, "IDX") == 0 || strcmp(s->clauses[i].key, "IDX_CLEAR") == 0 ||
            strcmp(s->clauses[i].key, "IDX_ADD") == 0) {
            has_idx_clause = 1;
        }
        if (strncmp(s->clauses[i].key, "ORD_", 4) == 0) {
            has_ord_clause = 1;
        }
    }
    if (rel_clause_present(s)) {
        return rx_cmd_set_relations(ctx, s);
    }
    if (has_idx_clause) {
        return rx_cmd_set_index(ctx, s); /* M6 §8.4: SET INDEX TO */
    }
    if (has_ord_clause) {
        return rx_cmd_set_order(ctx, s); /* M6 §8.4: SET ORDER TO */
    }
    if (s->nstrs == 0 && !has_filter_clause) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "SET requires an option");
    }
    if (has_filter_clause) {
        /* SET FILTER TO [expr]: parser stored the expr as a FILTER clause */
        for (size_t i = 0; i < s->nclause; i++) {
            if (strcmp(s->clauses[i].key, "FILTER") == 0) {
                if (ctx->filter_on || ctx->filter.ops) {
                    rx_code_free(&ctx->filter);
                    memset(&ctx->filter, 0, sizeof(ctx->filter));
                    ctx->filter_on = 0;
                }
                if (s->clauses[i].e) {
                    int rc = rx_compile_expr(s->clauses[i].e, &ctx->filter, &ctx->err);
                    if (rc != R_OK) {
                        return rc;
                    }
                    ctx->filter_on = 1;
                    free(ctx->filter_text);
                    ctx->filter_text = s->auxtext ? rx_strdup(s->auxtext) : NULL; /* FILTER() */
                } else {
                    free(ctx->filter_text);
                    ctx->filter_text = NULL;
                }
            }
        }
        return R_OK;
    }
    const char* opt = s->strs[0];
    if (!set_known_p(opt)) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "Unknown SET option %s", opt);
    }
    /* forms: SET X ON|OFF | SET X TO v | SET X v */
    int val = -1;
    const char* toval = NULL;
    if (s->nstrs >= 2) {
        if (rx_strcasecmp(s->strs[1], "TO") == 0) {
            if (s->nstrs >= 3) {
                toval = s->strs[2];
            }
        } else {
            int b;
            if (parse_on_off(s->strs[1], &b)) {
                val = b;
            } else {
                toval = s->strs[1];
            }
        }
    }
    if (rx_strcasecmp(opt, "TALK") == 0 && val >= 0) {
        ctx->set.talk = val;
    } else if (rx_strcasecmp(opt, "HEADINGS") == 0 && val >= 0) {
        ctx->set.headings = val;
    } else if (rx_strcasecmp(opt, "EXACT") == 0 && val >= 0) {
        ctx->set.exact = val;
    } else if (rx_strcasecmp(opt, "FIXED") == 0 && val >= 0) {
        ctx->set.fixed = val;
    } else if (rx_strcasecmp(opt, "CENTURY") == 0 && val >= 0) {
        ctx->set.century = val;
    } else if (rx_strcasecmp(opt, "DELETED") == 0 && val >= 0) {
        ctx->set.deleted = val;
    } else if (rx_strcasecmp(opt, "ESCAPE") == 0 && val >= 0) {
        ctx->set.escape = val;
    } else if (rx_strcasecmp(opt, "CONSOLE") == 0 && val >= 0) {
        ctx->set.console = val;
    } else if (rx_strcasecmp(opt, "SAFETY") == 0 && val >= 0) {
        ctx->set.safety = val;
    } else if (rx_strcasecmp(opt, "CLOCK") == 0 && val >= 0) {
        ctx->set.clock = val;
    } else if (rx_strcasecmp(opt, "MOUSE") == 0 && val >= 0) {
        ctx->set.mouse = val;
    } else if (rx_strcasecmp(opt, "DECIMALS") == 0 && toval) {
        long d = strtol(toval, NULL, 10);
        if (d < 0 || d > 15) {
            return rx_raise(ctx, R_ERR_RANGE, s->line, s->col, "SET DECIMALS out of range");
        }
        ctx->set.decimals = (int)d;
    } else if (rx_strcasecmp(opt, "DEFAULT") == 0 && toval) {
        snprintf(ctx->set.defdir, sizeof(ctx->set.defdir), "%s", toval);
    } else if (rx_strcasecmp(opt, "PATH") == 0 && toval) {
        snprintf(ctx->set.path, sizeof(ctx->set.path), "%s", toval);
    } else if (rx_strcasecmp(opt, "DATE") == 0 && toval) {
        ctx->set.date_fmt = 0; /* AMERICAN; other formats at M5 */
    } else if (rx_strcasecmp(opt, "EXCLUSIVE") == 0 && val >= 0) {
        ctx->set.exclusive = val;
    } else if (rx_strcasecmp(opt, "MULTILOCKS") == 0 && val >= 0) {
        ctx->set.multilocks = val;
    } else if (rx_strcasecmp(opt, "UNIQUE") == 0 && val >= 0) {
        ctx->set.unique = val; /* M6 §8.4: UNIQUE index builds */
    } else if (rx_strcasecmp(opt, "REPROCESS") == 0 && toval) {
        long v = strtol(toval, NULL, 10);
        if (rx_strcasecmp(toval, "AUTOMATIC") == 0) {
            v = -1;
        }
        ctx->set.reprocess = (int)v;
        rx_data_set_reprocess((int)v);
    } else if (rx_strcasecmp(opt, "HELP") == 0 && toval) {
        /* M9 9.7: SET HELP TO <help.dbf>; SET HELP ON/OFF accepted, ignored */
        snprintf(ctx->set.helpfile, sizeof(ctx->set.helpfile), "%s", toval);
    } else if (rx_strcasecmp(opt, "PROCEDURE") == 0) {
        /* M9 9.6: SET PROCEDURE TO <file.prg|app.rpp> */
        return rx_cmd_set_proc(ctx, s);
    }
    /* other Appendix B options are accepted and stored for their milestone */
    return R_OK;
}

int rx_cmd_browse(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = rx_wa_dbf(0, &dbf);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
    }
    if (!ctx->browse_fn) {
        return rx_raise(ctx, R_ERR_NOUI, 0, 0, "BROWSE requires the interactive UI");
    }
    /* resolve plain field columns against the current table now */
    for (size_t i = 0; i < s->nbcols; i++) {
        if (s->bcols[i].is_field) {
            size_t idx;
            if (rx_dbf_field_index(dbf, s->bcols[i].name, &idx) != R_OK) {
                return rx_raise(ctx, R_ERR_FIELD, s->line, s->col, "Unknown field %s",
                                s->bcols[i].name);
            }
            s->bcols[i].fldidx = idx;
        }
    }
    /* FREEZE names a field; resolve to a column in the UI after the default
       column set (no FIELDS clause) has been built */
    for (size_t i = 0; i < s->nclause; i++) {
        if (strcmp(s->clauses[i].key, "FREEZE") == 0) {
            size_t idx;
            if (rx_dbf_field_index(dbf, s->clauses[i].word, &idx) != R_OK) {
                return rx_raise(ctx, R_ERR_FIELD, s->line, s->col, "Unknown field %s",
                                s->clauses[i].word);
            }
            for (size_t k = 0; k < s->nbcols; k++) {
                if (s->bcols[k].is_field && s->bcols[k].fldidx == idx) {
                    s->bcols[k].frozen = 1;
                }
            }
        }
    }
    return ctx->browse_fn(ctx->browse_ud, s);
}
