/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file lang.h
 * @brief Interpreter core: lexer, parser, expression compiler, VM (spec §7).
 *        lang calls data and ui through these interfaces; it never
 *        includes data internals directly.
 */
#ifndef REYNARD_LANG_H
#define REYNARD_LANG_H

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "reynard/rval.h"
#include "reynard/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ lexer ============================ */

typedef enum {
    TK_WORD,    /**< identifier or keyword */
    TK_STR,     /**< quoted string */
    TK_NUM,     /**< numeric literal */
    TK_OP,      /**< operator / punctuation, op code in ival */
    TK_NEWLINE, /**< end of logical input line */
    TK_EOF
} tk_kind;

/* operator codes for TK_OP */
enum {
    OP_NONE = 0,
    OP_PLUS, OP_MINUS, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_EQ, OP_EEQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_IN, /* $ */
    OP_LP, OP_RP, OP_COMMA, OP_DOT, OP_AMP, OP_ARROW,           /* ( ) , . & -> */
    OP_LB, OP_RB, OP_COLON, OP_SEMI, OP_Q,                      /* ? */
    OP_AND_LOGICAL, OP_OR_LOGICAL, OP_NOT_LOGICAL               /* .AND. AND etc. */
};

typedef struct rx_tok {
    tk_kind kind;
    int line, col;
    char *text;   /**< owned: WORD text, STR contents, NUM text, OP symbol */
    int ival;     /**< OP_* for TK_OP */
    double num;   /**< TK_NUM value */
} rx_tok;

/** Lex a whole source buffer. Caller frees with rx_lex_free. */
int rx_lex(const char *src, const char *fname, rx_tok **out, size_t *nout, r_err *err);
void rx_lex_free(rx_tok *toks, size_t n);

/* ============================ parser ============================ */

typedef struct rx_expr rx_expr;

typedef enum {
    EX_NUM, EX_STR, EX_LOGIC, EX_DATE, EX_UNDEF,
    EX_VAR,    /**< bare identifier: memvar else field of current work area */
    EX_FIELD,  /**< alias->field or m. prefix resolved at runtime */
    EX_ARRAY,  /**< name[e, ...] */
    EX_BIN,    /**< op, l, r */
    EX_UN,     /**< op, r */
    EX_FUNC    /**< fid, args */
} ex_kind;

struct rx_expr {
    ex_kind kind;
    int line, col;
    int op;                 /**< OP_* for EX_BIN/EX_UN */
    int fid;                /**< function id for EX_FUNC */
    rx_expr *l, *r;
    rx_expr **args;
    size_t nargs;
    rval lit;               /**< owned literal for EX_NUM/STR/LOGIC/DATE */
    char name[128];         /**< EX_VAR/EX_FIELD/EX_ARRAY (uppercased) */
};

typedef enum {
    ST_PRINT,    /**< ? exprs */
    ST_PRINTN,   /**< ?? exprs */
    ST_USE,
    ST_CLOSE,
    ST_SELECT,
    ST_LIST,
    ST_DISPLAY,
    ST_GO,
    ST_SKIP,
    ST_ASSIGN,   /**< target = expr */
    ST_STORE,    /**< STORE expr TO v1, v2 */
    ST_SET,
    ST_QUIT,
    ST_CLEAR,
    ST_EXPR,
    ST_BROWSE,
    ST_APPEND,    /**< APPEND [BLANK | FROM file [TYPE] SDF|DELIMITED ...] */
    ST_REPLACE,   /**< REPLACE [scope] [FOR/WHILE] f WITH e, ... */
    ST_DELETE,    /**< DELETE [scope] [FOR/WHILE] */
    ST_RECALL,    /**< RECALL [scope] [FOR/WHILE] */
    ST_PACK,
    ST_ZAP,
    ST_LOCATE,    /**< LOCATE [scope] FOR e [WHILE e] */
    ST_CONTINUE,
    ST_COPY,      /**< COPY TO f [FIELDS..] [scope] [TYPE SDF|DELIMITED..] | COPY STRUCTURE TO f */
    ST_CREATE,    /**< CREATE f [FROM structure-dbf] */
    ST_FLUSH,
    ST_TYPE,      /**< TYPE file */
    ST_UNLOCK,    /**< UNLOCK [ALL] */
    ST_EMPTY     /**< comment-only line */
} st_kind;

