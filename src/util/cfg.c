/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cfg.c
 * @brief reynard.cfg loading (spec §10.2). One statement per line; parse
 *        errors are collected as line-numbered warnings and never abort.
 */
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rx_cfg_default_path(char* out, size_t outsz) {
    const char* env = getenv("REYNARD_CFG");
    if (env && *env) {
        snprintf(out, outsz, "%s", env);
        return R_OK;
    }
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(out, outsz, "%s/reynard/reynard.cfg", xdg);
        return R_OK;
    }
    const char* home = getenv("HOME");
    if (!home || !*home) {
        return R_ERR_IO_NOTFOUND;
    }
    snprintf(out, outsz, "%s/.config/reynard/reynard.cfg", home);
    return R_OK;
}

static void add_line(char*** arr, size_t* n, const char* s) {
    char** na = realloc(*arr, (*n + 1) * sizeof(char*));
    if (!na) {
        RX_FATAL("out of memory");
    }
    *arr = na;
    (*arr)[*n] = rx_strdup(s);
    (*n)++;
}

static void warn(rx_cfg* cfg, int lineno, const char* text) {
    strbuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s:%d: %s", cfg->path, lineno, text);
    add_line(&cfg->warnings, &cfg->n_warnings, sb.buf ? sb.buf : "config warning");
    sb_free(&sb);
}

int rx_cfg_load(rx_cfg* cfg, const char* override_path) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->history = 1000;
    cfg->codepage = 0;
    cfg->utf8 = -1;
    snprintf(cfg->mouse, sizeof(cfg->mouse), "auto");

    char path[260];
    int rc;
    if (override_path) {
        snprintf(path, sizeof(path), "%s", override_path);
        rc = R_OK;
    } else {
        rc = rx_cfg_default_path(path, sizeof(path));
        if (rc != R_OK) {
            return R_OK; /* no config anywhere: defaults */
        }
    }
    snprintf(cfg->path, sizeof(cfg->path), "%s", path);

    FILE* f = fopen(path, "r");
    if (!f) {
        return R_OK; /* missing config file is not an error */
    }
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char* s = line;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        size_t n = strlen(s);
        while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
            s[--n] = '\0';
        }
        if (n == 0 || s[0] == '#') {
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq) {
            warn(cfg, lineno, "expected KEY = value");
            continue;
        }
        *eq = '\0';
        char* key = s;
        char* val = eq + 1;
        while (*key == ' ' || *key == '\t') {
            key++;
        }
        size_t kn = strlen(key);
        while (kn && (key[kn - 1] == ' ' || key[kn - 1] == '\t')) {
            key[--kn] = '\0';
        }
        while (*val == ' ' || *val == '\t') {
            val++;
        }
        if (rx_strcasecmp(key, "HISTORY") == 0) {
            long h = strtol(val, NULL, 10);
            if (h < 0 || h > 1000000) {
                warn(cfg, lineno, "HISTORY out of range, using 1000");
            } else {
                cfg->history = h;
            }
        } else if (rx_strcasecmp(key, "CODEPAGE_DEFAULT") == 0) {
            cfg->codepage = (int)strtol(val, NULL, 10);
        } else if (rx_strcasecmp(key, "MOUSE") == 0) {
            snprintf(cfg->mouse, sizeof(cfg->mouse), "%s", val);
        } else if (rx_strcasecmp(key, "UTF8") == 0) {
            cfg->utf8 = (rx_strcasecmp(val, "on") == 0) ? 1 : 0;
        } else {
            /* Any other statement (SET ..., COLOR SCHEME, F12 = ...) is
               replayed verbatim at startup by the language layer. */
            strbuf sb;
            sb_init(&sb);
            sb_printf(&sb, "%s = %s", key, val);
            add_line(&cfg->set_lines, &cfg->n_set, sb.buf ? sb.buf : s);
            sb_free(&sb);
        }
    }
    fclose(f);
    return R_OK;
}

void rx_cfg_free(rx_cfg* cfg) {
    for (size_t i = 0; i < cfg->n_set; i++) {
        free(cfg->set_lines[i]);
    }
    free(cfg->set_lines);
    for (size_t i = 0; i < cfg->n_warnings; i++) {
        free(cfg->warnings[i]);
    }
    free(cfg->warnings);
    memset(cfg, 0, sizeof(*cfg));
}
