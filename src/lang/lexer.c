/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file lexer.c
 * @brief Tokenizer (spec §7.2). One token per lexical unit with 1-based
 *        line/col for §6.6 diagnostics. Handles && and * line comments,
 *        quoted strings with doubled-quote escapes, {^date} literals,
 *        numeric literals, and the operator set of §7.3.
 */
#include "lang/lang.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util/util.h"

typedef struct lexer {
    const char* src;
    size_t pos, len;
    size_t tok_start; /**< offset where the current token began */
    int line, col;
    const char* fname;
    rx_tok* toks;
    size_t n, cap;
    r_err* err;
    int failed;
} lexer;

static int lx_emit(lexer* lx, tk_kind kind, const char* text, size_t tn, int ival, double num) {
    if (lx->n == lx->cap) {
        size_t nc = lx->cap ? lx->cap * 2 : 64;
        rx_tok* nt = realloc(lx->toks, nc * sizeof(rx_tok));
        if (!nt) {
            RX_FATAL("out of memory");
        }
        lx->toks = nt;
        lx->cap = nc;
    }
    rx_tok* t = &lx->toks[lx->n++];
    t->kind = kind;
    t->line = lx->line;
    t->col = lx->col;
    t->off = lx->tok_start;
    t->ival = ival;
    t->num = num;
    t->text = malloc(tn + 1);
    if (!t->text) {
        RX_FATAL("out of memory");
    }
    memcpy(t->text, text, tn);
    t->text[tn] = '\0';
    return R_OK;
}

static int lx_fail(lexer* lx, const char* msg) {
    if (!lx->failed) {
        lx->failed = 1;
        lx->err->code = R_ERR_SYNTAX;
        lx->err->line = lx->line;
        lx->err->col = lx->col;
        snprintf(lx->err->src_file, sizeof(lx->err->src_file), "%s", lx->fname);
        snprintf(lx->err->msg, sizeof(lx->err->msg), "%s", msg);
    }
    return R_ERR_SYNTAX;
}

static char lx_peek(lexer* lx) {
    return lx->pos < lx->len ? lx->src[lx->pos] : '\0';
}

static char lx_peek2(lexer* lx) {
    return lx->pos + 1 < lx->len ? lx->src[lx->pos + 1] : '\0';
}

static char lx_get(lexer* lx) {
    char c = lx_peek(lx);
    if (lx->pos < lx->len) {
        lx->pos++;
        if (c == '\n') {
            lx->line++;
            lx->col = 1;
        } else {
            lx->col++;
        }
    }
    return c;
}

static void lx_skip_inline_comment(lexer* lx) {
    /* && comment to end of line */
    while (lx->pos < lx->len && lx->src[lx->pos] != '\n') {
        lx->pos++;
        lx->col++;
    }
}

/* after a line-initial TEXT word: consume the raw block up to the ENDTEXT
   line, emitting a synthetic ENDTEXT word token so the parser can find the
   block end (parse_text captures the verbatim span from token offsets) */
static void lx_skip_text_block(lexer* lx) {
    while (lx->pos < lx->len && lx->src[lx->pos] != '\n') {
        lx_get(lx);
    }
    for (;;) {
        while (lx->pos < lx->len &&
               (lx->src[lx->pos] == ' ' || lx->src[lx->pos] == '\t' || lx->src[lx->pos] == '\r')) {
            lx_get(lx);
        }
        if (lx->pos >= lx->len) {
            return;
        }
        if (lx->pos + 7 <= lx->len && strncasecmp(lx->src + lx->pos, "ENDTEXT", 7) == 0 &&
            (lx->pos + 7 >= lx->len ||
             !(isalnum((unsigned char)lx->src[lx->pos + 7]) || lx->src[lx->pos + 7] == '_'))) {
            size_t off = lx->pos;
            int line = lx->line, col = lx->col;
            for (int i = 0; i < 7; i++) {
                lx_get(lx);
            }
            lx_emit(lx, TK_WORD, "ENDTEXT", 7, 0, 0);
            lx->toks[lx->n - 1].off = off;
            lx->toks[lx->n - 1].line = line;
            lx->toks[lx->n - 1].col = col;
            return;
        }
        while (lx->pos < lx->len && lx->src[lx->pos] != '\n') {
            lx_get(lx);
        }
        if (lx->pos < lx->len) {
            lx_get(lx);
        }
    }
}

