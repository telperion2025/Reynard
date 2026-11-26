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
    OP_LB, OP_RB, OP_COLON, OP_SEMI, OP_Q, OP_AT,               /* ? @ */
    OP_AND_LOGICAL, OP_OR_LOGICAL, OP_NOT_LOGICAL               /* .AND. AND etc. */
};

typedef struct rx_tok {
    tk_kind kind;
    int line, col;
    size_t off;   /**< byte offset in the source (for TEXT...ENDTEXT spans) */
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
    ST_EMPTY,    /**< comment-only line */
    /* M5 control flow / program structure (§7.1) */
    ST_IF,       /**< exprs[0..ncond-1] branch conds, kids[0..ncond-1] bodies, kids[ncond] else */
    ST_WHILE,    /**< exprs[0] cond, kids[0] body */
    ST_FOR,      /**< target=var, exprs: start,end,step(optional), kids[0] body */
    ST_CASE,     /**< like ST_IF with OTHERWISE as final else */
    ST_SCAN,     /**< scope/FOR/WHILE in clauses, kids[0] body */
    ST_PROC,     /**< target=name, strs=param names, kids[0] body; FUNCTION sets is_func via clause */
    ST_TEXT,     /**< raw = literal text block (TEXT...ENDTEXT) */
    ST_DO,       /**< target=proc name, exprs=args, clauses WITH */
    ST_RETURN,   /**< exprs[0] optional value */
    ST_EXIT,     /**< EXIT loop */
    ST_LOOP,     /**< LOOP to next iteration */
    ST_RETRY,    /**< RETRY inside ON ERROR handler */
    ST_PARAMETERS, /**< strs = param names (LPARAMETERS: clause LPAR) */
    ST_PRIVATE,  /**< strs = names, or clause ALL LIKE/EXCEPT pattern */
    ST_PUBLIC,   /**< strs = names */
    ST_RELEASE,  /**< strs = names, or MEMVAR ALL LIKE/EXCEPT, or ALL */
    ST_ON,       /**< clause key ERROR/ESCAPE/KEY LABEL, word = handler */
    ST_WAIT,     /**< strs[0] optional message */
    ST_INPUT,    /**< strs[0] prompt, target = var */
    ST_ACCEPT,   /**< strs[0] prompt, target = var */
    ST_CANCEL,   /**< CANCEL: abort program deterministically */
    ST_SUSPEND,  /**< SUSPEND (interactive; batch: no-op + message) */
    ST_RESUME,   /**< RESUME */
    ST_DIMENSION,/**< kids: one per array (target=name, exprs=dims) */
    /* M6 §8.4 index statements */
    ST_INDEX,    /**< INDEX ON e TO file | TAG t [OF cdx] [FOR e] [UNIQUE] [DESC] [ADDITIVE] */
    ST_SEEK,     /**< SEEK e */
    ST_REINDEX,  /**< REINDEX */
    /* M7 §7.1 aggregates / restructuring */
    ST_COUNT,    /**< COUNT [scope] [FOR/WHILE] [TO var] */
    ST_SUM,      /**< SUM [scope] [exprs] [FOR/WHILE] TO var|ARRAY a */
    ST_AVERAGE,  /**< AVERAGE ... TO var|ARRAY a */
    ST_CALCULATE,/**< CALCULATE [scope] CNT()|SUM(e)|AVG(e)|MIN(e)|MAX(e),... TO var|ARRAY */
    ST_SORT,     /**< SORT TO f ON fld [/A|/D|/C],... [scope] [FOR/WHILE] [FIELDS..] */
    ST_TOTAL,    /**< TOTAL TO f ON e [FIELDS..] [scope] [FOR/WHILE] */
    ST_SCATTER,  /**< SCATTER [FIELDS..] [MEMO] TO ARRAY a | MEMVAR [BLANK] */
    ST_GATHER,   /**< GATHER FROM ARRAY a | MEMVAR [FIELDS..] [MEMO] */
    /* M7 §9.2 forms: @ SAY/GET, READ */
    ST_SAYGET,   /**< @ r,c [SAY e [PICTURE..]] [GET var ...] | @ r,c CLEAR | @ r,c BOX ... */
    ST_READ,     /**< READ [CYCLE] | CLEAR GETS | CLEAR READ [ALL] */
    /* M7 \u00a79.2 screen builder: CREATE/MODIFY/GENERATE SCREEN (strs[0]=name, strs[1]=TO path) */
    ST_CREATE_SCREEN,
    ST_MODIFY_SCREEN,
    ST_GENERATE_SCREEN,
    /* M8 9.3/9.4 report/label: REPORT FORM / LABEL FORM, CREATE/MODIFY REPORT/LABEL */
    ST_REPORT_FORM,
    ST_LABEL_FORM,
    ST_CREATE_REPORT,
    ST_MODIFY_REPORT,
    ST_CREATE_LABEL,
    ST_MODIFY_LABEL,
    ST_ENDMARK   /**< stray ENDIF/ENDDO/... at top level: syntax error */
} st_kind;

