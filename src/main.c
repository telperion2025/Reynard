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
#include <unistd.h>

#include "lang/lang.h"
#include "reynard/dbf.h"
#include "term/term_if.h"
#include "ui/ui.h"
#include "util/util.h"

extern const char rx_license_text[];

#define RX_VERSION "0.1.0"

static void usage(FILE* f) {
    fprintf(f, "Usage: reynard [options] [script.prg]\n"
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

static char* read_all(FILE* f, size_t* outn) {
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

int main(int argc, char** argv) {
    rx_log_init();
    const char* script = NULL;
    const char* carg = NULL;
    const char* ofile = NULL;
    const char* cfgfile = NULL;
    const char* runapp = NULL;
    int test_mode = 0;
    int no_mouse = 0;
    const char* mouse_forced = NULL;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
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

    FILE* out = stdout;
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
    ctx.in = stdin; /* INPUT/ACCEPT/WAIT read here (M5) */
    ctx.set.mouse = !no_mouse;
    /* data-engine session: frozen header date in -t, default code page mark
       from REYNARD_CODEPAGE or the config file (spec §8.2) */
    {
        int cp = 0;
        const char* env = getenv("REYNARD_CODEPAGE");
        if (env && *env) {
            cp = (int)strtol(env, NULL, 10);
        } else if (cfg.codepage) {
            cp = cfg.codepage;
        }
        int mark = cp ? (int)rx_cp_to_mark(cp) : 0x01;
        rx_data_session(test_mode, mark);
    }

    int rc = 0;
    if (runapp) {
        /* M9 9.6: run a packaged application built by BUILD PROJECT */
        if (access(runapp, R_OK) != 0) {
            fprintf(stderr, "reynard: cannot open %s\n", runapp);
            rc = 2;
        } else if (rx_run_rpp(&ctx, runapp) != R_OK && !ctx.quit) {
            rc = 1;
        }
    } else if (carg) {
        /* -c: semicolons split commands (spec §6.4) */
        char* copy = rx_strdup(carg);
        char* save = NULL;
        for (char* part = strtok_r(copy, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
            if (rx_run_source(&ctx, part, "<command>") != R_OK && !ctx.quit) {
                rc = 1;
                break;
            }
            if (ctx.quit) {
                break;
            }
        }
        free(copy);
    } else if (script && strcmp(script, "-") != 0) {
        /* M5: .prg files run through rx_run_file with the .RXP compiled cache */
        if (access(script, R_OK) != 0) {
            fprintf(stderr, "reynard: cannot open %s\n", script);
            rc = 2;
        } else if (rx_run_file(&ctx, script) != R_OK && !ctx.quit) {
            rc = 1;
        }
    } else if (script && strcmp(script, "-") == 0) {
        size_t n;
        char* src = read_all(stdin, &n);
        if (rx_run_source(&ctx, src, "<stdin>") != R_OK && !ctx.quit) {
            rc = 1;
        }
        free(src);
    } else {
        /* interactive full-screen shell (spec §3.3, §6). The script backend
           is selected when RX_EVENTS is set (golden-screen tests); otherwise
           the production ncurses backend. */
        const term_backend* tb = getenv("RX_EVENTS") ? rx_term_script() : rx_term_ncurses();
        if (!tb) {
            fprintf(stderr,
                    "reynard: interactive UI needs a terminal backend (ncurses not available "
                    "in this build).\n"
                    "Use batch mode: reynard script.prg, reynard -c \"? 1+1\", or reynard -\n");
            rc = 2;
        } else {
            ctx.out = NULL; /* output routes through the UI hooks */
            int uirc = rx_ui_run(&ctx, tb);
            if (uirc == R_ERR_NOUI) {
                fprintf(stderr, "reynard: could not initialize the terminal\n");
                rc = 2;
            } else {
                rc = uirc;
            }
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
