/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file parser.c
 * @brief Hand-written recursive-descent parser (spec §7.2). One command per
 *        logical line; ';' continues a line. Keywords match on their first
 *        four letters (REPL == REPLACE); ambiguity is a syntax error.
 */
#include "lang/lang.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util/util.h"

typedef struct parser {
    rx_tok *toks;
    size_t n, pos;
    r_err *err;
    int failed;
    rx_stmt *stmts;
    size_t nstmt, capstmt;
    const char *src;  /**< source text (TEXT blocks), may be NULL */
    rx_stmt *cur;     /**< non-NULL: fill this kid instead of pushing (M5 blocks) */
} parser;

static int pk_op_ahead_is(parser *p, size_t k, int op);

static rx_tok *pk_cur(parser *p)
{
    return &p->toks[p->pos < p->n ? p->pos : p->n - 1];
}

static rx_tok *pk_ahead(parser *p, size_t k)
{
    size_t i = p->pos + k;
    return &p->toks[i < p->n ? i : p->n - 1];
}

static int pk_is(parser *p, tk_kind kind)
{
    return pk_cur(p)->kind == kind;
}

static int pk_op(parser *p, int op)
{
    return pk_cur(p)->kind == TK_OP && pk_cur(p)->ival == op;
}

static int pk_word_is(parser *p, const char *w)
{
    return pk_cur(p)->kind == TK_WORD && rx_strcasecmp(pk_cur(p)->text, w) == 0;
}

static void pk_adv(parser *p)
{
    if (p->pos < p->n - 1) {
        p->pos++;
    }
}

static int pk_fail(parser *p, int line, int col, const char *msg)
{
    if (!p->failed) {
        p->failed = 1;
        p->err->code = R_ERR_SYNTAX;
        p->err->line = line;
        p->err->col = col;
        snprintf(p->err->msg, sizeof(p->err->msg), "%s", msg);
    }
    return R_ERR_SYNTAX;
}

static int pk_expect_op(parser *p, int op, const char *what)
{
    if (!pk_op(p, op)) {
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, what);
    }
    pk_adv(p);
    return R_OK;
}

static rx_expr *ex_new(parser *p, ex_kind kind)
{
    rx_expr *e = calloc(1, sizeof(rx_expr));
    if (!e) {
        RX_FATAL("out of memory");
    }
    e->kind = kind;
    e->line = pk_cur(p)->line;
    e->col = pk_cur(p)->col;
    return e;
}

void rx_expr_free(rx_expr *e)
{
    if (!e) {
        return;
    }
    rx_expr_free(e->l);
    rx_expr_free(e->r);
    for (size_t i = 0; i < e->nargs; i++) {
        rx_expr_free(e->args[i]);
    }
    free(e->args);
    rx_rval_free(&e->lit);
    free(e);
}

/* ---- dot-delimited word: .AND. .T. etc. Returns word text or NULL. ---- */
static const char *pk_dot_word(parser *p)
{
    if (pk_op(p, OP_DOT) && pk_ahead(p, 1)->kind == TK_WORD && pk_op_ahead_is(p, 2, OP_DOT)) {
        return pk_ahead(p, 1)->text;
    }
    return NULL;
}

static int pk_op_ahead_is(parser *p, size_t k, int op)
{
    return pk_ahead(p, k)->kind == TK_OP && pk_ahead(p, k)->ival == op;
}

static int word_logic(parser *p, const char *w, rx_logic *out)
{
    (void)p;
    if (rx_strcasecmp(w, "T") == 0 || rx_strcasecmp(w, "Y") == 0) {
        *out = L_T;
        return 1;
    }
    if (rx_strcasecmp(w, "F") == 0 || rx_strcasecmp(w, "N") == 0) {
        *out = L_F;
        return 1;
    }
    return 0;
}

/* ============================ expressions ============================ */

static int parse_expr(parser *p, rx_expr **out);
static int parse_browse(parser *p, rx_stmt *s);

static int parse_primary(parser *p, rx_expr **out)
{
    rx_tok *t = pk_cur(p);
    if (t->kind == TK_NUM) {
        rx_expr *e = ex_new(p, EX_NUM);
        e->lit.t = RX_T_N;
        e->lit.u.n.d = t->num;
        e->lit.u.n.dec = 2;
        pk_adv(p);
        *out = e;
        return R_OK;
    }
    if (t->kind == TK_STR) {
        rx_expr *e = ex_new(p, EX_STR);
        rval_set_str(&e->lit, rx_strdup(t->text), strlen(t->text));
        pk_adv(p);
        *out = e;
        return R_OK;
    }
    if (t->kind == TK_WORD) {
        if (t->text[0] == '^' || (t->text[0] == '{' && t->text[1] == '}')) {
            /* date literal from {^...} */
            rx_expr *e = ex_new(p, EX_DATE);
            rval_set_str(&e->lit, rx_strdup(t->text), strlen(t->text));
            pk_adv(p);
            *out = e;
            return R_OK;
        }
        /* m.var */
        if ((rx_strcasecmp(t->text, "M") == 0) && pk_op_ahead_is(p, 1, OP_DOT) &&
            pk_ahead(p, 2)->kind == TK_WORD) {
            rx_expr *e = ex_new(p, EX_VAR);
            snprintf(e->name, sizeof(e->name), "M.%s", pk_ahead(p, 2)->text);
            for (char *s = e->name; *s; s++) {
                *s = (char)toupper((unsigned char)*s);
            }
            pk_adv(p);
            pk_adv(p);
            pk_adv(p);
            *out = e;
            return R_OK;
        }
        /* alias->field */
        if (pk_op_ahead_is(p, 1, OP_ARROW) && pk_ahead(p, 2)->kind == TK_WORD) {
            rx_expr *e = ex_new(p, EX_FIELD);
            snprintf(e->name, sizeof(e->name), "%s->%s", t->text, pk_ahead(p, 2)->text);
            for (char *s = e->name; *s; s++) {
                *s = (char)toupper((unsigned char)*s);
            }
            pk_adv(p);
            pk_adv(p);
            pk_adv(p);
            *out = e;
            return R_OK;
        }
        /* function call: name resolved at runtime (spec §7.3: TYPE()/
           EVALUATE()/& are runtime lookups; short-circuit .AND./.OR. must
           never evaluate the skipped side, spec §7.6) */
        if (pk_op_ahead_is(p, 1, OP_LP)) {
            {
                rx_expr *e = ex_new(p, EX_FUNC);
                e->fid = -1;
                snprintf(e->name, sizeof(e->name), "%s", t->text);
                for (char *c = e->name; *c; c++) {
                    *c = (char)toupper((unsigned char)*c);
                }
                pk_adv(p); /* word */
                pk_adv(p); /* ( */
                if (!pk_op(p, OP_RP)) {
                    for (;;) {
                        rx_expr *a = NULL;
                        int rc = parse_expr(p, &a);
                        if (rc != R_OK) {
                            rx_expr_free(e);
                            return rc;
                        }
                        rx_expr **na = realloc(e->args, (e->nargs + 1) * sizeof(rx_expr *));
                        if (!na) {
                            RX_FATAL("out of memory");
                        }
                        e->args = na;
                        e->args[e->nargs++] = a;
                        if (pk_op(p, OP_COMMA)) {
                            pk_adv(p);
                            continue;
                        }
                        break;
                    }
                }
                int rc = pk_expect_op(p, OP_RP, "Missing )");
                if (rc != R_OK) {
                    rx_expr_free(e);
                    return rc;
                }
                *out = e;
                return R_OK;
            }
        }
        /* array subscript */
        if (pk_op_ahead_is(p, 1, OP_LB)) {
            rx_expr *e = ex_new(p, EX_ARRAY);
            snprintf(e->name, sizeof(e->name), "%s", t->text);
            for (char *s = e->name; *s; s++) {
                *s = (char)toupper((unsigned char)*s);
            }
            pk_adv(p);
            pk_adv(p);
            for (;;) {
                rx_expr *a = NULL;
                int rc = parse_expr(p, &a);
                if (rc != R_OK) {
                    rx_expr_free(e);
                    return rc;
                }
                rx_expr **na = realloc(e->args, (e->nargs + 1) * sizeof(rx_expr *));
                if (!na) {
                    RX_FATAL("out of memory");
                }
                e->args = na;
                e->args[e->nargs++] = a;
                if (pk_op(p, OP_COMMA)) {
                    pk_adv(p);
                    continue;
                }
                break;
            }
            int rc = pk_expect_op(p, OP_RB, "Missing ]");
            if (rc != R_OK) {
                rx_expr_free(e);
                return rc;
            }
            *out = e;
            return R_OK;
        }
        /* bare identifier: memvar or field */
        rx_expr *e = ex_new(p, EX_VAR);
        snprintf(e->name, sizeof(e->name), "%s", t->text);
        for (char *s = e->name; *s; s++) {
            *s = (char)toupper((unsigned char)*s);
        }
        pk_adv(p);
        *out = e;
        return R_OK;
    }
    if (pk_op(p, OP_LP)) {
        pk_adv(p);
        int rc = parse_expr(p, out);
        if (rc != R_OK) {
            return rc;
        }
        return pk_expect_op(p, OP_RP, "Missing )");
    }
    const char *dw = pk_dot_word(p);
    if (dw) {
        rx_logic lg;
        if (word_logic(p, dw, &lg)) {
            rx_expr *e = ex_new(p, EX_LOGIC);
            e->lit.t = RX_T_L;
            e->lit.u.l = lg;
            pk_adv(p);
            pk_adv(p);
            pk_adv(p);
            *out = e;
            return R_OK;
        }
    }
    if (pk_op(p, OP_Q)) { /* ? used inside expression: error */
        return pk_fail(p, t->line, t->col, "Syntax error");
    }
    return pk_fail(p, t->line, t->col, "Syntax error");
}

static int parse_unary(parser *p, rx_expr **out)
{
    if (pk_op(p, OP_AMP)) {
        pk_adv(p); /* & : textual macro; the stmt raw span re-parses at exec */
        return parse_unary(p, out);
    }
    if (pk_op(p, OP_MINUS) || pk_op(p, OP_PLUS)) {
        int op = pk_cur(p)->ival;
        int line = pk_cur(p)->line, col = pk_cur(p)->col;
        pk_adv(p);
        rx_expr *e = ex_new(p, EX_UN);
        e->op = op;
        e->line = line;
        e->col = col;
        if (parse_unary(p, &e->r) != R_OK) {
            rx_expr_free(e);
            return R_ERR_SYNTAX;
        }
        *out = e;
        return R_OK;
    }
    if (pk_word_is(p, "NOT")) {
        pk_adv(p);
        rx_expr *e = ex_new(p, EX_UN);
        e->op = OP_NOT_LOGICAL;
        if (parse_unary(p, &e->r) != R_OK) {
            rx_expr_free(e);
            return R_ERR_SYNTAX;
        }
        *out = e;
        return R_OK;
    }
    const char *dw = pk_dot_word(p);
    if (dw && rx_strcasecmp(dw, "NOT") == 0) {
        pk_adv(p);
        pk_adv(p);
        pk_adv(p);
        rx_expr *e = ex_new(p, EX_UN);
        e->op = OP_NOT_LOGICAL;
        if (parse_unary(p, &e->r) != R_OK) {
            rx_expr_free(e);
            return R_ERR_SYNTAX;
        }
        *out = e;
        return R_OK;
    }
    return parse_primary(p, out);
}

