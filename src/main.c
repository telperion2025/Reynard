/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file main.c
 * @brief Argument parsing, mode dispatch, config load (spec §10). Modes:
 *        interactive (no args), batch (script.prg / -c / stdin '-'), test
 *        (-t, deterministic). Exit codes per §10.1.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lang/lang.h"
#include "reynard/dbf.h"
#include "term/term_if.h"
#include "util/util.h"

extern const char rx_license_text[];

#define RX_VERSION "0.1.0"

static void usage(FILE *f)
{
    fprintf(f,
            "Usage: reynard [options] [script.prg]\n"
            "  -c \"cmd; cmd\"   run commands then exit (batch)\n"
            "  -t               test mode: deterministic batch (DATE/TIME/RAND frozen)\n"
            "  -o <file>        batch stdout redirect\n"
            "  --run <app.rpp>  run a packaged application (M9)\n"
            "  --cfg <file>     alternate config file\n"
            "  --no-mouse       disable all mouse backends\n"
            "  --mouse=<m>      force mouse backend: auto|sgr|u1000|1002|1015|gpm|sysmouse\n"
            "  --utf8=on|off    force wide-char mode\n"
            "  --version, -V    print version\n"
            "  --help, -h       this help\n"
            "  --dump-license   print the 2-clause BSD license\n"
            "Exit codes: 0 success, 1 script/runtime error, 2 usage/environment,\n"
            "            3 lock conflict, 4 internal assertion (bug)\n");
}

static char *read_all(FILE *f, size_t *outn)
{
    strbuf sb;
    sb_init(&sb);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        sb_put(&sb, buf, n);
    }
    if (!sb.buf) {
        sb_putc(&sb, '\0');
        sb.len = 0;
    }
    *outn = sb.len;
    return sb.buf;
}

/* run one source buffer; returns exit-code contribution (0 or error code) */
static int run_source(rx_ctx *ctx, const char *src, const char *name)
{
    rx_tok *toks;
    size_t ntok;
    r_err err;
    memset(&err, 0, sizeof(err));
    snprintf(ctx->prog, sizeof(ctx->prog), "%s", name);
    ctx->src = src;
    if (rx_lex(src, name, &toks, &ntok, &err) != R_OK) {
        rx_raise(ctx, err.code, err.line, err.col, "%s", err.msg);
        return err.code;
    }
    rx_stmt *stmts;
    size_t nstmt;
    if (rx_parse(toks, ntok, &stmts, &nstmt, &err) != R_OK) {
        rx_raise(ctx, err.code, err.line, err.col, "%s", err.msg);
        rx_lex_free(toks, ntok);
        return err.code;
    }
    int rc = rx_exec(ctx, stmts, nstmt);
    for (size_t i = 0; i < nstmt; i++) {
        rx_stmt_free(&stmts[i]);
    }
    free(stmts);
    rx_lex_free(toks, ntok);
    if (rc != R_OK) {
        if (!ctx->err_printed) {
            char msg[512];
            snprintf(msg, sizeof(msg), "%s", ctx->err.msg[0] ? ctx->err.msg : rx_status_str(rc));
            rx_raise(ctx, rc, ctx->err.line ? ctx->err.line : err.line,
                     ctx->err.col ? ctx->err.col : err.col, "%s", msg);
        }
        return rc;
    }
    return R_OK;
}

