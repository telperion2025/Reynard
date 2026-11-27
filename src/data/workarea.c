/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file workarea.c
 * @brief The documented work-area table (spec §8.1): areas 1..255 plus
 *        "current" (0). Mirrors SELECT()/USED()/ALIAS().
 */
#include "data/data.h"

#include <string.h>

#include "util/util.h"

#define RX_MAX_WA 255

typedef struct wa_slot {
    rx_dbf* dbf;
    int used;
} wa_slot;

static wa_slot g_wa[RX_MAX_WA + 1]; /* index 1..255 */
static int g_current = 1;

int rx_wa_select(int area) {
    if (area < 0 || area > RX_MAX_WA) {
        return R_ERR_RANGE;
    }
    if (area != 0) {
        g_current = area;
    }
    return R_OK;
}

int rx_wa_current(void) {
    return g_current;
}

int rx_wa_used(int area) {
    if (area == 0) {
        area = g_current;
    }
    if (area < 1 || area > RX_MAX_WA) {
        return 0;
    }
    return g_wa[area].used;
}

int rx_wa_alias(int area, const char** out) {
    if (area == 0) {
        area = g_current;
    }
    if (area < 1 || area > RX_MAX_WA || !g_wa[area].used) {
        *out = "";
        return R_OK;
    }
    return rx_dbf_alias(g_wa[area].dbf, out);
}

int rx_wa_open(int area, const char* path, int mode, const char* alias) {
    if (area < 1 || area > RX_MAX_WA) {
        return R_ERR_RANGE;
    }
    rx_wa_close(area);
    rx_dbf* dbf = NULL;
    int rc = rx_dbf_open(path, mode, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    if (alias && *alias) {
        rx_dbf_set_alias(dbf, alias);
    }
    g_wa[area].dbf = dbf;
    g_wa[area].used = 1;
    g_current = area;
    return R_OK;
}

int rx_wa_close(int area) {
    if (area == 0) {
        area = g_current;
    }
    if (area < 1 || area > RX_MAX_WA) {
        return R_ERR_RANGE;
    }
    if (g_wa[area].used) {
        rx_dbf_close(g_wa[area].dbf);
        g_wa[area].dbf = NULL;
        g_wa[area].used = 0;
    }
    return R_OK;
}

int rx_wa_dbf(int area, rx_dbf** out) {
    if (area == 0) {
        area = g_current;
    }
    if (area < 1 || area > RX_MAX_WA || !g_wa[area].used) {
        return R_ERR_DBF_NOTOPEN;
    }
    *out = g_wa[area].dbf;
    return R_OK;
}