static int parse_pow(parser *p, rx_expr **out)
{
    int rc = parse_unary(p, out);
    if (rc != R_OK) {
        return rc;
    }
    if (pk_op(p, OP_POW)) {
        pk_adv(p);
        rx_expr *e = ex_new(p, EX_BIN);
        e->op = OP_POW;
        e->l = *out;
        rc = parse_pow(p, &e->r); /* right associative */
        if (rc != R_OK) {
            return rc;
        }
        *out = e;
    }
    return R_OK;
}

static int parse_mul(parser *p, rx_expr **out)
{
    int rc = parse_pow(p, out);
    if (rc != R_OK) {
        return rc;
    }
    while (pk_op(p, OP_MUL) || pk_op(p, OP_DIV) || pk_op(p, OP_MOD)) {
        int op = pk_cur(p)->ival;
        pk_adv(p);
        rx_expr *e = ex_new(p, EX_BIN);
        e->op = op;
        e->l = *out;
        rc = parse_pow(p, &e->r);
        if (rc != R_OK) {
            rx_expr_free(e);
            return rc;
        }
        *out = e;
    }
    return R_OK;
}

static int parse_add(parser *p, rx_expr **out)
{
    int rc = parse_mul(p, out);
    if (rc != R_OK) {
        return rc;
    }
    while (pk_op(p, OP_PLUS) || pk_op(p, OP_MINUS)) {
        int op = pk_cur(p)->ival;
        pk_adv(p);
        rx_expr *e = ex_new(p, EX_BIN);
        e->op = op;
        e->l = *out;
        rc = parse_mul(p, &e->r);
        if (rc != R_OK) {
            rx_expr_free(e);
            return rc;
        }
        *out = e;
    }
    return R_OK;
}

static int parse_cmp(parser *p, rx_expr **out)
{
    int rc = parse_add(p, out);
    if (rc != R_OK) {
        return rc;
    }
    if (pk_cur(p)->kind == TK_OP &&
        (pk_cur(p)->ival == OP_EQ || pk_cur(p)->ival == OP_EEQ || pk_cur(p)->ival == OP_NE ||
         pk_cur(p)->ival == OP_LT || pk_cur(p)->ival == OP_LE || pk_cur(p)->ival == OP_GT ||
         pk_cur(p)->ival == OP_GE || pk_cur(p)->ival == OP_IN)) {
        int op = pk_cur(p)->ival;
        pk_adv(p);
        rx_expr *e = ex_new(p, EX_BIN);
        e->op = op;
        e->l = *out;
        rc = parse_add(p, &e->r);
        if (rc != R_OK) {
            rx_expr_free(e);
            return rc;
        }
        *out = e;
    }
    return R_OK;
}

static int parse_not(parser *p, rx_expr **out)
{
    return parse_cmp(p, out); /* NOT handled in parse_unary */
}

static int parse_and(parser *p, rx_expr **out)
{
    int rc = parse_not(p, out);
    if (rc != R_OK) {
        return rc;
    }
    for (;;) {
        int is_and = pk_word_is(p, "AND");
        const char *dw = pk_dot_word(p);
        if (dw && rx_strcasecmp(dw, "AND") == 0) {
            is_and = 1;
        }
        if (!is_and) {
            break;
        }
        if (pk_cur(p)->kind == TK_WORD) {
            pk_adv(p);
        } else {
            pk_adv(p);
            pk_adv(p);
            pk_adv(p);
        }
        rx_expr *e = ex_new(p, EX_BIN);
        e->op = OP_AND_LOGICAL;
        e->l = *out;
        rc = parse_not(p, &e->r);
        if (rc != R_OK) {
            rx_expr_free(e);
            return rc;
        }
        *out = e;
    }
    return R_OK;
}

static int parse_or(parser *p, rx_expr **out)
{
    int rc = parse_and(p, out);
    if (rc != R_OK) {
        return rc;
    }
    for (;;) {
        int is_or = pk_word_is(p, "OR");
        const char *dw = pk_dot_word(p);
        if (dw && rx_strcasecmp(dw, "OR") == 0) {
            is_or = 1;
        }
        if (!is_or) {
            break;
        }
        if (pk_cur(p)->kind == TK_WORD) {
            pk_adv(p);
        } else {
            pk_adv(p);
            pk_adv(p);
            pk_adv(p);
        }
        rx_expr *e = ex_new(p, EX_BIN);
        e->op = OP_OR_LOGICAL;
        e->l = *out;
        rc = parse_and(p, &e->r);
        if (rc != R_OK) {
            rx_expr_free(e);
            return rc;
        }
        *out = e;
    }
    return R_OK;
}

static int parse_expr(parser *p, rx_expr **out)
{
    return parse_or(p, out);
}

/* ============================ statements ============================ */

static void st_add_expr(rx_stmt *s, rx_expr *e)
{
    if (s->nexpr == s->capexpr) {
        size_t nc = s->capexpr ? s->capexpr * 2 : 8;
        rx_expr **ne = realloc(s->exprs, nc * sizeof(rx_expr *));
        if (!ne) {
            RX_FATAL("out of memory");
        }
        s->exprs = ne;
        s->capexpr = nc;
    }
    s->exprs[s->nexpr++] = e;
}

static void st_add_clause(rx_stmt *s, const char *key, const char *word, rx_expr *e)
{
    rx_clause *nc = realloc(s->clauses, (s->nclause + 1) * sizeof(rx_clause));
    if (!nc) {
        RX_FATAL("out of memory");
    }
    s->clauses = nc;
    rx_clause *c = &s->clauses[s->nclause++];
    memset(c, 0, sizeof(*c));
    snprintf(c->key, sizeof(c->key), "%s", key);
    if (word) {
        snprintf(c->word, sizeof(c->word), "%s", word);
    }
    c->e = e;
}

static void st_add_str(rx_stmt *s, const char *w)
{
    char **ns = realloc(s->strs, (s->nstrs + 1) * sizeof(char *));
    if (!ns) {
        RX_FATAL("out of memory");
    }
    s->strs = ns;
    s->strs[s->nstrs++] = rx_strdup(w);
}

static int stmt_push(parser *p, rx_stmt *s)
{
    if (p->nstmt == p->capstmt) {
        size_t nc = p->capstmt ? p->capstmt * 2 : 16;
        rx_stmt *ns = realloc(p->stmts, nc * sizeof(rx_stmt));
        if (!ns) {
            RX_FATAL("out of memory");
        }
        p->stmts = ns;
        p->capstmt = nc;
    }
    p->stmts[p->nstmt++] = *s;
    return R_OK;
}

static void st_add_bcol(rx_stmt *s, const rx_bcol *col)
{
    if (s->nbcols == s->capbcols) {
        size_t nc = s->capbcols ? s->capbcols * 2 : 8;
        rx_bcol *nb = realloc(s->bcols, nc * sizeof(rx_bcol));
        if (!nb) {
            RX_FATAL("out of memory");
        }
        s->bcols = nb;
        s->capbcols = nc;
    }
    s->bcols[s->nbcols++] = *col;
}

void rx_stmt_steal_bcols(rx_stmt *s, rx_bcol **cols, size_t *ncols)
{
    *cols = s->bcols;
    *ncols = s->nbcols;
    s->bcols = NULL;
    s->nbcols = s->capbcols = 0;
}

void rx_stmt_add_clause(rx_stmt *s, const char *key, const char *word, rx_expr *e)
{
    st_add_clause(s, key, word, e);
}

/* finish a statement: into the pending kid slot (block mode) or the list */
static int stmt_finish(parser *p, rx_stmt *s)
{
    if (p->cur) {
        *p->cur = *s;
        return R_OK;
    }
    return stmt_push(p, s);
}

static void st_add_kid(rx_stmt *s, rx_stmt *kid)
{
    if (s->nkids == s->capkids) {
        size_t nc = s->capkids ? s->capkids * 2 : 8;
        rx_stmt **nk = realloc(s->kids, nc * sizeof(rx_stmt *));
        if (!nk) {
            RX_FATAL("out of memory");
        }
        s->kids = nk;
        s->capkids = nc;
    }
    s->kids[s->nkids++] = kid;
}

/* frees the contents of *s, not the struct itself (stmts are array elements) */
void rx_stmt_free(rx_stmt *s)
{
    for (size_t i = 0; i < s->nexpr; i++) {
        rx_expr_free(s->exprs[i]);
    }
    free(s->exprs);
    s->exprs = NULL;
    s->nexpr = s->capexpr = 0;
    for (size_t i = 0; i < s->nclause; i++) {
        rx_expr_free(s->clauses[i].e);
    }
    free(s->clauses);
    for (size_t i = 0; i < s->nstrs; i++) {
        free(s->strs[i]);
    }
    free(s->strs);
    for (size_t i = 0; i < s->nbcols; i++) {
        rx_expr_free(s->bcols[i].e);
        rx_expr_free(s->bcols[i].valid);
    }
    free(s->bcols);
    for (size_t i = 0; i < s->nkids; i++) {
        rx_stmt_free(s->kids[i]);
        free(s->kids[i]);
    }
    free(s->kids);
    free(s->raw);
    memset(s, 0, sizeof(*s));
}

/* keyword table: >=4-letter prefix match */
typedef struct {
    const char *word;
    st_kind kind;
} kw;