typedef struct rx_clause {
    char key[24];   /**< e.g. SCOPE, IN, ALIAS, TO */
    char word[64];  /**< keyword value when applicable */
    rx_expr *e;     /**< expression value when applicable */
} rx_clause;

typedef struct rx_stmt {
    st_kind kind;
    int line, col;
    rx_expr **exprs;
    size_t nexpr, capexpr;
    rx_clause *clauses;
    size_t nclause;
    char target[128]; /**< assignment target / raw text */
    char **strs;      /**< misc words (SET args etc.) */
    size_t nstrs;
} rx_stmt;

/** Parse a token stream into statements (one per logical line). */
int rx_parse(rx_tok *toks, size_t ntok, rx_stmt **out, size_t *nout, r_err *err);
void rx_stmt_free(rx_stmt *s);
void rx_expr_free(rx_expr *e);

/* ============================ expression compiler ============================ */

enum {
    BOP_NOP,
    BOP_PUSH_STR, BOP_PUSH_NUM, BOP_PUSH_LOGIC, BOP_PUSH_DATE,
    BOP_VAR, BOP_FIELD,
    BOP_NOT, BOP_NEG,
    BOP_ADD, BOP_SUB, BOP_MUL, BOP_DIV, BOP_MOD, BOP_POW,
    BOP_EQ, BOP_EEQ, BOP_NE, BOP_LT, BOP_LE, BOP_GT, BOP_GE, BOP_IN,
    BOP_JMPF, BOP_JMPT, /* short-circuit, arg = absolute target */
    BOP_FUNC, BOP_ARGC  /* BOP_ARGC follows BOP_FUNC with the arg count */
};

typedef struct rx_code {
    uint8_t *ops;
    uint32_t *arg;   /**< per-op argument (index into tables) */
    size_t n, cap;
    double *nums;
    size_t nnums, capnums;
    char **strs;     /**< owned string constants */
    size_t nstrs, capstrs;
} rx_code;

int rx_compile_expr(const rx_expr *e, rx_code *code, r_err *err);
void rx_code_free(rx_code *code);

/* ============================ memvars ============================ */

typedef struct rx_ctx rx_ctx;

typedef struct rx_memvar {
    char name[64]; /**< uppercased */
    rval val;      /**< owned */
} rx_memvar;

rx_memvar *rx_memvar_find(struct rx_ctx *ctx, const char *name);
int rx_memvar_set(struct rx_ctx *ctx, const char *name, const rval *v);
void rx_memvar_clear(struct rx_ctx *ctx);

/* ============================ context / VM ============================ */

typedef struct rx_set {
    int talk;       /**< SET TALK, default ON */
    int headings;   /**< SET HEADINGS, default ON */
    int exact;      /**< SET EXACT, default OFF */
    int fixed;      /**< SET FIXED, default OFF */
    int century;    /**< SET CENTURY, default OFF */
    int deleted;    /**< SET DELETED, default OFF */
    int escape;     /**< SET ESCAPE, default ON */
    int console;    /**< SET CONSOLE, default ON */
    int safety;     /**< SET SAFETY, default ON */
    int clock;      /**< SET CLOCK */
    int mouse;      /**< SET MOUSE, default ON */
    int decimals;   /**< SET DECIMALS, default 2 */
    int date_fmt;   /**< SET DATE, 0=AMERICAN */
    int exclusive;  /**< SET EXCLUSIVE, default ON */
    int reprocess;  /**< SET REPROCESS, 0 = no retry, <0 = AUTOMATIC */
    int multilocks; /**< SET MULTILOCKS, default OFF */
    char defdir[260];
    char path[1024];
} rx_set;