int rx_lex(const char* src, const char* fname, rx_tok** out, size_t* nout, r_err* err) {
    lexer lx;
    memset(&lx, 0, sizeof(lx));
    lx.src = src;
    lx.len = strlen(src);
    lx.line = 1;
    lx.col = 1;
    lx.fname = fname ? fname : "<command>";
    lx.err = err;

    while (lx.pos < lx.len && !lx.failed) {
        lx.tok_start = lx.pos;
        char c = lx_peek(&lx);
        if (c == ' ' || c == '\t' || c == '\r') {
            lx_get(&lx);
            continue;
        }
        if (c == '\n') {
            int line = lx.line, col = lx.col;
            lx_get(&lx);
            lx_emit(&lx, TK_NEWLINE, "\n", 1, 0, 0);
            lx.toks[lx.n - 1].line = line;
            lx.toks[lx.n - 1].col = col;
            continue;
        }
        if (c == '&' && lx_peek2(&lx) == '&') {
            lx_skip_inline_comment(&lx);
            continue;
        }
        if (c == '*' && (lx.n == 0 || lx.toks[lx.n - 1].kind == TK_NEWLINE)) {
            lx_skip_inline_comment(&lx); /* * full-line comment */
            continue;
        }
        int line = lx.line, col = lx.col;
        if (c == '"' || c == '\'') {
            char q = lx_get(&lx);
            strbuf sb;
            sb_init(&sb);
            int closed = 0;
            while (lx.pos < lx.len) {
                char d = lx_get(&lx);
                if (d == q) {
                    if (lx_peek(&lx) == q) {
                        lx_get(&lx);
                        sb_putc(&sb, q);
                    } else {
                        closed = 1;
                        break;
                    }
                } else {
                    sb_putc(&sb, d);
                }
            }
            if (!closed) {
                sb_free(&sb);
                return lx_fail(&lx, "Unterminated string");
            }
            if (!sb.buf) {
                sb_putc(&sb, '\0');
                sb.len = 0;
            }
            lx_emit(&lx, TK_STR, sb.buf, sb.len, 0, 0);
            lx.toks[lx.n - 1].line = line;
            lx.toks[lx.n - 1].col = col;
            sb_free(&sb);
            continue;
        }
        if (c == '{') { /* {^yyyy-mm-dd[ hh:mm[:ss]]} or {} empty date */
            lx_get(&lx);
            strbuf sb;
            sb_init(&sb);
            while (lx.pos < lx.len && lx_peek(&lx) != '}') {
                sb_putc(&sb, lx_get(&lx));
            }
            if (lx_peek(&lx) != '}') {
                sb_free(&sb);
                return lx_fail(&lx, "Unterminated date literal");
            }
            lx_get(&lx);
            lx_emit(&lx, TK_OP, "{", 1, OP_LP, 0); /* marker reused below */
            lx.toks[lx.n - 1].kind = TK_WORD;      /* treat as date literal word */
            free(lx.toks[lx.n - 1].text);
            if (sb.len && sb.buf[0] == '^') {
                lx.toks[lx.n - 1].text = malloc(sb.len + 1);
                if (!lx.toks[lx.n - 1].text) {
                    RX_FATAL("out of memory");
                }
                memcpy(lx.toks[lx.n - 1].text, sb.buf, sb.len);
                lx.toks[lx.n - 1].text[sb.len] = '\0';
            } else {
                lx.toks[lx.n - 1].text = rx_strdup("{}");
            }
            lx.toks[lx.n - 1].line = line;
            lx.toks[lx.n - 1].col = col;
            sb_free(&sb);
            continue;
        }
        if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)lx_peek2(&lx)))) {
            size_t start = lx.pos;
            while (lx.pos < lx.len &&
                   (isdigit((unsigned char)lx.src[lx.pos]) || lx.src[lx.pos] == '.')) {
                lx_get(&lx);
            }
            size_t tn = lx.pos - start;
            char* tmp = malloc(tn + 1);
            if (!tmp) {
                RX_FATAL("out of memory");
            }
            memcpy(tmp, lx.src + start, tn);
            tmp[tn] = '\0';
            double v = strtod(tmp, NULL);
            lx_emit(&lx, TK_NUM, tmp, tn, 0, v);
            lx.toks[lx.n - 1].line = line;
            lx.toks[lx.n - 1].col = col;
            free(tmp);
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = lx.pos;
            while (lx.pos < lx.len && (isalnum((unsigned char)lx.src[lx.pos]) ||
                                       lx.src[lx.pos] == '_' || lx.src[lx.pos] == '$')) {
                lx_get(&lx);
            }
            size_t tn = lx.pos - start;
            int line_start = lx.n == 0 || lx.toks[lx.n - 1].kind == TK_NEWLINE;
            lx_emit(&lx, TK_WORD, lx.src + start, tn, 0, 0);
            lx.toks[lx.n - 1].line = line;
            lx.toks[lx.n - 1].col = col;
            if (line_start && tn == 4 && strncasecmp(lx.src + start, "TEXT", 4) == 0) {
                lx_skip_text_block(&lx);
            }
            continue;
        }
        /* two-char operators first */
        char c2 = lx_peek2(&lx);
        if (c == '=' && c2 == '=') {
            lx_get(&lx);
            lx_get(&lx);
            lx_emit(&lx, TK_OP, "==", 2, OP_EEQ, 0);
        } else if ((c == '<' || c == '>' || c == '!' || c == '=') && c2 == '=') {
            lx_get(&lx);
            lx_get(&lx);
            int op = c == '<' ? OP_LE : c == '>' ? OP_GE : c == '!' ? OP_NE : OP_EQ;
            lx_emit(&lx, TK_OP, "", 0, op, 0);
        } else if (c == '<' && c2 == '>') {
            lx_get(&lx);
            lx_get(&lx);
            lx_emit(&lx, TK_OP, "<>", 2, OP_NE, 0);
        } else if (c == '-' && c2 == '>') {
            lx_get(&lx);
            lx_get(&lx);
            lx_emit(&lx, TK_OP, "->", 2, OP_ARROW, 0);
        } else if (c == '*' && c2 == '*') {
            lx_get(&lx);
            lx_get(&lx);
            lx_emit(&lx, TK_OP, "**", 2, OP_POW, 0);
        } else {
            lx_get(&lx);
            int op = OP_NONE;
            switch (c) {
            case '+':
                op = OP_PLUS;
                break;
            case '-':
                op = OP_MINUS;
                break;
            case '*':
                op = OP_MUL;
                break;
            case '/':
                op = OP_DIV;
                break;
            case '%':
                op = OP_MOD;
                break;
            case '^':
                op = OP_POW;
                break;
            case '=':
                op = OP_EQ;
                break;
            case '<':
                op = OP_LT;
                break;
            case '>':
                op = OP_GT;
                break;
            case '$':
                op = OP_IN;
                break;
            case '(':
                op = OP_LP;
                break;
            case ')':
                op = OP_RP;
                break;
            case ',':
                op = OP_COMMA;
                break;
            case '.':
                op = OP_DOT;
                break;
            case '&':
                op = OP_AMP;
                break;
            case '[':
                op = OP_LB;
                break;
            case ']':
                op = OP_RB;
                break;
            case ':':
                op = OP_COLON;
                break;
            case ';':
                op = OP_SEMI;
                break;
            case '?':
                op = OP_Q;
                break;
            case '@':
                op = OP_AT;
                break;
            default:
                return lx_fail(&lx, "Unexpected character");
            }
            lx_emit(&lx, TK_OP, "", 0, op, 0);
        }
        lx.toks[lx.n - 1].line = line;
        lx.toks[lx.n - 1].col = col;
    }
    lx_emit(&lx, TK_EOF, "", 0, 0, 0);
    *out = lx.toks;
    *nout = lx.n;
    return lx.failed ? R_ERR_SYNTAX : R_OK;
}

void rx_lex_free(rx_tok* toks, size_t n) {
    if (!toks) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(toks[i].text);
    }
    free(toks);
}