static const kw keywords[] = {
    { "USE", ST_USE },       { "CLOSE", ST_CLOSE },   { "SELECT", ST_SELECT },
    { "LIST", ST_LIST },     { "DISPLAY", ST_DISPLAY }, { "GOTO", ST_GO },
    { "GO", ST_GO },         { "SKIP", ST_SKIP },     { "STORE", ST_STORE },
    { "SET", ST_SET },       { "QUIT", ST_QUIT },     { "CLEAR", ST_CLEAR },
    { "BROWSE", ST_BROWSE }, { "BROW", ST_BROWSE },
    { "APPEND", ST_APPEND }, { "REPLACE", ST_REPLACE }, { "DELETE", ST_DELETE },
    { "RECALL", ST_RECALL }, { "PACK", ST_PACK },     { "ZAP", ST_ZAP },
    { "LOCATE", ST_LOCATE }, { "CONTINUE", ST_CONTINUE }, { "COPY", ST_COPY },
    { "CREATE", ST_CREATE }, { "FLUSH", ST_FLUSH },   { "TYPE", ST_TYPE },
    { "UNLOCK", ST_UNLOCK },
    /* M5 §7.1 program flow / I/O / macros */
    { "IF", ST_IF },       { "DO", ST_DO },         { "FOR", ST_FOR },
    { "SCAN", ST_SCAN },   { "PROCEDURE", ST_PROC }, { "FUNCTION", ST_PROC },
    { "TEXT", ST_TEXT },   { "RETURN", ST_RETURN }, { "EXIT", ST_EXIT },
    { "LOOP", ST_LOOP },   { "RETRY", ST_RETRY },   { "PARAMETERS", ST_PARAMETERS },
    { "LPARAMETERS", ST_PARAMETERS }, { "PRIVATE", ST_PRIVATE }, { "PUBLIC", ST_PUBLIC },
    { "RELEASE", ST_RELEASE }, { "ON", ST_ON },     { "WAIT", ST_WAIT },
    { "INPUT", ST_INPUT }, { "ACCEPT", ST_ACCEPT }, { "CANCEL", ST_CANCEL },
    { "SUSPEND", ST_SUSPEND }, { "RESUME", ST_RESUME }, { "DIMENSION", ST_DIMENSION },
    { "DECLARE", ST_DIMENSION },
    { "ENDIF", ST_ENDMARK }, { "ENDDO", ST_ENDMARK }, { "ENDFOR", ST_ENDMARK },
    { "ENDCASE", ST_ENDMARK }, { "ENDSCAN", ST_ENDMARK }, { "ENDPROC", ST_ENDMARK },
    { "ENDFUNC", ST_ENDMARK }, { "ENDTEXT", ST_ENDMARK },
};

const char *const *rx_keyword_list(size_t *n)
{
    static const char *words[sizeof(keywords) / sizeof(keywords[0])];
    static int ready = 0;
    if (!ready) {
        for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
            words[i] = keywords[i].word;
        }
        ready = 1;
    }
    *n = sizeof(keywords) / sizeof(keywords[0]);
    return words;
}

static int kw_match(const char *w, st_kind *kind)
{
    size_t wl = strlen(w);
    int found = 0;
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        int m = (wl >= 4 && strncasecmp(w, keywords[i].word, wl) == 0 &&
                 strlen(keywords[i].word) >= wl) ||
                rx_strcasecmp(w, keywords[i].word) == 0;
        if (m) {
            if (found && *kind != keywords[i].kind) {
                return -1; /* ambiguous */
            }
            *kind = keywords[i].kind;
            found = 1;
        }
    }
    return found;
}

/* parse scope clause: ALL | REST | NEXT expr | RECORD expr */
static int parse_scope(parser *p, rx_stmt *s)
{
    if (pk_word_is(p, "ALL")) {
        st_add_clause(s, "SCOPE", "ALL", NULL);
        pk_adv(p);
        return R_OK;
    }
    if (pk_word_is(p, "REST")) {
        st_add_clause(s, "SCOPE", "REST", NULL);
        pk_adv(p);
        return R_OK;
    }
    if (pk_word_is(p, "NEXT")) {
        pk_adv(p);
        rx_expr *e = NULL;
        int rc = parse_expr(p, &e);
        if (rc != R_OK) {
            return rc;
        }
        st_add_clause(s, "SCOPE", "NEXT", e);
        return R_OK;
    }
    if (pk_word_is(p, "RECORD")) {
        pk_adv(p);
        rx_expr *e = NULL;
        int rc = parse_expr(p, &e);
        if (rc != R_OK) {
            return rc;
        }
        st_add_clause(s, "SCOPE", "RECORD", e);
        return R_OK;
    }
    return 0; /* no scope */
}

static int end_of_line(parser *p)
{
    return pk_is(p, TK_NEWLINE) || pk_is(p, TK_EOF) || pk_op(p, OP_SEMI);
}

/* skip the remainder of a logical line */
static void skip_to_eol(parser *p)
{
    while (!pk_is(p, TK_NEWLINE) && !pk_is(p, TK_EOF)) {
        pk_adv(p);
    }
}

/* gather a path-like token sequence (WORD . WORD / WORD NUM...) into an
   owned string, stopping at any stop keyword or end of line */
static char *parse_path_until(parser *p, const char **stopkw)
{
    strbuf path;
    sb_init(&path);
    for (;;) {
        rx_tok *t = pk_cur(p);
        if (t->kind == TK_WORD) {
            int is_kw = 0;
            for (int k = 0; stopkw && stopkw[k]; k++) {
                if (rx_strcasecmp(t->text, stopkw[k]) == 0) {
                    is_kw = 1;
                    break;
                }
            }
            if (is_kw && path.len > 0) {
                break;
            }
            if (is_kw && path.len == 0) {
                break; /* clause keyword where a path was expected */
            }
            sb_puts(&path, t->text);
            pk_adv(p);
        } else if (pk_op(p, OP_DIV) || pk_op(p, OP_DOT)) {
            sb_putc(&path, pk_cur(p)->ival == OP_DIV ? '/' : '.');
            pk_adv(p);
        } else if (t->kind == TK_NUM) {
            sb_puts(&path, t->text);
            pk_adv(p);
        } else {
            break;
        }
    }
    if (!path.buf) {
        sb_putc(&path, '\0');
        path.len = 0;
    }
    return path.buf; /* owned */
}

/* scope + FOR/WHILE clauses shared by LIST/DISPLAY/DELETE/RECALL/REPLACE/
   LOCATE/COPY */
static int parse_scope_fw(parser *p, rx_stmt *s)
{
    parse_scope(p, s);
    while (!end_of_line(p)) {
        if (pk_word_is(p, "FOR") || pk_word_is(p, "WHILE")) {
            const char *key = pk_cur(p)->text;
            pk_adv(p);
            rx_expr *e = NULL;
            int rc = parse_expr(p, &e);
            if (rc != R_OK) {
                return rc;
            }
            st_add_clause(s, key, "", e);
        } else {
            break;
        }
    }
    return R_OK;
}

/* FIELDS f1, f2, ... -> one "FIELD" clause per name */
static int parse_fields(parser *p, rx_stmt *s)
{
    if (!pk_word_is(p, "FIELDS")) {
        return 0;
    }
    pk_adv(p);
    for (;;) {
        if (pk_cur(p)->kind != TK_WORD) {
            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected field name");
        }
        st_add_clause(s, "FIELD", pk_cur(p)->text, NULL);
        pk_adv(p);
        if (pk_op(p, OP_COMMA)) {
            pk_adv(p);
            continue;
        }
        break;
    }
    return 1;
}

/* ============================ M5 block statements (§7.1) ============================ */

static int parse_stmt(parser *p);

/* parse statements into block->kids until one of the end words (or EOF when
   eof_ok, or PROCEDURE/FUNCTION when stop_at_proc); end words not consumed */
/* capture the raw source span of one statement (M5 runtime macro expansion) */
static void set_raw_span(parser *p, rx_stmt *s, size_t pos0)
{
    if (!p->src || s->kind == ST_TEXT || s->raw) {
        return;
    }
    size_t srclen = strlen(p->src);
    size_t end = p->pos < p->n ? p->toks[p->pos].off : srclen;
    size_t start = p->toks[pos0].off;
    if (end > start && end <= srclen) {
        s->raw = malloc(end - start + 1);
        if (!s->raw) {
            RX_FATAL("out of memory");
        }
        memcpy(s->raw, p->src + start, end - start);
        s->raw[end - start] = '\0';
    }
}

static int parse_body(parser *p, rx_stmt *block, const char *const *ends, int stop_at_proc,
                      int eof_ok)
{
    for (;;) {
        while (pk_is(p, TK_NEWLINE)) {
            pk_adv(p);
        }
        if (pk_is(p, TK_EOF)) {
            if (eof_ok) {
                return R_OK;
            }
            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                           "Unexpected end of input (missing END)");
        }
        rx_tok *t = pk_cur(p);
        if (t->kind == TK_WORD) {
            int is_end = 0;
            for (int k = 0; ends[k]; k++) {
                if (pk_word_is(p, ends[k])) {
                    is_end = 1;
                    break;
                }
            }
            if (is_end) {
                return R_OK;
            }
            if (stop_at_proc && (pk_word_is(p, "PROCEDURE") || pk_word_is(p, "FUNCTION"))) {
                return R_OK;
            }
        }
        rx_stmt *kid = calloc(1, sizeof(rx_stmt));
        if (!kid) {
            RX_FATAL("out of memory");
        }
        size_t pos0 = p->pos;
        rx_stmt *saved_cur = p->cur;
        p->cur = kid;
        int rc = parse_stmt(p);
        p->cur = saved_cur;
        if (rc == R_OK) {
            set_raw_span(p, kid, pos0);
        }
        if (rc != R_OK) {
            rx_stmt_free(kid);
            free(kid);
            return rc;
        }
        st_add_kid(block, kid);
        while (pk_op(p, OP_SEMI)) {
            pk_adv(p);
            while (pk_is(p, TK_NEWLINE)) {
                pk_adv(p);
            }
        }
        while (pk_is(p, TK_NEWLINE)) {
            pk_adv(p);
        }
    }
}

static int parse_if(parser *p, rx_stmt *s)
{
    s->kind = ST_IF;
    pk_adv(p); /* IF */
    for (;;) {
        rx_expr *e = NULL;
        int rc = parse_expr(p, &e);
        if (rc != R_OK) {
            return rc;
        }
        st_add_expr(s, e);
        s->ncond++;
        rx_stmt *body = calloc(1, sizeof(rx_stmt));
        if (!body) {
            RX_FATAL("out of memory");
        }
        static const char *const ends[] = { "ELSEIF", "ELSE", "ENDIF", NULL };
        rc = parse_body(p, body, ends, 0, 0);
        if (rc != R_OK) {
            rx_stmt_free(body);
            free(body);
            return rc;
        }
        st_add_kid(s, body);
        if (pk_word_is(p, "ELSEIF")) {
            pk_adv(p);
            continue;
        }
        if (pk_word_is(p, "ELSE")) {
            pk_adv(p);
            rx_stmt *els = calloc(1, sizeof(rx_stmt));
            if (!els) {
                RX_FATAL("out of memory");
            }
            static const char *const e2[] = { "ENDIF", NULL };
            rc = parse_body(p, els, e2, 0, 0);
            if (rc != R_OK) {
                rx_stmt_free(els);
                free(els);
                return rc;
            }
            st_add_kid(s, els);
        }
        if (pk_word_is(p, "ENDIF")) {
            pk_adv(p);
            break;
        }
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected ELSEIF, ELSE or ENDIF");
    }
    return stmt_finish(p, s);
}