typedef struct rx_clause {
    char key[24];    /**< e.g. SCOPE, IN, ALIAS, TO, IDXFILE */
    char word[280];  /**< keyword value or path when applicable */
    rx_expr *e;      /**< expression value when applicable */
    char *text;      /**< owned raw source text of e (M6, may be NULL) */
} rx_clause;

/** One BROWSE FIELDS column spec (spec §9.1, M4). For plain fields is_field
 *  is set and name holds the uppercased field name; otherwise e is a
 *  computed display column evaluated per visible row. Ownership of e and
 *  valid can be stolen by the UI (browse) via rx_stmt_steal_bcols. */
typedef struct rx_bcol {
    char name[128];   /**< field name (uppercased) when is_field */
    rx_expr *e;       /**< computed column expression (NULL for plain fields) */
    char header[64];  /**< :H = "title" */
    int width;        /**< :W = n, 0 = auto */
    rx_expr *valid;   /**< :V = expr (field validation on cell exit) */
    char verr[160];   /**< :E = "message" for :V failure */
    int rflag;        /**< :R read-only */
    int fflag;        /**< :F */
    int frozen;       /**< set when FREEZE names this column */
    int is_field;
    size_t fldidx;    /**< resolved against the table at BROWSE open */
} rx_bcol;

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
    rx_bcol *bcols;   /**< ST_BROWSE FIELDS specs */
    size_t nbcols, capbcols;
    /* M6: raw source text of statement expressions, kept for index tag
       headers (KEY()), SET FILTER text (FILTER()) and macro re-expansion */
    char *keytext;    /**< ST_INDEX key expression text (owned) */
    char *auxtext;    /**< ST_INDEX FOR text / ST_SET FILTER text (owned) */
    /* M5 block structure: kids = child statements (branch bodies / loop body
       / procedure body); ncond = number of leading branch conditions in
       exprs for ST_IF/ST_CASE; nidx = array index exprs for ST_ASSIGN;
       raw = TEXT block content */
    struct rx_stmt **kids;
    size_t nkids, capkids;
    size_t ncond;
    size_t nidx;
    char *raw;
} rx_stmt;

/** Steal the BROWSE column specs out of a stmt (sets s->bcols = NULL), so a
 *  long-lived UI object can own the expressions after the stmt is freed. */
void rx_stmt_steal_bcols(rx_stmt *s, rx_bcol **cols, size_t *ncols);
/** Append a clause (public wrapper for command handlers synthesizing stmts). */
void rx_stmt_add_clause(rx_stmt *s, const char *key, const char *word, rx_expr *e);

/** Parse a token stream into statements (one per logical line). */
int rx_parse(rx_tok *toks, size_t ntok, rx_stmt **out, size_t *nout, r_err *err);
/** Like rx_parse, but keeps the source text so TEXT...ENDTEXT blocks are
 *  captured verbatim (M5). */
int rx_parse_src(rx_tok *toks, size_t ntok, const char *src, rx_stmt **out, size_t *nout,
                 r_err *err);
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
    BOP_FUNC, BOP_ARGC, /* BOP_ARGC follows BOP_FUNC with the arg count */
    BOP_ARRAY_LOAD,     /* M5: name str, then argc index values on stack */
    BOP_ARGNAME         /* M5: like BOP_VAR, but an array memvar pushes its name */
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
typedef struct rx_dbf rx_dbf; /**< data engine handle (reynard/dbf.h) */

typedef struct rx_memvar {
    char name[64]; /**< uppercased */
    rval val;      /**< owned */
    /* M5 arrays: when is_array, aelem[0..anelem) hold the elements row-major */
    int is_array;
    size_t adim[2]; /**< adim[1]==0 for 1-D */
    rval *aelem;
    size_t anelem;
} rx_memvar;