int main(int argc, char **argv)
{
    rx_log_init();
    const char *script = NULL;
    const char *carg = NULL;
    const char *ofile = NULL;
    const char *cfgfile = NULL;
    const char *runapp = NULL;
    int test_mode = 0;
    int no_mouse = 0;
    const char *mouse_forced = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-c") == 0 && i + 1 < argc) {
            carg = argv[++i];
        } else if (strcmp(a, "-t") == 0) {
            test_mode = 1;
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            ofile = argv[++i];
        } else if (strcmp(a, "--run") == 0 && i + 1 < argc) {
            runapp = argv[++i];
        } else if (strcmp(a, "--cfg") == 0 && i + 1 < argc) {
            cfgfile = argv[++i];
        } else if (strcmp(a, "--no-mouse") == 0) {
            no_mouse = 1;
        } else if (strncmp(a, "--mouse=", 8) == 0) {
            mouse_forced = a + 8;
        } else if (strncmp(a, "--utf8=", 7) == 0) {
            /* wide-char mode flag; rendering honors it at M3 */
            (void)a;
        } else if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) {
            printf("reynard %s — the clever fox of terminals\n", RX_VERSION);
            return 0;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--dump-license") == 0) {
            fputs(rx_license_text, stdout);
            return 0;
        } else if (a[0] == '-' && a[1] != '\0' && strcmp(a, "-") != 0) {
            fprintf(stderr, "reynard: unknown option %s\n", a);
            usage(stderr);
            return 2;
        } else {
            script = a;
        }
    }

    /* config (spec §10.2) */
    rx_cfg cfg;
    rx_cfg_load(&cfg, cfgfile);
    for (size_t i = 0; i < cfg.n_warnings; i++) {
        fprintf(stderr, "reynard: config warning: %s\n", cfg.warnings[i]);
    }
    if (no_mouse || (mouse_forced && strcmp(mouse_forced, "off") == 0)) {
        setenv("REYNARD_NO_MOUSE", "1", 1);
    }

    if (runapp) {
        fprintf(stderr, "reynard: --run <app.rpp> lands at milestone M9 (project manager)\n");
        rx_cfg_free(&cfg);
        return 2;
    }

    FILE *out = stdout;
    if (ofile) {
        out = fopen(ofile, "w");
        if (!out) {
            fprintf(stderr, "reynard: cannot open %s for writing\n", ofile);
            rx_cfg_free(&cfg);
            return 2;
        }
    }

    rx_ctx ctx;
    rx_ctx_init(&ctx, out, test_mode);
    ctx.set.mouse = !no_mouse;
    /* data-engine session: frozen header date in -t, default code page mark
       from REYNARD_CODEPAGE or the config file (spec §8.2) */
    {
        int cp = 0;
        const char *env = getenv("REYNARD_CODEPAGE");
        if (env && *env) {
            cp = (int)strtol(env, NULL, 10);
        } else if (cfg.codepage) {
            cp = cfg.codepage;
        }
        int mark = cp ? (int)rx_cp_to_mark(cp) : 0x01;
        rx_data_session(test_mode, mark);
    }

    int rc = 0;
    if (carg) {
        /* -c: semicolons split commands (spec §6.4) */
        char *copy = rx_strdup(carg);
        char *save = NULL;
        for (char *part = strtok_r(copy, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
            if (run_source(&ctx, part, "<command>") != R_OK && !ctx.quit) {
                rc = 1;
                break;
            }
            if (ctx.quit) {
                break;
            }
        }
        free(copy);
    } else if (script && strcmp(script, "-") != 0) {
        FILE *f = fopen(script, "r");
        if (!f) {
            fprintf(stderr, "reynard: cannot open %s\n", script);
            rc = 2;
        } else {
            size_t n;
            char *src = read_all(f, &n);
            fclose(f);
            if (run_source(&ctx, src, script) != R_OK && !ctx.quit) {
                rc = 1;
            }
            free(src);
        }
    } else if (script && strcmp(script, "-") == 0) {
        size_t n;
        char *src = read_all(stdin, &n);
        if (run_source(&ctx, src, "<stdin>") != R_OK && !ctx.quit) {
            rc = 1;
        }
        free(src);
    } else {
        /* interactive (spec §3.3) */
        const term_backend *tb = rx_term_ncurses();
        if (!tb) {
            fprintf(stderr,
                    "reynard: interactive UI needs ncurses (not available in this build).\n"
                    "Use batch mode: reynard script.prg, reynard -c \"? 1+1\", or reynard -\n");
            rc = 2;
        } else {
            /* The full-screen UI lands at M3; M0 provides the term layer and
               the script backend self-test (tests/harness.c). */
            fprintf(stderr,
                    "reynard: interactive shell lands at milestone M3; "
                    "batch mode is available now (reynard --help)\n");
            rc = 2;
        }
    }

    if (ctx.err.code == R_ERR_LOCK_CONFLICT || ctx.err.code == R_ERR_FILE_IN_USE) {
        rc = 3;
    } else if (rc == 0 && ctx.err.code != R_OK && !ctx.quit) {
        rc = 1;
    }
    if (ctx.quit) {
        rc = 0;
    }

    for (int i = 1; i <= 255; i++) {
        rx_wa_close(i);
    }
    if (out != stdout) {
        fclose(out);
    }
    rx_ctx_free(&ctx);
    rx_cfg_free(&cfg);
    return rc;
}