static int parse_while(parser *p, rx_stmt *s)
{
    s->kind = ST_WHILE;
    pk_adv(p); /* DO */
    pk_adv(p); /* WHILE */
    rx_expr *e = NULL;
    int rc = parse_expr(p, &e);
    if (rc != R_OK) {
        return rc;
    }
    st_add_expr(s, e);
    rx_stmt *body = calloc(1, sizeof(rx_stmt));
    if (!body) {
        RX_FATAL("out of memory");
    }
    static const char *const ends[] = { "ENDDO", NULL };
    rc = parse_body(p, body, ends, 0, 0);
    if (rc != R_OK) {
        rx_stmt_free(body);
        free(body);
        return rc;
    }
    st_add_kid(s, body);
    if (!pk_word_is(p, "ENDDO")) {
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected ENDDO");
    }
    pk_adv(p);
    return stmt_finish(p, s);
}

static int parse_for(parser *p, rx_stmt *s)
{
    s->kind = ST_FOR;
    pk_adv(p); /* FOR */
    if (pk_cur(p)->kind != TK_WORD) {
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected loop variable after FOR");
    }
    snprintf(s->target, sizeof(s->target), "%s", pk_cur(p)->text);
    for (char *c = s->target; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    pk_adv(p);
    if (!pk_op(p, OP_EQ)) {
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected = in FOR");
    }
    pk_adv(p);
    rx_expr *e = NULL;
    int rc = parse_expr(p, &e); /* start */
    if (rc != R_OK) {
        return rc;
    }
    st_add_expr(s, e);
    if (!pk_word_is(p, "TO")) {
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected TO in FOR");
    }
    pk_adv(p);
    rc = parse_expr(p, &e); /* end */
    if (rc != R_OK) {
        return rc;
    }
    st_add_expr(s, e);
    if (pk_word_is(p, "STEP")) {
        pk_adv(p);
        rc = parse_expr(p, &e);
        if (rc != R_OK) {
            return rc;
        }
        st_add_expr(s, e);
    }
    s->ncond = s->nexpr; /* 2 or 3 header exprs */
    rx_stmt *body = calloc(1, sizeof(rx_stmt));
    if (!body) {
        RX_FATAL("out of memory");
    }
    static const char *const ends[] = { "ENDFOR", "NEXT", NULL };
    rc = parse_body(p, body, ends, 0, 0);
    if (rc != R_OK) {
        rx_stmt_free(body);
        free(body);
        return rc;
    }
    st_add_kid(s, body);
    if (!pk_word_is(p, "ENDFOR") && !pk_word_is(p, "NEXT")) {
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected ENDFOR or NEXT");
    }
    pk_adv(p);
    return stmt_finish(p, s);
}

static int parse_case(parser *p, rx_stmt *s)
{
    s->kind = ST_CASE;
    pk_adv(p); /* DO */
    pk_adv(p); /* CASE */
    for (;;) {
        while (pk_is(p, TK_NEWLINE)) {
            pk_adv(p);
        }
        if (pk_word_is(p, "ENDCASE")) {
            pk_adv(p);
            break;
        }
        if (pk_word_is(p, "OTHERWISE")) {
            pk_adv(p);
            rx_stmt *els = calloc(1, sizeof(rx_stmt));
            if (!els) {
                RX_FATAL("out of memory");
            }
            static const char *const e2[] = { "ENDCASE", NULL };
            int rc = parse_body(p, els, e2, 0, 0);
            if (rc != R_OK) {
                rx_stmt_free(els);
                free(els);
                return rc;
            }
            st_add_kid(s, els);
            if (!pk_word_is(p, "ENDCASE")) {
                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected ENDCASE");
            }
            pk_adv(p);
            break;
        }
        if (!pk_word_is(p, "CASE")) {
            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected CASE or ENDCASE");
        }
        pk_adv(p);
        rx_expr *e = NULL;
        int rc = parse_expr(p, &e);
        if (rc != R_OK) {
            return rc;
        }
        st_add_expr(s, e);
        s->ncond++;
        rx_stmt *body = calloc(1, sizeof(rx_stmt));
        if (!body) {
            RX_FATAL("out of memory");
        }
        static const char *const ends[] = { "CASE", "OTHERWISE", "ENDCASE", NULL };
        rc = parse_body(p, body, ends, 0, 0);
        if (rc != R_OK) {
            rx_stmt_free(body);
            free(body);
            return rc;
        }
        st_add_kid(s, body);
    }
    return stmt_finish(p, s);
}

static int parse_scan(parser *p, rx_stmt *s)
{
    s->kind = ST_SCAN;
    pk_adv(p); /* SCAN */
    int rc = parse_scope_fw(p, s);
    if (rc != R_OK) {
        return rc;
    }
    if (!end_of_line(p)) {
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in SCAN");
    }
    rx_stmt *body = calloc(1, sizeof(rx_stmt));
    if (!body) {
        RX_FATAL("out of memory");
    }
    static const char *const ends[] = { "ENDSCAN", NULL };
    rc = parse_body(p, body, ends, 0, 0);
    if (rc != R_OK) {
        rx_stmt_free(body);
        free(body);
        return rc;
    }
    st_add_kid(s, body);
    if (!pk_word_is(p, "ENDSCAN")) {
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected ENDSCAN");
    }
    pk_adv(p);
    return stmt_finish(p, s);
}

static int parse_proc(parser *p, rx_stmt *s)
{
    int is_func = pk_word_is(p, "FUNCTION");
    s->kind = ST_PROC;
    pk_adv(p);
    if (pk_cur(p)->kind != TK_WORD) {
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected procedure name");
    }
    snprintf(s->target, sizeof(s->target), "%s", pk_cur(p)->text);
    for (char *c = s->target; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    pk_adv(p);
    if (is_func) {
        st_add_clause(s, "FUNC", "1", NULL);
    }
    if (pk_op(p, OP_LP)) { /* optional parameter list */
        pk_adv(p);
        while (!pk_op(p, OP_RP)) {
            if (pk_cur(p)->kind == TK_WORD) {
                st_add_str(s, pk_cur(p)->text);
                pk_adv(p);
            } else {
                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected parameter name");
            }
            if (pk_op(p, OP_COMMA)) {
                pk_adv(p);
            }
        }
        pk_adv(p);
    }
    rx_stmt *body = calloc(1, sizeof(rx_stmt));
    if (!body) {
        RX_FATAL("out of memory");
    }
    static const char *const ends[] = { "ENDPROC", "ENDFUNC", NULL };
    int rc = parse_body(p, body, ends, 1, 1);
    if (rc != R_OK) {
        rx_stmt_free(body);
        free(body);
        return rc;
    }
    st_add_kid(s, body);
    if (pk_word_is(p, "ENDPROC") || pk_word_is(p, "ENDFUNC")) {
        pk_adv(p);
    }
    return stmt_finish(p, s);
}

static int parse_text(parser *p, rx_stmt *s)
{
    s->kind = ST_TEXT;
    size_t text_off = pk_cur(p)->off;
    pk_adv(p); /* TEXT */
    if (p->src) {
        /* verbatim span: end of the TEXT line .. start of the ENDTEXT line */
        size_t i = text_off;
        while (p->src[i] && p->src[i] != '\n') {
            i++;
        }
        size_t start = p->src[i] == '\n' ? i + 1 : i;
        size_t end = strlen(p->src);
        for (size_t k = p->pos; k < p->n; k++) {
            rx_tok *t = &p->toks[k];
            if (t->kind == TK_WORD && rx_strcasecmp(t->text, "ENDTEXT") == 0) {
                size_t e = t->off;
                while (e > 0 && p->src[e - 1] != '\n') {
                    e--;
                }
                end = e;
                break;
            }
        }
        size_t n = end > start ? end - start : 0;
        s->raw = malloc(n + 1);
        if (!s->raw) {
            RX_FATAL("out of memory");
        }
        memcpy(s->raw, p->src + start, n);
        s->raw[n] = '\0';
        while (!pk_is(p, TK_EOF) && !pk_word_is(p, "ENDTEXT")) {
            pk_adv(p);
        }
        if (pk_word_is(p, "ENDTEXT")) {
            pk_adv(p);
        }
        return stmt_finish(p, s);
    }
    /* no source text available: join token texts (approximate) */
    strbuf sb;
    sb_init(&sb);
    while (!pk_is(p, TK_EOF) && !pk_word_is(p, "ENDTEXT")) {
        rx_tok *t = pk_cur(p);
        if (t->kind == TK_NEWLINE) {
            sb_putc(&sb, '\n');
        } else if (t->text) {
            if (sb.len && sb.buf[sb.len - 1] != '\n') {
                sb_putc(&sb, ' ');
            }
            sb_puts(&sb, t->text);
        }
        pk_adv(p);
    }
    if (pk_word_is(p, "ENDTEXT")) {
        pk_adv(p);
    }
    if (!sb.buf) {
        sb_putc(&sb, '\0');
        sb.len = 0;
    }
    s->raw = sb.buf;
    return stmt_finish(p, s);
}

static int parse_stmt(parser *p)
{
    rx_stmt s;
    memset(&s, 0, sizeof(s));
    s.line = pk_cur(p)->line;
    s.col = pk_cur(p)->col;

    /* M5: a line starting with & is a macro line; keep it raw, it is
       expanded and re-parsed at execution time */
    if (pk_op(p, OP_AMP)) {
        s.kind = ST_EMPTY;
        while (!end_of_line(p)) {
            pk_adv(p);
        }
        return stmt_finish(p, &s);
    }

    /* M5 block openers (recursive) */
    if (pk_word_is(p, "IF")) {
        return parse_if(p, &s);
    }
    if (pk_word_is(p, "DO")) {
        rx_tok *a = pk_ahead(p, 1);
        if (a->kind == TK_WORD && rx_strcasecmp(a->text, "WHILE") == 0) {
            return parse_while(p, &s);
        }
        if (a->kind == TK_WORD && rx_strcasecmp(a->text, "CASE") == 0) {
            return parse_case(p, &s);
        }
    }
    if (pk_word_is(p, "FOR")) {
        return parse_for(p, &s);
    }
    if (pk_word_is(p, "SCAN")) {
        return parse_scan(p, &s);
    }
    if (pk_word_is(p, "PROCEDURE") || pk_word_is(p, "FUNCTION")) {
        return parse_proc(p, &s);
    }
    if (pk_word_is(p, "TEXT")) {
        return parse_text(p, &s);
    }

    if (pk_op(p, OP_Q)) {
        int qq = 0;
        while (pk_op(p, OP_Q)) {
            qq++;
            pk_adv(p);
        }
        s.kind = qq >= 2 ? ST_PRINTN : ST_PRINT;
        if (!end_of_line(p)) {
            for (;;) {
                rx_expr *e = NULL;
                int rc = parse_expr(p, &e);
                if (rc != R_OK) {
                    return rc;
                }
                st_add_expr(&s, e);
                if (pk_op(p, OP_COMMA)) {
                    pk_adv(p);
                    continue;
                }
                break;
            }
        }
        if (!end_of_line(p)) {
            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error");
        }
        return stmt_finish(p, &s);
    }

    if (pk_cur(p)->kind == TK_WORD) {
        st_kind kind = ST_EMPTY;
        int km = kw_match(pk_cur(p)->text, &kind);
        if (km < 0) {
            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Ambiguous command abbreviation");
        }
        if (km > 0) {
            s.kind = kind;
            pk_adv(p);
            int rc = R_OK;
            switch (kind) {
            case ST_USE:
                if (!end_of_line(p)) {
                    rx_expr *e = NULL;
                    if ((pk_cur(p)->kind == TK_WORD || pk_op(p, OP_DOT) || pk_op(p, OP_DIV)) &&
                        !pk_op_ahead_is(p, 1, OP_LP)) {
                        /* bare table name or path: USE people, USE people.dbf,
                           USE sub/dir/people — gather WORD/DOT/DIV tokens until
                           a clause keyword or EOL */
                        static const char *clause_kw[] = { "IN", "ALIAS", "AGAIN", "EXCLUSIVE",
                                                           "SHARED", "NOUPDATE", "INDEX", "ORDER",
                                                           "ASCENDING", "DESCENDING", NULL };
                        strbuf path;
                        sb_init(&path);
                        for (;;) {
                            rx_tok *t = pk_cur(p);
                            if (t->kind == TK_WORD) {
                                int is_kw = 0;
                                for (int k = 0; clause_kw[k]; k++) {
                                    if (rx_strcasecmp(t->text, clause_kw[k]) == 0) {
                                        is_kw = 1;
                                        break;
                                    }
                                }
                                if (is_kw && path.len > 0) {
                                    break;
                                }
                                sb_puts(&path, t->text);
                                pk_adv(p);
                            } else if (pk_op(p, OP_DIV) || pk_op(p, OP_DOT)) {
                                sb_putc(&path, pk_cur(p)->ival == OP_DIV ? '/' : '.');
                                pk_adv(p);
                            } else if (t->kind == TK_NUM) {
                                sb_puts(&path, t->text);
                                pk_adv(p);
                            } else {
                                break;
                            }
                        }
                        if (!path.buf) {
                            sb_putc(&path, '\0');
                            path.len = 0;
                        }
                        e = ex_new(p, EX_STR);
                        rval_set_str(&e->lit, path.buf, path.len);
                    } else {
                        rc = parse_expr(p, &e);
                        if (rc != R_OK) {
                            return rc;
                        }
                    }
                    st_add_expr(&s, e);
                }
                while (!end_of_line(p)) {
                    if (pk_word_is(p, "IN")) {
                        pk_adv(p);
                        if (pk_cur(p)->kind == TK_WORD || pk_cur(p)->kind == TK_NUM) {
                            st_add_clause(&s, "IN", pk_cur(p)->text, NULL);
                            pk_adv(p);
                        } else {
                            rx_expr *e = NULL;
                            rc = parse_expr(p, &e);
                            if (rc != R_OK) {
                                return rc;
                            }
                            st_add_clause(&s, "IN", "", e);
                        }
                    } else if (pk_word_is(p, "ALIAS")) {
                        pk_adv(p);
                        if (pk_cur(p)->kind != TK_WORD) {
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected alias");
                        }
                        st_add_clause(&s, "ALIAS", pk_cur(p)->text, NULL);
                        pk_adv(p);
                    } else if (pk_word_is(p, "AGAIN") || pk_word_is(p, "EXCLUSIVE") ||
                               pk_word_is(p, "SHARED") || pk_word_is(p, "NOUPDATE")) {
                        st_add_str(&s, pk_cur(p)->text);
                        pk_adv(p);
                    } else if (pk_word_is(p, "INDEX") || pk_word_is(p, "ORDER")) {
                        skip_to_eol(p); /* index clauses land at M6 */
                    } else {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in USE");
                    }
                }
                break;
            case ST_CLOSE:
                while (!end_of_line(p)) {
                    st_add_str(&s, pk_cur(p)->text);
                    pk_adv(p);
                }
                break;
            case ST_SELECT:
                if (end_of_line(p)) {
                    return pk_fail(p, s.line, s.col, "SELECT requires an argument");
                }
                if (pk_cur(p)->kind == TK_WORD && !pk_op_ahead_is(p, 1, OP_LP) &&
                    !pk_op_ahead_is(p, 1, OP_EQ)) {
                    /* bare word: alias name or area number as text */
                    st_add_str(&s, pk_cur(p)->text);
                    pk_adv(p);
                } else {
                    rx_expr *e = NULL;
                    rc = parse_expr(p, &e);
                    if (rc == R_OK) {
                        st_add_expr(&s, e);
                    }
                    if (rc != R_OK) {
                        return rc;
                    }
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in SELECT");
                }
                break;
            case ST_LIST:
            case ST_DISPLAY: {
                rc = parse_scope_fw(p, &s);
                if (rc != R_OK) {
                    return rc;
                }
                while (!end_of_line(p)) {
                    if (pk_word_is(p, "OFF")) {
                        st_add_str(&s, "OFF");
                        pk_adv(p);
                    } else if (parse_fields(p, &s)) {
                        /* FIELDS clause consumed */
                    } else {
                        skip_to_eol(p); /* unknown clauses tolerated for now */
                        break;
                    }
                }
                break;
            }
            case ST_GO:
                if (pk_word_is(p, "TOP")) {
                    st_add_clause(&s, "TARGET", "TOP", NULL);
                    pk_adv(p);
                } else if (pk_word_is(p, "BOTTOM")) {
                    st_add_clause(&s, "TARGET", "BOTTOM", NULL);
                    pk_adv(p);
                } else if (pk_word_is(p, "RECORD")) {
                    pk_adv(p);
                    rx_expr *e = NULL;
                    rc = parse_expr(p, &e);
                    if (rc != R_OK) {
                        return rc;
                    }
                    st_add_clause(&s, "TARGET", "RECORD", e);
                } else {
                    rx_expr *e = NULL;
                    rc = parse_expr(p, &e);
                    if (rc != R_OK) {
                        return rc;
                    }
                    st_add_clause(&s, "TARGET", "EXPR", e);
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in GO");
                }
                break;
            case ST_SKIP:
                if (!end_of_line(p)) {
                    rx_expr *e = NULL;
                    rc = parse_expr(p, &e);
                    if (rc != R_OK) {
                        return rc;
                    }
                    st_add_expr(&s, e);
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in SKIP");
                }
                break;
            case ST_STORE: {
                rx_expr *e = NULL;
                rc = parse_expr(p, &e);
                if (rc != R_OK) {
                    return rc;
                }
                st_add_expr(&s, e);
                if (!pk_word_is(p, "TO")) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected TO");
                }
                pk_adv(p);
                for (;;) {
                    if (pk_cur(p)->kind != TK_WORD) {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected variable");
                    }
                    st_add_str(&s, pk_cur(p)->text);
                    pk_adv(p);
                    if (pk_op(p, OP_COMMA)) {
                        pk_adv(p);
                        continue;
                    }
                    break;
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in STORE");
                }
                break;
            }
            case ST_SET:
                if (pk_word_is(p, "RELATION")) {
                    /* SET RELATION [TO [e INTO alias [, ...]] [ADDITIVE]] |
                       SET RELATION OFF INTO alias  (M4) */
                    pk_adv(p);
                    if (pk_word_is(p, "OFF")) {
                        pk_adv(p);
                        if (!pk_word_is(p, "INTO")) {
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                           "Expected INTO after SET RELATION OFF");
                        }
                        pk_adv(p);
                        if (pk_cur(p)->kind != TK_WORD) {
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                           "Expected an alias after INTO");
                        }
                        st_add_clause(&s, "REL_OFF", pk_cur(p)->text, NULL);
                        pk_adv(p);
                        if (!end_of_line(p)) {
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                           "Syntax error in SET RELATION");
                        }
                        break;
                    }
                    if (pk_word_is(p, "TO")) {
                        pk_adv(p);
                    }
                    if (end_of_line(p)) {
                        st_add_clause(&s, "REL_CLEAR", "", NULL);
                        break;
                    }
                    for (;;) {
                        rx_expr *e = NULL;
                        rc = parse_expr(p, &e);
                        if (rc != R_OK) {
                            return rc;
                        }
                        if (!pk_word_is(p, "INTO")) {
                            rx_expr_free(e);
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                           "Expected INTO in SET RELATION");
                        }
                        pk_adv(p);
                        if (pk_cur(p)->kind != TK_WORD) {
                            rx_expr_free(e);
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                           "Expected an alias after INTO");
                        }
                        st_add_clause(&s, "REL", pk_cur(p)->text, e);
                        pk_adv(p);
                        if (pk_op(p, OP_COMMA)) {
                            pk_adv(p);
                            continue;
                        }
                        break;
                    }
                    if (pk_word_is(p, "ADDITIVE")) {
                        st_add_clause(&s, "REL_ADD", "1", NULL);
                        pk_adv(p);
                    }
                    if (!end_of_line(p)) {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                       "Syntax error in SET RELATION");
                    }
                    break;
                }
                if (pk_word_is(p, "SKIP")) {
                    /* SET SKIP TO [alias [, ...]]  (M4) */
                    pk_adv(p);
                    if (!pk_word_is(p, "TO")) {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected TO after SET SKIP");
                    }
                    pk_adv(p);
                    if (end_of_line(p)) {
                        st_add_clause(&s, "SKIP_CLEAR", "", NULL);
                        break;
                    }
                    for (;;) {
                        if (pk_cur(p)->kind != TK_WORD) {
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                           "Expected an alias in SET SKIP TO");
                        }
                        st_add_clause(&s, "SKIP", pk_cur(p)->text, NULL);
                        pk_adv(p);
                        if (pk_op(p, OP_COMMA)) {
                            pk_adv(p);
                            continue;
                        }
                        break;
                    }
                    if (!end_of_line(p)) {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in SET SKIP");
                    }
                    break;
                }
                if (pk_word_is(p, "FILTER")) {
                    /* SET FILTER TO [expr] — the expr is compiled at runtime */
                    pk_adv(p);
                    if (!pk_word_is(p, "TO")) {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected TO after SET FILTER");
                    }
                    pk_adv(p);
                    if (end_of_line(p)) {
                        st_add_clause(&s, "FILTER", "", NULL); /* clear the filter */
                    } else {
                        rx_expr *e = NULL;
                        rc = parse_expr(p, &e);
                        if (rc != R_OK) {
                            return rc;
                        }
                        st_add_clause(&s, "FILTER", "", e);
                    }
                    if (!end_of_line(p)) {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                       "Syntax error in SET FILTER");
                    }
                    break;
                }
                while (!end_of_line(p)) {
                    rx_tok *t = pk_cur(p);
                    if (t->kind == TK_STR) {
                        char buf[1100];
                        snprintf(buf, sizeof(buf), "\"%s\"", t->text);
                        st_add_str(&s, buf);
                    } else {
                        st_add_str(&s, t->text);
                    }
                    pk_adv(p);
                }
                break;
            case ST_BROWSE: {
                int brc = parse_browse(p, &s);
                if (brc != R_OK) {
                    return brc;
                }
                break;
            }
            case ST_QUIT:
            case ST_CLEAR:
            case ST_FLUSH:
            case ST_PACK:
            case ST_ZAP:
            case ST_UNLOCK:
                while (!end_of_line(p)) {
                    st_add_str(&s, pk_cur(p)->text);
                    pk_adv(p);
                }
                break;
            case ST_APPEND: {
                if (pk_word_is(p, "BLANK")) {
                    st_add_str(&s, "BLANK");
                    pk_adv(p);
                } else if (pk_word_is(p, "FROM")) {
                    pk_adv(p);
                    static const char *stop[] = { "TYPE", "SDF", "DELIMITED", "WITH", "FIELDS",
                                                  NULL };
                    char *path = parse_path_until(p, stop);
                    st_add_str(&s, path);
                    free(path);
                    while (!end_of_line(p)) {
                        if (pk_word_is(p, "TYPE")) {
                            pk_adv(p);
                            if (pk_cur(p)->kind != TK_WORD) {
                                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                               "Expected SDF or DELIMITED");
                            }
                            st_add_clause(&s, "TYPE", pk_cur(p)->text, NULL);
                            pk_adv(p);
                        } else if (pk_word_is(p, "SDF") || pk_word_is(p, "DELIMITED")) {
                            st_add_clause(&s, "TYPE", pk_cur(p)->text, NULL);
                            pk_adv(p);
                        } else if (pk_word_is(p, "WITH")) {
                            pk_adv(p);
                            if (pk_cur(p)->kind != TK_STR && pk_cur(p)->kind != TK_WORD) {
                                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                               "Expected delimiter in WITH");
                            }
                            st_add_clause(&s, "WITH", pk_cur(p)->text, NULL);
                            pk_adv(p);
                        } else if (parse_fields(p, &s)) {
                            /* FIELDS clause consumed */
                        } else {
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                           "Syntax error in APPEND FROM");
                        }
                    }
                } else {
                    st_add_str(&s, "APPEND"); /* bare APPEND: interactive at M3+ */
                    skip_to_eol(p);
                }
                break;
            }
            case ST_REPLACE: {
                rc = parse_scope_fw(p, &s);
                if (rc != R_OK) {
                    return rc;
                }
                int npairs = 0;
                while (!end_of_line(p)) {
                    if (pk_cur(p)->kind != TK_WORD) {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                       "Expected field name in REPLACE");
                    }
                    char fname[140];
                    snprintf(fname, sizeof(fname), "%s", pk_cur(p)->text);
                    pk_adv(p);
                    if (pk_op(p, OP_ARROW) && pk_ahead(p, 1)->kind == TK_WORD) {
                        size_t fl = strlen(fname);
                        snprintf(fname + fl, sizeof(fname) - fl, "->%s", pk_ahead(p, 1)->text);
                        pk_adv(p);
                        pk_adv(p);
                    }
                    for (char *c = fname; *c; c++) {
                        *c = (char)toupper((unsigned char)*c);
                    }
                    if (!pk_word_is(p, "WITH")) {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                       "Expected WITH in REPLACE");
                    }
                    pk_adv(p);
                    rx_expr *e = NULL;
                    rc = parse_expr(p, &e);
                    if (rc != R_OK) {
                        return rc;
                    }
                    st_add_clause(&s, "REPL", fname, e);
                    npairs++;
                    if (pk_op(p, OP_COMMA)) {
                        pk_adv(p);
                        continue;
                    }
                    break;
                }
                if (!npairs) {
                    return pk_fail(p, s.line, s.col, "REPLACE requires field WITH expression");
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in REPLACE");
                }
                break;
            }
            case ST_DELETE:
            case ST_RECALL:
                rc = parse_scope_fw(p, &s);
                if (rc != R_OK) {
                    return rc;
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error");
                }
                break;
            case ST_LOCATE:
                rc = parse_scope_fw(p, &s);
                if (rc != R_OK) {
                    return rc;
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in LOCATE");
                }
                break;
            case ST_CONTINUE:
                skip_to_eol(p);
                break;
            case ST_COPY: {
                if (pk_word_is(p, "STRUCTURE")) {
                    st_add_str(&s, "STRUCTURE");
                    pk_adv(p);
                }
                if (!pk_word_is(p, "TO")) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "COPY expects TO");
                }
                pk_adv(p);
                static const char *stop[] = { "FIELDS", "ALL",  "REST",   "NEXT", "RECORD",
                                              "FOR",    "WHILE", "TYPE",   "SDF",  "DELIMITED",
                                              "WITH",   "EXTENDED", NULL };
                char *path = parse_path_until(p, stop);
                st_add_str(&s, path);
                free(path);
                while (!end_of_line(p)) {
                    if (parse_fields(p, &s)) {
                        continue;
                    }
                    if (pk_word_is(p, "EXTENDED")) {
                        st_add_str(&s, "EXTENDED");
                        pk_adv(p);
                        continue;
                    }
                    if (pk_word_is(p, "TYPE")) {
                        pk_adv(p);
                        if (pk_cur(p)->kind != TK_WORD) {
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                           "Expected SDF or DELIMITED");
                        }
                        st_add_clause(&s, "TYPE", pk_cur(p)->text, NULL);
                        pk_adv(p);
                        continue;
                    }
                    if (pk_word_is(p, "SDF") || pk_word_is(p, "DELIMITED")) {
                        st_add_clause(&s, "TYPE", pk_cur(p)->text, NULL);
                        pk_adv(p);
                        continue;
                    }
                    if (pk_word_is(p, "WITH")) {
                        pk_adv(p);
                        if (pk_cur(p)->kind != TK_STR && pk_cur(p)->kind != TK_WORD) {
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                           "Expected delimiter in WITH");
                        }
                        st_add_clause(&s, "WITH", pk_cur(p)->text, NULL);
                        pk_adv(p);
                        continue;
                    }
                    if (pk_word_is(p, "ALL") || pk_word_is(p, "REST") || pk_word_is(p, "NEXT") ||
                        pk_word_is(p, "RECORD") || pk_word_is(p, "FOR") || pk_word_is(p, "WHILE")) {
                        rc = parse_scope_fw(p, &s);
                        if (rc != R_OK) {
                            return rc;
                        }
                        continue;
                    }
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in COPY");
                }
                break;
            }
            case ST_CREATE: {
                static const char *stop[] = { "FROM", NULL };
                char *path = parse_path_until(p, stop);
                st_add_str(&s, path);
                free(path);
                if (pk_word_is(p, "FROM")) {
                    pk_adv(p);
                    char *p2 = parse_path_until(p, NULL);
                    st_add_str(&s, p2);
                    free(p2);
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in CREATE");
                }
                break;
            }
            case ST_TYPE: {
                char *path = parse_path_until(p, NULL);
                st_add_str(&s, path);
                free(path);
                skip_to_eol(p); /* TO PRINT etc. at their milestone */
                break;
            }
            /* ---- M5 single-line statements ---- */
            case ST_ENDMARK:
                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Unexpected END keyword");
            case ST_DO:
                if (pk_cur(p)->kind != TK_WORD) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected procedure name after DO");
                }
                snprintf(s.target, sizeof(s.target), "%s", pk_cur(p)->text);
                for (char *c = s.target; *c; c++) {
                    *c = (char)toupper((unsigned char)*c);
                }
                pk_adv(p);
                if (pk_word_is(p, "WITH")) {
                    pk_adv(p);
                    for (;;) {
                        rx_expr *e = NULL;
                        rc = parse_expr(p, &e);
                        if (rc != R_OK) {
                            return rc;
                        }
                        st_add_expr(&s, e);
                        if (pk_op(p, OP_COMMA)) {
                            pk_adv(p);
                            continue;
                        }
                        break;
                    }
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in DO");
                }
                break;
            case ST_RETURN:
                if (!end_of_line(p)) {
                    rx_expr *e = NULL;
                    rc = parse_expr(p, &e);
                    if (rc != R_OK) {
                        return rc;
                    }
                    st_add_expr(&s, e);
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in RETURN");
                }
                break;
            case ST_EXIT:
            case ST_LOOP:
            case ST_RETRY:
            case ST_CANCEL:
            case ST_SUSPEND:
            case ST_RESUME:
                skip_to_eol(p);
                break;
            case ST_PARAMETERS: {
                /* LPARAMETERS (by-reference) vs PARAMETERS: look at the keyword
                   token just consumed */
                if (p->pos > 0 && rx_strcasecmp(p->toks[p->pos - 1].text, "LPARAMETERS") == 0) {
                    st_add_clause(&s, "LPAR", "1", NULL);
                }
                for (;;) {
                    if (pk_cur(p)->kind != TK_WORD) {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                       "Expected parameter name");
                    }
                    st_add_str(&s, pk_cur(p)->text);
                    pk_adv(p);
                    if (pk_op(p, OP_COMMA)) {
                        pk_adv(p);
                        continue;
                    }
                    break;
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in PARAMETERS");
                }
                break;
            }
            case ST_PRIVATE:
            case ST_PUBLIC: {
                if (pk_word_is(p, "ALL")) {
                    pk_adv(p);
                    st_add_clause(&s, "ALL", "", NULL);
                    if (pk_word_is(p, "LIKE") || pk_word_is(p, "EXCEPT")) {
                        st_add_clause(&s, "ALL", pk_cur(p)->text, NULL);
                        pk_adv(p);
                        if (pk_cur(p)->kind == TK_STR || pk_cur(p)->kind == TK_WORD) {
                            st_add_clause(&s, "PATTERN", pk_cur(p)->text, NULL);
                            pk_adv(p);
                        }
                    }
                } else {
                    for (;;) {
                        if (pk_cur(p)->kind != TK_WORD) {
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected variable name");
                        }
                        st_add_str(&s, pk_cur(p)->text);
                        pk_adv(p);
                        if (pk_op(p, OP_COMMA)) {
                            pk_adv(p);
                            continue;
                        }
                        break;
                    }
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error");
                }
                break;
            }
            case ST_RELEASE:
                if (pk_word_is(p, "MEMVAR") || pk_word_is(p, "MEMORY")) {
                    pk_adv(p);
                }
                if (pk_word_is(p, "ALL")) {
                    pk_adv(p);
                    st_add_clause(&s, "ALL", "", NULL);
                    if (pk_word_is(p, "LIKE") || pk_word_is(p, "EXCEPT")) {
                        st_add_clause(&s, "ALL", pk_cur(p)->text, NULL);
                        pk_adv(p);
                        if (pk_cur(p)->kind == TK_STR || pk_cur(p)->kind == TK_WORD) {
                            st_add_clause(&s, "PATTERN", pk_cur(p)->text, NULL);
                            pk_adv(p);
                        }
                    }
                } else {
                    for (;;) {
                        if (pk_cur(p)->kind != TK_WORD) {
                            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected variable name");
                        }
                        st_add_str(&s, pk_cur(p)->text);
                        pk_adv(p);
                        if (pk_op(p, OP_COMMA)) {
                            pk_adv(p);
                            continue;
                        }
                        break;
                    }
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in RELEASE");
                }
                break;
            case ST_ON:
                /* ON ERROR|ESCAPE [DO proc] | ON KEY LABEL key [DO proc] */
                if (pk_word_is(p, "KEY")) {
                    pk_adv(p);
                    if (!pk_word_is(p, "LABEL")) {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected LABEL after ON KEY");
                    }
                    pk_adv(p);
                    if (pk_cur(p)->kind == TK_WORD || pk_is(p, TK_STR)) {
                        st_add_clause(&s, "KEYLABEL", pk_cur(p)->text, NULL);
                        pk_adv(p);
                    }
                    if (pk_word_is(p, "DO")) {
                        pk_adv(p);
                        if (pk_cur(p)->kind == TK_WORD) {
                            st_add_str(&s, pk_cur(p)->text);
                            pk_adv(p);
                        }
                    }
                } else if (pk_word_is(p, "ERROR") || pk_word_is(p, "ESCAPE")) {
                    st_add_clause(&s, pk_cur(p)->text, "1", NULL);
                    pk_adv(p);
                    if (pk_word_is(p, "DO")) {
                        pk_adv(p);
                        if (pk_cur(p)->kind == TK_WORD) {
                            st_add_str(&s, pk_cur(p)->text);
                            pk_adv(p);
                        }
                    }
                } else {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col,
                                   "Expected ERROR, ESCAPE or KEY LABEL after ON");
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in ON");
                }
                break;
            case ST_WAIT:
                if (pk_word_is(p, "WINDOW")) {
                    st_add_clause(&s, "WINDOW", "1", NULL);
                    pk_adv(p);
                }
                if (pk_word_is(p, "NOWAIT") || pk_word_is(p, "CLEAR")) {
                    pk_adv(p);
                }
                if (!end_of_line(p)) {
                    if (pk_is(p, TK_STR)) {
                        st_add_str(&s, pk_cur(p)->text);
                        pk_adv(p);
                    } else {
                        rx_expr *e = NULL;
                        rc = parse_expr(p, &e);
                        if (rc != R_OK) {
                            return rc;
                        }
                        st_add_expr(&s, e);
                    }
                }
                if (pk_word_is(p, "TIMEOUT")) {
                    pk_adv(p);
                    rx_expr *e = NULL;
                    rc = parse_expr(p, &e);
                    if (rc != R_OK) {
                        return rc;
                    }
                    st_add_clause(&s, "TIMEOUT", "", e);
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in WAIT");
                }
                break;
            case ST_INPUT:
            case ST_ACCEPT: {
                /* [prompt] TO var */
                if (!pk_word_is(p, "TO")) {
                    if (pk_is(p, TK_STR)) {
                        st_add_str(&s, pk_cur(p)->text);
                        pk_adv(p);
                    } else {
                        rx_expr *e = NULL;
                        rc = parse_expr(p, &e);
                        if (rc != R_OK) {
                            return rc;
                        }
                        st_add_expr(&s, e);
                    }
                    if (pk_word_is(p, "TO")) {
                        pk_adv(p);
                    }
                } else {
                    pk_adv(p);
                }
                if (pk_cur(p)->kind != TK_WORD) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected variable after TO");
                }
                snprintf(s.target, sizeof(s.target), "%s", pk_cur(p)->text);
                for (char *c = s.target; *c; c++) {
                    *c = (char)toupper((unsigned char)*c);
                }
                pk_adv(p);
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error");
                }
                break;
            }
            case ST_DIMENSION:
                /* DIMENSION a[d1,d2], b[d1], ... — one kid stmt per array */
                for (;;) {
                    if (pk_cur(p)->kind != TK_WORD) {
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected array name");
                    }
                    rx_stmt *kid = calloc(1, sizeof(rx_stmt));
                    if (!kid) {
                        RX_FATAL("out of memory");
                    }
                    kid->kind = ST_DIMENSION;
                    snprintf(kid->target, sizeof(kid->target), "%s", pk_cur(p)->text);
                    for (char *c = kid->target; *c; c++) {
                        *c = (char)toupper((unsigned char)*c);
                    }
                    pk_adv(p);
                    if (!pk_op(p, OP_LB)) {
                        rx_stmt_free(kid);
                        free(kid);
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected [ after array name");
                    }
                    pk_adv(p);
                    for (;;) {
                        rx_expr *e = NULL;
                        rc = parse_expr(p, &e);
                        if (rc != R_OK) {
                            rx_stmt_free(kid);
                            free(kid);
                            return rc;
                        }
                        st_add_expr(kid, e);
                        if (pk_op(p, OP_COMMA)) {
                            pk_adv(p);
                            continue;
                        }
                        break;
                    }
                    if (!pk_op(p, OP_RB)) {
                        rx_stmt_free(kid);
                        free(kid);
                        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected ] in DIMENSION");
                    }
                    pk_adv(p);
                    st_add_kid(&s, kid);
                    if (pk_op(p, OP_COMMA)) {
                        pk_adv(p);
                        continue;
                    }
                    break;
                }
                if (!end_of_line(p)) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error in DIMENSION");
                }
                break;
            default:
                break;
            }
            return stmt_finish(p, &s);
        }
        /* array element assignment: WORD [i, j] = expr */
        if (pk_cur(p)->kind == TK_WORD && pk_op_ahead_is(p, 1, OP_LB)) {
            s.kind = ST_ASSIGN;
            snprintf(s.target, sizeof(s.target), "%s", pk_cur(p)->text);
            for (char *c = s.target; *c; c++) {
                *c = (char)toupper((unsigned char)*c);
            }
            pk_adv(p);
            pk_adv(p); /* [ */
            for (;;) {
                rx_expr *e = NULL;
                int rc2 = parse_expr(p, &e);
                if (rc2 != R_OK) {
                    return rc2;
                }
                st_add_expr(&s, e);
                s.nidx++;
                if (pk_op(p, OP_COMMA)) {
                    pk_adv(p);
                    continue;
                }
                break;
            }
            if (!pk_op(p, OP_RB)) {
                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected ] in array assignment");
            }
            pk_adv(p);
            if (!pk_op(p, OP_EQ)) {
                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected = in array assignment");
            }
            pk_adv(p);
            rx_expr *v = NULL;
            int rc3 = parse_expr(p, &v);
            if (rc3 != R_OK) {
                return rc3;
            }
            st_add_expr(&s, v);
            if (!end_of_line(p)) {
                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error");
            }
            return stmt_finish(p, &s);
        }
        /* assignment: WORD = expr  (or m.var = expr) */
        if (pk_op_ahead_is(p, 1, OP_EQ)) {
            s.kind = ST_ASSIGN;
            snprintf(s.target, sizeof(s.target), "%s", pk_cur(p)->text);
            for (char *c = s.target; *c; c++) {
                *c = (char)toupper((unsigned char)*c);
            }
            pk_adv(p);
            pk_adv(p);
            rx_expr *e = NULL;
            int rc = parse_expr(p, &e);
            if (rc != R_OK) {
                return rc;
            }
            st_add_expr(&s, e);
            if (!end_of_line(p)) {
                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error");
            }
            return stmt_finish(p, &s);
        }
        if (pk_op_ahead_is(p, 1, OP_DOT) && pk_ahead(p, 2)->kind == TK_WORD &&
            pk_op_ahead_is(p, 3, OP_EQ)) {
            s.kind = ST_ASSIGN;
            snprintf(s.target, sizeof(s.target), "%s.%s", pk_cur(p)->text, pk_ahead(p, 2)->text);
            for (char *c = s.target; *c; c++) {
                *c = (char)toupper((unsigned char)*c);
            }
            pk_adv(p);
            pk_adv(p);
            pk_adv(p);
            pk_adv(p);
            rx_expr *e = NULL;
            int rc = parse_expr(p, &e);
            if (rc != R_OK) {
                return rc;
            }
            st_add_expr(&s, e);
            if (!end_of_line(p)) {
                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error");
            }
            return stmt_finish(p, &s);
        }
    }

    /* expression statement (any leading token kind: 2+3*4, "hi", f(x), ...) */
    s.kind = ST_EXPR;
    rx_expr *e = NULL;
    int rc = parse_expr(p, &e);
    if (rc != R_OK) {
        return rc;
    }
    st_add_expr(&s, e);
    if (!end_of_line(p)) {
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Syntax error");
    }
    return stmt_finish(p, &s);
}