/** A user procedure/function definition (M5): PROCEDURE/FUNCTION ... body. */
typedef struct rx_proc {
    char name[64];
    char params[16][64];
    size_t nparams;
    struct rx_stmt **body; /**< owned array of owned body stmts */
    size_t nbody;
    int is_func;
} rx_proc;

/** A procedure call frame: its own memvar scope (PRIVATE hiding, §7.5). */
typedef struct rx_frame {
    rx_memvar *vars;
    size_t nvars, capvars;
    struct rx_frame *up;
} rx_frame;

/* loop/return control */
#define RX_CTL_NONE 0
#define RX_CTL_LOOP 1
#define RX_CTL_EXIT 2
#define RX_CTL_RETRY 3

rx_memvar *rx_memvar_find(struct rx_ctx *ctx, const char *name);
int rx_memvar_set(struct rx_ctx *ctx, const char *name, const rval *v);
/** Deep copy of an rval (strings/memos duplicated). */
void rx_rval_deep(rval *dst, const rval *src);
/** M5 scoping: PUBLIC writes the global table; PRIVATE declares in the
 *  current frame (hiding outer scopes); RELEASE removes from any scope. */
int rx_memvar_set_public(struct rx_ctx *ctx, const char *name, const rval *v);
int rx_memvar_private(struct rx_ctx *ctx, const char *name);
int rx_memvar_release(struct rx_ctx *ctx, const char *name);
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
    int unique;     /**< SET UNIQUE, default OFF (M6: UNIQUE index builds) */
    char defdir[260];
    char path[1024];
} rx_set;

/* ============================ M7 §9.2 GET objects ============================ */

/** One pending @ ... GET object (spec §9.2). Values are owned. */
typedef struct rx_get {
    char var[64];       /**< memvar or field name (uppercased) */
    int is_field;       /**< var names a field of the current work area */
    rval val;           /**< current value (owned) */
    int row, col;       /**< GET field position on the desktop */
    int size_h, size_w; /**< SIZE h,w (display width = size_w) */
    char *say;          /**< SAY label text (owned, may be NULL) */
    int say_row, say_col;
    char *picture;      /**< PICTURE template (owned) */
    char *function;     /**< FUNCTION codes (owned) */
    rx_code when, valid;/**< compiled WHEN/VALID */
    int has_when, has_valid;
    rval range_lo, range_hi; /**< RANGE bounds (owned) */
    int has_range;
    rval deflt;         /**< DEFAULT value (owned) */
    int has_default;
    int enabled;        /**< WHEN() currently true */
} rx_get;

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
    /* M4: interactive BROWSE hook + SET RELATION / SET SKIP state */
    int (*browse_fn)(void *ud, struct rx_stmt *s); /**< NULL in batch -> NOUI 601 */
    void *browse_ud;
    struct rx_rel *rels; /**< SET RELATION list (driver -> target) */
    size_t nrels, caprels;
    int skip_to[256];    /**< SET SKIP TO alias areas (1-based) */
    /* M6 §8.4: index key expressions seen this session, for REINDEX and
       REPLACE/APPEND maintenance (data layer stores only the text) */
    struct rx_idxexpr *idxexprs;
    size_t nidxexprs, capidxexprs;
    char *filter_text;   /**< SET FILTER expression text for FILTER() */
    /* M5: frames, procedures, error trapping, loop control, macros, .RXP */
    rx_frame *frame;     /**< current procedure frame, NULL at top level */
    rx_proc *procs;      /**< user procedures/functions defined in the program */
    size_t nprocs, capprocs;
    char on_error[128];  /**< ON ERROR handler proc name ("" = none) */
    int in_on_error;     /**< handler currently running */
    int retry;           /**< RETRY seen in handler: re-run the erroring stmt */
    int loop_ctl;        /**< RX_CTL_* set by LOOP/EXIT */
    int returning;       /**< RETURN seen: unwind current body */
    rval retval;         /**< RETURN expr value (owned) */
    FILE *in;            /**< input stream for INPUT/ACCEPT/WAIT (default stdin) */
    /* M7 §9.2: pending GETs + UI hooks (NULL in batch -> NOUI 601) */
    rx_get *gets;
    size_t ngets, capgets;
    int read_active;     /**< inside a READ loop (RDLEVEL) */
    int read_cycle;      /**< READ CYCLE wraps at the ends */
    int read_clear;      /**< CLEAR READ requested: leave the loop */
    int read_abort;      /**< Esc pressed: terminate READ (READKEY 27) */
    int cur_obj;         /**< OBJNUM() of the current GET (1-based) */
    int (*read_fn)(void *ud);          /**< run the modal READ loop (ui) */
    void *read_ud;
    int (*say_fn)(void *ud, int row, int col, const char *text); /**< paint @ SAY (ui) */
    void *say_ud;
    int (*screen_fn)(void *ud, struct rx_stmt *s); /**< CREATE/MODIFY SCREEN builder */
    void *screen_ud;
    /* M8 9.3/9.4: report/label designer + preview hooks (NULL in batch) */
    int (*rptb_fn)(void *ud, struct rx_stmt *s);
    void *rptb_ud;
    int (*preview_fn)(void *ud, const char *title, const char *text); /**< report preview window */
    void *preview_ud;
};

