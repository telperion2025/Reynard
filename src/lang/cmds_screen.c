/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_screen.c
 * @brief CREATE/MODIFY/GENERATE SCREEN (spec 9.2, M7 screen builder).
 *
 * CREATE/MODIFY SCREEN name opens the interactive designer through
 * ctx->screen_fn (NULL in batch: CREATE writes an empty .SCR+.RSO, MODIFY
 * recompiles the .RSO from the .SCR — the builder round-trip). GENERATE
 * SCREEN name [TO file.prg] emits .PRG code (@ SAY/GET + READ CYCLE) from
 * the .SCR (or .RSO) and works headless, so conformance tests can TYPE the
 * generated program.
 */
#include <stdio.h>
#include <string.h>

#include "lang/lang.h"
#include "util/scrfmt.h"
#include "util/util.h"

static void make_paths(const char *name, const char *to, char *scrpath, size_t s1, char *rsopath,
                       size_t s2, char *prgpath, size_t s3)
{
    snprintf(scrpath, s1, "%s.scr", name);
    snprintf(rsopath, s2, "%s.rso", name);
    if (to && *to) {
        snprintf(prgpath, s3, "%s", to);
    } else {
        snprintf(prgpath, s3, "%s.prg", name);
    }
}

int rx_cmd_screen(rx_ctx *ctx, rx_stmt *s)
{
    if (s->nstrs < 1 || !s->strs[0] || !s->strs[0][0]) {
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "Expected screen name");
    }
    const char *name = s->strs[0];
    const char *to = s->nstrs > 1 ? s->strs[1] : NULL;
    char scrpath[600], rsopath[600], prgpath[600];
    make_paths(name, to, scrpath, sizeof(scrpath), rsopath, sizeof(rsopath), prgpath, sizeof(prgpath));

    switch (s->kind) {
    case ST_CREATE_SCREEN:
        if (ctx->screen_fn) {
            return ctx->screen_fn(ctx->screen_ud, s);
        }
        /* batch: create an empty screen + compiled form */
        {
            scr_file sf;
            scrf_init(&sf, name);
            int rc = scrf_save(scrpath, &sf);
            if (rc == R_OK) {
                rc = scrf_compile(rsopath, &sf);
            }
            scrf_free(&sf);
            if (rc != R_OK) {
                return rx_raise(ctx, rc, s->line, s->col, "Cannot create screen");
            }
            if (ctx->set.talk) {
                rx_outf(ctx, "Screen %s created.\n", scrpath);
            }
            return R_OK;
        }
    case ST_MODIFY_SCREEN:
        if (ctx->screen_fn) {
            return ctx->screen_fn(ctx->screen_ud, s);
        }
        /* batch: recompile the .RSO from the .SCR (builder round-trip gate) */
        {
            scr_file sf;
            int rc = scrf_load(scrpath, &sf);
            if (rc != R_OK) {
                rc = scrf_load_rso(rsopath, &sf);
            }
            if (rc != R_OK) {
                return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "Screen file not found: %s",
                                name);
            }
            rc = scrf_compile(rsopath, &sf);
            scrf_free(&sf);
            if (rc != R_OK) {
                return rx_raise(ctx, rc, s->line, s->col, "Cannot write .RSO");
            }
            if (ctx->set.talk) {
                rx_outf(ctx, "Screen %s compiled to %s.\n", name, rsopath);
            }
            return R_OK;
        }
    case ST_GENERATE_SCREEN: {
        scr_file sf;
        int rc = scrf_load(scrpath, &sf);
        if (rc != R_OK) {
            rc = scrf_load_rso(rsopath, &sf);
        }
        if (rc != R_OK) {
            return rx_raise(ctx, R_ERR_IO_NOTFOUND, s->line, s->col, "Screen file not found: %s", name);
        }
        strbuf sb;
        sb_init(&sb);
        rc = scrf_generate(&sf, &sb);
        scrf_free(&sf);
        if (rc != R_OK) {
            sb_free(&sb);
            return rx_raise(ctx, rc, s->line, s->col, "Code generation failed");
        }
        FILE *f = fopen(prgpath, "w");
        if (!f) {
            sb_free(&sb);
            return rx_raise(ctx, R_ERR_IO_OPEN, s->line, s->col, "Cannot open %s", prgpath);
        }
        int wrc = fwrite(sb.buf ? sb.buf : "", 1, sb.len, f) == sb.len ? R_OK : R_ERR_IO_WRITE;
        fclose(f);
        sb_free(&sb);
        if (wrc != R_OK) {
            return rx_raise(ctx, wrc, s->line, s->col, "Cannot write %s", prgpath);
        }
        if (ctx->set.talk) {
            rx_outf(ctx, "Screen %s generated to %s.\n", name, prgpath);
        }
        return R_OK;
    }
    default:
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "Unknown screen command");
    }
}