struct rx_ctx {
    FILE *out;          /**< batch stdout (or -o file); NULL in interactive */
    int (*out_fn)(void *ud, const char *s, size_t n); /**< interactive output hook (M3) */
    void *out_ud;
    void (*msg_line)(void *ud, const char *s); /**< interactive message-line hook (M3) */
    void *msg_ud;
    int test_mode;      /**< -t: DATE()/TIME()/RAND() frozen to seed */
    uint32_t rng;       /**< LCG state, seeded 1 in test mode */
    rx_set set;
    rx_memvar *vars;
    size_t nvars, capvars;
    r_err err;          /**< last error */
    int err_printed;    /**< rx_raise already emitted ctx->err to stderr */
    int abort;          /**< set when batch must stop */
    int quit;           /**< QUIT seen: stop cleanly with exit 0 */
    char prog[260];     /**< current source file name for diagnostics */
    const char *src;    /**< whole source buffer (for caret lines) */
    rx_code filter;     /**< SET FILTER TO compiled expression */
    int filter_on;      /**< SET FILTER active */
    rx_expr *locate_expr; /**< LOCATE ... FOR expression, reused by CONTINUE */
    int locate_kind;    /**< scope kind of the active LOCATE */
    uint32_t locate_n;  /**< scope count of the active LOCATE */
    int locate_active;  /**< LOCATE issued, chain still open */
};


void rx_ctx_init(rx_ctx *ctx, FILE *out, int test_mode);
void rx_ctx_free(rx_ctx *ctx);

/** Lex+parse+execute one source buffer (interactive Command window, main). */
int rx_run_source(rx_ctx *ctx, const char *src, const char *name);

/** Evaluate one expression string (calculator dialog); result in *out. */
int rx_eval_str(rx_ctx *ctx, const char *src, rval *out, r_err *err);

/** Parser keyword table (uppercase words), for Command-window completion. */
const char *const *rx_keyword_list(size_t *n);

/** Evaluate compiled bytecode; result left in *out. */
int rx_vm_run(rx_ctx *ctx, const rx_code *code, rval *out, r_err *err);
/** Evaluate an expression AST (compile + run). */
int rx_eval(rx_ctx *ctx, const rx_expr *e, rval *out, r_err *err);
/** Execute a statement list. */
int rx_exec(rx_ctx *ctx, rx_stmt *stmts, size_t n);

/* ============================ errors ============================ */

/** Raise a structured error; in batch formats §6.6 text to stderr. */
int rx_raise(rx_ctx *ctx, int code, int line, int col, const char *fmt, ...);
/** Format "Error in line N col C of FILE: msg" + source caret (needs src). */
void rx_format_error(const r_err *err, const char *srcline, char *out, size_t outsz);

/* ============================ output ============================ */

int rx_out(rx_ctx *ctx, const char *s, size_t n);
int rx_outs(rx_ctx *ctx, const char *s);
int rx_outf(rx_ctx *ctx, const char *fmt, ...);

/* ============================ functions ============================ */

/** Resolve a function name (>=4 letter prefix) to an id, -1 + err if none. */
int rx_func_id(const char *name, r_err *err);
/** Call function id with argc argv (not owned). */
int rx_func_call(rx_ctx *ctx, int fid, size_t argc, const rval *argv, rval *out, r_err *err);

/* render a value per display rules into sb (used by ?/?? and LIST) */
struct strbuf;
int rx_vm_display(struct rx_ctx *ctx, const rval *v, struct strbuf *sb);

/* ---- command handlers (cmds_*.c) ---- */
int rx_cmd_print(rx_ctx *ctx, rx_stmt *s, int newline);
int rx_cmd_use(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_close(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_select(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_list(rx_ctx *ctx, rx_stmt *s, int display);
int rx_cmd_go(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_skip(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_assign(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_store(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_set(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_browse(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_append(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_replace(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_delete(rx_ctx *ctx, rx_stmt *s, int recall);
int rx_cmd_pack(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_zap(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_locate(rx_ctx *ctx, rx_stmt *s, int cont);
int rx_cmd_copy(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_create(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_flush(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_type(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_unlock(rx_ctx *ctx, rx_stmt *s);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_LANG_H */
