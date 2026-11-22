/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file log.c
 * @brief RX_LOG trace facility: RX_LOG=debug|info|warn|error selects level.
 *        Silent by default; never used on rendering hot paths.
 */
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int log_level = 0; /* 0 = off */
static int log_inited = 0;

void rx_log_init(void)
{
    if (log_inited) {
        return;
    }
    log_inited = 1;
    const char *v = getenv("RX_LOG");
    if (!v) {
        return;
    }
    if (strcmp(v, "debug") == 0) {
        log_level = RX_LOG_DEBUG;
    } else if (strcmp(v, "info") == 0) {
        log_level = RX_LOG_INFO;
    } else if (strcmp(v, "warn") == 0) {
        log_level = RX_LOG_WARN;
    } else if (strcmp(v, "error") == 0) {
        log_level = RX_LOG_ERROR;
    }
}

void rx_fatal(const char *what, const char *file, int line)
{
    FILE *f = fopen("reynard-crash.log", "w");
    if (f) {
        fprintf(f, "reynard fatal: %s at %s:%d\n", what, file, line);
        fclose(f);
    }
    fprintf(stderr, "reynard: internal fatal: %s (%s:%d)\n", what, file, line);
    abort();
}

void rx_log(int level, const char *fmt, ...)
{
    if (!log_inited) {
        rx_log_init();
    }
    if (log_level == 0 || level > log_level) {
        return;
    }
    static const char *names[] = { "", "ERROR", "WARN", "INFO", "DEBUG" };
    fprintf(stderr, "[reynard %s] ", names[level & 7]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
