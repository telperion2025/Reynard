/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file errhand.c
 * @brief Structured errors and the §6.6 diagnostic format:
 *        "Error in line N col C of FILE: <text>" with a clang-style caret
 *        under the offending token. Batch prints to stderr; interactive
 *        routes to the message line (M3+).
 */
#include "lang/lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util/util.h"

int rx_raise(rx_ctx *ctx, int code, int line, int col, const char *fmt, ...)
{
    r_err *e = &ctx->err;
    e->code = code;
    e->line = line;
    e->col = col;
    snprintf(e->src_file, sizeof(e->src_file), "%s", ctx->prog);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);

    ctx->err_printed = 0;
    /* M5: when an ON ERROR handler is registered (and not itself running),
       the diagnostic is suppressed — exec_list dispatches to the handler */
    if (ctx->on_error[0] && !ctx->in_on_error && !ctx->quit) {
        ctx->err_printed = 1;
        return code;
    }

    /* interactive mode (M3): route the first line to the UI message line */
    if (ctx->msg_line && !ctx->out) {
        char buf[1200];
        if (ctx->src && line > 0) {
            const char *p = ctx->src;
            int cur = 1;
            while (*p && cur < line) {
                if (*p == '\n') {
                    cur++;
                }
                p++;
            }
            const char *eol = p;
            while (*eol && *eol != '\n') {
                eol++;
            }
            size_t n = (size_t)(eol - p);
            char *srcline = malloc(n + 1);
            if (!srcline) {
                RX_FATAL("out of memory");
            }
            memcpy(srcline, p, n);
            srcline[n] = '\0';
            rx_format_error(e, srcline, buf, sizeof(buf));
            free(srcline);
        } else {
            rx_format_error(e, NULL, buf, sizeof(buf));
        }
        if (!ctx->err_printed) {
            char first[1200];
            snprintf(first, sizeof(first), "%s", buf);
            char *nl = strchr(first, '\n');
            if (nl) {
                *nl = '\0';
            }
            ctx->msg_line(ctx->msg_ud, first);
            ctx->err_printed = 1;
        }
        return code;
    }

    /* batch mode: stderr with caret */
    char buf[1200];
    if (ctx->src && line > 0) {
        /* extract source line `line` from ctx->src */
        const char *p = ctx->src;
        int cur = 1;
        while (*p && cur < line) {
            if (*p == '\n') {
                cur++;
            }
            p++;
        }
        const char *eol = p;
        while (*eol && *eol != '\n') {
            eol++;
        }
        size_t n = (size_t)(eol - p);
        char *srcline = malloc(n + 1);
        if (!srcline) {
            RX_FATAL("out of memory");
        }
        memcpy(srcline, p, n);
        srcline[n] = '\0';
        rx_format_error(e, srcline, buf, sizeof(buf));
        free(srcline);
    } else {
        rx_format_error(e, NULL, buf, sizeof(buf));
    }
    if (!ctx->err_printed) {
        fprintf(stderr, "%s\n", buf);
        ctx->err_printed = 1;
    }
    return code;
}

void rx_format_error(const r_err *err, const char *srcline, char *out, size_t outsz)
{
    const char *file = err->src_file[0] ? err->src_file : "<command>";
    if (err->line > 0) {
        snprintf(out, outsz, "Error in line %d col %d of %s: %s", err->line, err->col, file,
                 err->msg);
    } else {
        snprintf(out, outsz, "Error in %s: %s", file, err->msg);
    }
    if (srcline && err->line > 0 && err->col > 0) {
        size_t used = strlen(out);
        snprintf(out + used, outsz - used, "\n%s\n", srcline);
        used = strlen(out);
        int caret = err->col - 1;
        if (caret > 200) {
            caret = 200;
        }
        if (used + (size_t)caret + 4 < outsz) {
            memset(out + used, ' ', (size_t)caret);
            out[used + caret] = '^';
            out[used + caret + 1] = '\0';
        }
    }
}