/* BROWSE clause grammar (spec §9.1, M4):
   BROWSE [FIELDS f[:H="t"][:W=n][:P="pic"][:V=e[:E="m"]][:F][:R], ...]
          [PARTITION n] [LEDIT|REDIT] [FREEZE f] [LOCK n] [WIDTH n]
          [NOAPPEND] [NODELETE] [NOEDIT|NOMODIFY] [NOWAIT] [NOOPTIMIZE]
          [TITLE "t"] [FOR e] [WHEN e] [VALID[:F] e[:E="m"]]
          [IN [WINDOW w] | SCREEN] */
static int parse_browse_num(parser *p, char *word, size_t wordsz)
{
    rx_tok *t = pk_cur(p);
    if (t->kind != TK_NUM) {
        return pk_fail(p, t->line, t->col, "Expected a number");
    }
    snprintf(word, wordsz, "%d", (int)t->num);
    pk_adv(p);
    return R_OK;
}

static int parse_browse_str(parser *p, char *out, size_t outsz, const char *what)
{
    if (!pk_is(p, TK_STR)) {
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, what);
    }
    snprintf(out, outsz, "%s", pk_cur(p)->text);
    pk_adv(p);
    return R_OK;
}

/* colon options shared by FIELDS items and the record-level VALID clause */
static int parse_colon_opt(parser *p, char opt, rx_bcol *col, rx_expr **valid_out, char *verr,
                           size_t verrsz)
{
    switch (opt) {
    case 'H':
        if (pk_expect_op(p, OP_EQ, "Expected '=' after :H") != R_OK) {
            return R_ERR_SYNTAX;
        }
        return parse_browse_str(p, col ? col->header : verr, col ? sizeof(col->header) : verrsz,
                                "Expected a quoted title after :H =");
    case 'P':
        /* PICTURE: parsed, input masking lands with the M7 forms runtime */
        if (pk_expect_op(p, OP_EQ, "Expected '=' after :P") != R_OK) {
            return R_ERR_SYNTAX;
        }
        if (!pk_is(p, TK_STR)) {
            return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected a quoted picture after :P =");
        }
        pk_adv(p);
        return R_OK;
    case 'W': {
        if (pk_expect_op(p, OP_EQ, "Expected '=' after :W") != R_OK) {
            return R_ERR_SYNTAX;
        }
        rx_tok *w = pk_cur(p);
        if (w->kind != TK_NUM) {
            return pk_fail(p, w->line, w->col, "Expected a number after :W =");
        }
        if (col) {
            col->width = (int)w->num;
        }
        pk_adv(p);
        return R_OK;
    }
    case 'V': {
        if (pk_expect_op(p, OP_EQ, "Expected '=' after :V") != R_OK) {
            return R_ERR_SYNTAX;
        }
        rx_expr *v = NULL;
        int rc = parse_expr(p, &v);
        if (rc != R_OK) {
            return rc;
        }
        if (col) {
            col->valid = v;
        } else if (valid_out) {
            *valid_out = v;
        } else {
            rx_expr_free(v);
        }
        return R_OK;
    }
    case 'E':
        if (pk_expect_op(p, OP_EQ, "Expected '=' after :E") != R_OK) {
            return R_ERR_SYNTAX;
        }
        return parse_browse_str(p, verr, verrsz, "Expected a quoted message after :E =");
    case 'F':
        if (col) {
            col->fflag = 1;
        }
        return R_OK;
    case 'R':
        if (col) {
            col->rflag = 1;
        }
        return R_OK;
    default:
        return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Unknown BROWSE option letter");
    }
}