/** One SET RELATION (spec §8.1, M4): moving the driver work area
 *  repositions target by evaluating e in the driver. Without an index
 *  the key must be numeric and positions the target by record number;
 *  with an active order on the target the key is SEEKed (M6). */
typedef struct rx_rel {
    int driver;   /**< work area whose movement drives the relation */
    rx_expr *e;   /**< key expression, evaluated with driver selected */
    char *text;   /**< owned raw source text of e, for RELATION() (M6) */
    int target;   /**< INTO alias work area */
} rx_rel;

/** Index key expression registry entry (M6): one per (table, file, tag).
 *  Text is authoritative (stored in CDX headers); the parsed ASTs are
 *  compiled lazily once and reused for REINDEX / record maintenance. */
typedef struct rx_idxexpr {
    char dbf[260];   /**< owning table path */
    char path[264];  /**< index file path (structural CDX = dbf stem + .cdx) */
    char tag[12];    /**< tag name, uppercased */
    char *keytext;   /**< owned key expression source */
    char *fortext;   /**< owned FOR filter source, NULL when none */
    rx_expr *key_e;  /**< lazily parsed keytext (owned) */
    rx_expr *for_e;  /**< lazily parsed fortext (owned) */
    int parsed;      /**< key_e/for_e compiled */
} rx_idxexpr;


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
/** Execute a block (branch/loop body: array of stmt pointers) — SCAN. */
int rx_exec_block(rx_ctx *ctx, rx_stmt **list, size_t n);

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
/* M5 */
int rx_cmd_do(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_return(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_scan(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_on(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_wait(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_input(rx_ctx *ctx, rx_stmt *s, int eval);
int rx_cmd_dimension(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_private_public(rx_ctx *ctx, rx_stmt *s, int pub);
int rx_cmd_release(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_parameters(rx_ctx *ctx, rx_stmt *s, int lpar);

/* ---- M6 §8.4 index commands (cmds_index.c) ---- */
int rx_cmd_index_on(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_seek(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_reindex(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_set_index(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_set_order(rx_ctx *ctx, rx_stmt *s);
/** Parse one expression from a source string (used to re-compile index key
 *  expressions stored as text in CDX tag headers). */
int rx_parse_expr_str(const char *src, rx_expr **out, r_err *err);
/** Locate tag (path, tag) among the attached files; returns R_OK + fi/ti. */
int rx_index_find_tag(rx_dbf *dbf, const char *path, const char *tag, int *fi, int *ti);
/** Nonzero when a controlling index order is active on dbf. */
int rx_index_order_active(const rx_dbf *dbf);
/** Clear + rebuild one tag by scanning all records (REINDEX, PACK). */
int rx_index_rebuild_tag(rx_ctx *ctx, rx_dbf *dbf, int fi, int ti);
/** Rebuild every tag of every attached file. */
int rx_index_rebuild_all(rx_ctx *ctx, rx_dbf *dbf);
/** Record callback for rx_iterate (scope iteration honoring DELETED/FILTER/FOR/WHILE). */
typedef int (*rx_rec_cb)(rx_ctx *ctx, rx_dbf *dbf, uint32_t recno, void *ud);
/** Iterate the statement's scope over dbf, calling cb per record. */
int rx_iterate(rx_ctx *ctx, rx_stmt *s, rx_dbf *dbf, int default_kind, rx_rec_cb cb, void *ud,
               int *count);
/** The scope implied by a statement's clauses with a default kind (0/1/2/3/4). */
int rx_default_scope(rx_stmt *s, int dflt);

/* M7 §7.1 aggregate / restructuring commands */
int rx_cmd_count(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_sum(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_average(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_calculate(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_sort(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_total(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_scatter(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_gather(rx_ctx *ctx, rx_stmt *s);

/* M7 §9.2 @ SAY/GET + READ */
int rx_cmd_sayget(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_read(rx_ctx *ctx, rx_stmt *s);
int rx_cmd_screen(rx_ctx *ctx, rx_stmt *s); /**< CREATE/MODIFY/GENERATE SCREEN (M7 \u00a79.2) */
int rx_cmd_report(rx_ctx *ctx, rx_stmt *s, int is_label); /**< REPORT/LABEL FORM (M8 9.3/9.4) */
int rx_cmd_rptb(rx_ctx *ctx, rx_stmt *s); /**< CREATE/MODIFY REPORT/LABEL designer (M8) */
void rx_form_free_ctx(rx_ctx *ctx);
int rx_form_store(rx_ctx *ctx, rx_get *g);

struct strbuf; /* util/util.h */
/* picture.c (M7 §9.2 PICTURE/FUNCTION codes) */
int rx_picture_render(rx_ctx *ctx, const rval *v, const char *pic, const char *func, struct strbuf *out);
int rx_picture_input_ok(const char *pic, const char *func, const char *input);

/** Re-insert one record into every tag (REPLACE / APPEND maintenance);
 *  the work area of dbf must be selected and the pointer at recno. */
int rx_index_maintain_rec(rx_ctx *ctx, rx_dbf *dbf, uint32_t recno);
/** Drop registry entries for a table path (called on USE/CLOSE). */
void rx_index_registry_purge(rx_ctx *ctx, const char *dbf_path);
/** Ordered navigation (controlling index order): GO TOP/BOTTOM, SKIP. */
int rx_nav_top(rx_ctx *ctx);
int rx_nav_bottom(rx_ctx *ctx);
int rx_nav_skip_ordered(rx_ctx *ctx, int32_t delta);
/** First tag overall (structural CDX first) becomes controlling; natural when none. */
void rx_index_default_order(rx_dbf *dbf);
/** 1-based tag number across all attached files -> (fi, ti). */
int rx_index_order_nth(rx_dbf *dbf, uint32_t n, int *fi, int *ti);
/** Position at any logical recno (nrec+1 = EOF) without raising. */
void rx_goto_logical(rx_dbf *dbf, uint32_t recno);
/** Resolve an index file name per SET DEFAULT / SET PATH (.idx/.cdx/.ntx). */
int resolve_index_m6(rx_ctx *ctx, const char *name, char *out, size_t outsz);

/* ---- M5: memvar scopes, arrays, procedures, files ---- */
/** Find a memvar walking the frame chain, then the global table. */
rx_memvar *rx_memvar_lookup(rx_ctx *ctx, const char *name);
/** Define an array memvar (DIMENSION a[d1] / a[d1,d2]); elements undefined. */
int rx_array_def(rx_ctx *ctx, const char *name, size_t d1, size_t d2);
/** Array element access; 1-based indexes per FoxPro AELEMENT. */
int rx_array_get(rx_ctx *ctx, const char *name, size_t i, size_t j, rval *out);
int rx_array_set(rx_ctx *ctx, const char *name, size_t i, size_t j, const rval *v);
/** Array dims of a memvar; 0,0 when not an array. */
int rx_array_dims(rx_ctx *ctx, const char *name, size_t *d1, size_t *d2);
/** Call a user procedure (DO name / UDF call); out receives RETURN value. */
int rx_call_proc(rx_ctx *ctx, const char *name, size_t argc, const rval *argv, rval *out,
                 r_err *err);
/** Run a .prg file with the .RXP compiled cache (magic RXP1, sha256). */
int rx_run_file(rx_ctx *ctx, const char *path);
/** Macro-expand &var in src (depth <= 16); returns owned string. */
char *rx_macro_expand(rx_ctx *ctx, const char *src, r_err *err);

/* ---- M4: navigation with relation follow + relations ---- */
/** GO TO recno in the current work area, then reposition related parents. */
int rx_nav_goto(rx_ctx *ctx, uint32_t recno);
/** SKIP delta in the current work area, then reposition related parents
 *  (SET SKIP TO targets are not repositioned on skips). */
int rx_nav_skip(rx_ctx *ctx, int32_t delta);
/** Reposition every relation whose driver is `driver`; via_skip honors
 *  SET SKIP TO. Evaluates each key expression with the driver selected. */
void rx_rel_apply(rx_ctx *ctx, int driver, int via_skip);
/** Work area of an alias, -1 if none. */
int rx_rel_alias_area(const char *alias);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_LANG_H */
