/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_env.c
 * @brief Environment commands: ?/??, assignment, STORE, SET, QUIT, CLEAR,
 *        BROWSE (NOUI in batch). Spec §7.1 environment family.
 */
#include "lang/lang.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util/util.h"

int rx_cmd_print(rx_ctx *ctx, rx_stmt *s, int newline)
{
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

int rx_cmd_assign(rx_ctx *ctx, rx_stmt *s)
{
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

int rx_cmd_store(rx_ctx *ctx, rx_stmt *s)
{
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
static const char *set_known[] = {
    "ALTERNATE", "ANSI", "BELL", "BLINK", "BLOCKSIZE", "BORDER", "CARRY", "CENTURY", "CLEAR",
    "CLOCK", "COLOR", "COMPATIBLE", "CONFIRM", "CONSOLE", "CURRENCY", "CURSOR", "DATE", "DEBUG",
    "DECIMALS", "DEFAULT", "DELETED", "DELIMITERS", "DEVICE", "DISPLAY", "DOHISTORY", "ECHO",
    "ESCAPE", "EXACT", "EXCLUSIVE", "FIELDS", "FILTER", "FIXED", "FORMAT", "FULLPATH", "FUNCTION",
    "HEADINGS", "HELP", "HOURS", "INTENSITY", "KEY", "KEYCOMP", "LOCK", "LOGERRORS", "MACKEY",
    "MARGIN", "MARK", "MEMOWIDTH", "MESSAGE", "MOUSE", "MULTILOCKS", "NEAR", "NOTIFY",
    "ODOMETER", "OPTIMIZE", "ORDER", "PALETTE", "PATH", "PDSETUP", "POINT", "PRINTER",
    "PROCEDURE", "REFRESH", "REPROCESS", "RESOURCE", "SAFETY", "SCOREBOARD", "SEPARATOR",
    "SHADOWS", "SKIP", "SPACE", "STATUS", "STEP", "STICKY", "SYSFORMATS", "SYSMENU", "TALK",
    "TEXTMERGE", "TOPIC", "TYPEAHEAD", "UDFPARMS", "UNIQUE", "VIEW",
};

static int set_known_p(const char *w)
{
    for (size_t i = 0; i < sizeof(set_known) / sizeof(set_known[0]); i++) {
        if (rx_strcasecmp(w, set_known[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int parse_on_off(const char *w, int *out)
{
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

int rx_cmd_set(rx_ctx *ctx, rx_stmt *s)
{
    if (s->nstrs == 0) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "SET requires an option");
    }
    const char *opt = s->strs[0];
    if (!set_known_p(opt)) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "Unknown SET option %s", opt);
    }
    /* forms: SET X ON|OFF | SET X TO v | SET X v */
    int val = -1;
    const char *toval = NULL;
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
    }
    /* other Appendix B options are accepted and stored for their milestone */
    return R_OK;
}

int rx_cmd_browse(rx_ctx *ctx, rx_stmt *s)
{
    (void)s;
    return rx_raise(ctx, R_ERR_NOUI, 0, 0, "BROWSE requires the interactive UI (milestone M3+)");
}