static int parse_browse(parser *p, rx_stmt *s)
{
    while (!end_of_line(p)) {
        rx_tok *t = pk_cur(p);
        if (t->kind != TK_WORD) {
            return pk_fail(p, t->line, t->col, "Syntax error in BROWSE");
        }
        if (rx_strcasecmp(t->text, "FIELDS") == 0) {
            pk_adv(p);
            for (;;) {
                rx_bcol col;
                memset(&col, 0, sizeof(col));
                rx_expr *e = NULL;
                int rc = parse_expr(p, &e);
                if (rc != R_OK) {
                    return rc;
                }
                if (e->kind == EX_VAR) {
                    col.is_field = 1;
                    snprintf(col.name, sizeof(col.name), "%.127s", e->name);
                    snprintf(col.header, sizeof(col.header), "%.63s", e->name);
                    rx_expr_free(e);
                } else {
                    col.e = e;
                    snprintf(col.header, sizeof(col.header), "Expr");
                }
                while (pk_op(p, OP_COLON)) {
                    pk_adv(p);
                    rx_tok *o = pk_cur(p);
                    if (o->kind != TK_WORD || strlen(o->text) != 1) {
                        return pk_fail(p, o->line, o->col, "Expected an option letter after ':'");
                    }
                    char opt = (char)toupper((unsigned char)o->text[0]);
                    pk_adv(p);
                    rc = parse_colon_opt(p, opt, &col, NULL, col.verr, sizeof(col.verr));
                    if (rc != R_OK) {
                        return rc;
                    }
                }
                st_add_bcol(s, &col);
                if (pk_op(p, OP_COMMA)) {
                    pk_adv(p);
                    continue;
                }
                break;
            }
        } else if (rx_strcasecmp(t->text, "PARTITION") == 0 ||
                   rx_strcasecmp(t->text, "LOCK") == 0 || rx_strcasecmp(t->text, "WIDTH") == 0) {
            char key[16];
            snprintf(key, sizeof(key), "%s", t->text);
            for (char *q = key; *q; q++) {
                *q = (char)toupper((unsigned char)*q);
            }
            pk_adv(p);
            char word[24];
            int rc = parse_browse_num(p, word, sizeof(word));
            if (rc != R_OK) {
                return rc;
            }
            st_add_clause(s, key, word, NULL);
        } else if (rx_strcasecmp(t->text, "FREEZE") == 0) {
            pk_adv(p);
            if (pk_cur(p)->kind != TK_WORD) {
                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected a field after FREEZE");
            }
            st_add_clause(s, "FREEZE", pk_cur(p)->text, NULL);
            pk_adv(p);
        } else if (rx_strcasecmp(t->text, "TITLE") == 0) {
            pk_adv(p);
            char title[64];
            int rc = parse_browse_str(p, title, sizeof(title), "Expected a quoted title after TITLE");
            if (rc != R_OK) {
                return rc;
            }
            st_add_clause(s, "TITLE", title, NULL);
        } else if (rx_strcasecmp(t->text, "FOR") == 0 || rx_strcasecmp(t->text, "WHEN") == 0) {
            char key[8];
            snprintf(key, sizeof(key), "%s", t->text);
            for (char *q = key; *q; q++) {
                *q = (char)toupper((unsigned char)*q);
            }
            pk_adv(p);
            rx_expr *e = NULL;
            int rc = parse_expr(p, &e);
            if (rc != R_OK) {
                return rc;
            }
            st_add_clause(s, key, "", e);
        } else if (rx_strcasecmp(t->text, "VALID") == 0) {
            pk_adv(p);
            if (pk_op(p, OP_COLON)) { /* VALID:F */
                pk_adv(p);
                if (!pk_word_is(p, "F")) {
                    return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected F after VALID:");
                }
                pk_adv(p);
            }
            rx_expr *e = NULL;
            int rc = parse_expr(p, &e);
            if (rc != R_OK) {
                return rc;
            }
            st_add_clause(s, "VALID", "", e);
            char verr[160] = "";
            while (pk_op(p, OP_COLON)) {
                pk_adv(p);
                rx_tok *o = pk_cur(p);
                if (o->kind != TK_WORD || strlen(o->text) != 1) {
                    return pk_fail(p, o->line, o->col, "Expected an option letter after ':'");
                }
                char opt = (char)toupper((unsigned char)o->text[0]);
                pk_adv(p);
                rc = parse_colon_opt(p, opt, NULL, NULL, verr, sizeof(verr));
                if (rc != R_OK) {
                    return rc;
                }
            }
            if (verr[0]) {
                st_add_clause(s, "VERR", verr, NULL);
            }
        } else if (rx_strcasecmp(t->text, "IN") == 0) {
            pk_adv(p);
            if (pk_word_is(p, "WINDOW")) {
                pk_adv(p);
                if (pk_cur(p)->kind == TK_WORD) {
                    pk_adv(p); /* user windows land with M5 DEFINE WINDOW */
                }
            } else if (pk_word_is(p, "SCREEN")) {
                pk_adv(p);
            } else {
                return pk_fail(p, pk_cur(p)->line, pk_cur(p)->col, "Expected WINDOW or SCREEN after IN");
            }
        } else {
            /* flag clauses: LEDIT REDIT NOAPPEND NODELETE NOEDIT NOMODIFY NOWAIT
               NOOPTIMIZE SCREEN */
            static const char *flags[] = { "LEDIT",   "REDIT",  "NOAPPEND", "NODELETE", "NOEDIT",
                                           "NOMODIFY", "NOWAIT", "NOOPTIMIZE", "SCREEN", NULL };
            int known = 0;
            for (int k = 0; flags[k]; k++) {
                if (rx_strcasecmp(t->text, flags[k]) == 0) {
                    known = 1;
                    break;
                }
            }
            if (!known) {
                return pk_fail(p, t->line, t->col, "Syntax error in BROWSE");
            }
            char key[16];
            snprintf(key, sizeof(key), "%s", t->text);
            for (char *q = key; *q; q++) {
                *q = (char)toupper((unsigned char)*q);
            }
            st_add_clause(s, key, "1", NULL);
            pk_adv(p);
        }
    }
    return R_OK;
}

