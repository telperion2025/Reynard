/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file codepage.c
 * @brief DBF language-driver byte -> code page mapping (spec §8.2).
 *        Unknown mark 0: no conversion, one-time warning (rx_log).
 */
#include "data/data.h"

#include "util/util.h"

int rx_mark_to_cp(uint8_t mark) {
    switch (mark) {
    case 0x00:
        return 0; /* no mark: no conversion */
    case 0x01:
        return RX_CP437;
    case 0x02:
        return RX_CP850;
    case 0x03:
    case 0x57:
    case 0xC8: /* Windows ANSI */
        return RX_CP1252;
    case 0x64:
        return RX_CP852; /* table pending; falls back with warning */
    case 0x65:
        return RX_CP866;
    case 0xC9:
        return RX_CP1252; /* close enough for Latin-1 text; see DEVIATIONS */
    default:
        return -1; /* unknown mark */
    }
}

int rx_mark_known_no_table(uint8_t mark) {
    return mark == 0x64; /* CP852 table not yet shipped */
}

uint8_t rx_cp_to_mark(int cp) {
    switch (cp) {
    case RX_CP437:
        return 0x01;
    case RX_CP850:
        return 0x02;
    case RX_CP1252:
        return 0x03;
    case RX_CP852:
        return 0x64;
    case RX_CP866:
        return 0x65;
    default:
        return 0; /* no conversion */
    }
}