int rx_parse_src(rx_tok *toks, size_t ntok, const char *src, rx_stmt **out, size_t *nout,
                 r_err *err)
{
    parser p;
    memset(&p, 0, sizeof(p));
    p.toks = toks;
    p.n = ntok;
    p.pos = 0;
    p.src = src;
    p.err = err;

    while (!p.failed) {
        /* skip newlines */
        while (pk_is(&p, TK_NEWLINE)) {
            pk_adv(&p);
        }
        if (pk_is(&p, TK_EOF)) {
            break;
        }
        size_t pos_before = p.pos;
        int rc = parse_stmt(&p);
        if (rc != R_OK) {
            break;
        }
        if (p.nstmt > 0) {
            set_raw_span(&p, &p.stmts[p.nstmt - 1], pos_before);
        }
        if (p.pos == pos_before && !pk_is(&p, TK_NEWLINE) && !pk_is(&p, TK_EOF)) {
            pk_fail(&p, pk_cur(&p)->line, pk_cur(&p)->col, "Syntax error");
            break;
        }
        /* consume the line terminator, honoring ';' continuation */
        while (pk_op(&p, OP_SEMI)) {
            pk_adv(&p);
            while (pk_is(&p, TK_NEWLINE)) {
                pk_adv(&p);
            }
            if (pk_is(&p, TK_EOF)) {
                break;
            }
            /* continuation: parse another fragment appended? FoxPro semantics:
               ';' joins lines into one logical line; our per-line parser already
               stopped at ';', so re-parse is not needed for M1 single-line cmds. */
            break;
        }
        while (pk_is(&p, TK_NEWLINE)) {
            pk_adv(&p);
        }
    }
    if (p.failed) {
        for (size_t i = 0; i < p.nstmt; i++) {
            rx_stmt_free(&p.stmts[i]);
        }
        free(p.stmts);
        return R_ERR_SYNTAX;
    }
    *out = p.stmts;
    *nout = p.nstmt;
    return R_OK;
}

int rx_parse(rx_tok *toks, size_t ntok, rx_stmt **out, size_t *nout, r_err *err)
{
    return rx_parse_src(toks, ntok, NULL, out, nout, err);
}
