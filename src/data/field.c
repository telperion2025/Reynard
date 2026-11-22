/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file field.c
 * @brief Decode one raw DBF field into an rval (spec §8.2 field rules).
 */
#include "data/data.h"

#include <stdlib.h>
#include <string.h>

#include "util/util.h"

static int decode_char(const rx_dbf *dbf, const uint8_t *raw, uint8_t len, rval *out)
{
    char *utf8 = NULL;
    size_t n = 0;
    int rc = R_OK;
    if (dbf->codepage > 0) {
        rc = cp_to_utf8(dbf->codepage, raw, len, &utf8, &n);
    } else {
        /* No code page mark: bytes pass through unconverted. */
        utf8 = malloc((size_t)len + 1);
        if (!utf8) {
            RX_FATAL("out of memory");
        }
        memcpy(utf8, raw, len);
        utf8[len] = '\0';
        n = len;
    }
    if (rc != R_OK) {
        return rc;
    }
    rval_set_str(out, utf8, n);
    return R_OK;
}

int rx_field_decode(const rx_dbf *dbf, size_t fldidx, const uint8_t *raw, rval *out)
{
    const rx_fielddef *f = &dbf->fields[fldidx];
    rx_rval_undef(out);
    switch (f->type) {
    case RFT_C:
        return decode_char(dbf, raw, f->len, out);
    case RFT_N:
    case RFT_F: {
        int blank;
        double d = rx_parse_num((const char *)raw, f->len, &blank);
        out->t = RX_T_N;
        out->u.n.d = d;
        out->u.n.dec = f->dec;
        return R_OK;
    }
    case RFT_I: {
        if (f->len < 4) {
            return R_ERR_DBF_FIELDS;
        }
        int32_t v;
        memcpy(&v, raw, 4);
        out->t = RX_T_I;
        out->u.i = v;
        return R_OK;
    }
    case RFT_Y: {
        if (f->len < 8) {
            return R_ERR_DBF_FIELDS;
        }
        int64_t v;
        memcpy(&v, raw, 8);
        out->t = RX_T_Y;
        out->u.y = v;
        return R_OK;
    }
    case RFT_B: {
        if (f->len < 8) {
            return R_ERR_DBF_FIELDS;
        }
        double d;
        memcpy(&d, raw, 8);
        out->t = RX_T_B;
        out->u.b = d;
        return R_OK;
    }
    case RFT_D: {
        char tmp[9];
        memcpy(tmp, raw, 8);
        tmp[8] = '\0';
        int blank = 1;
        for (int i = 0; i < 8; i++) {
            if (tmp[i] != ' ' && tmp[i] != '0') {
                blank = 0;
                break;
            }
        }
        out->t = RX_T_D;
        if (blank) {
            memset(&out->u.d, 0, sizeof(out->u.d)); /* empty date {} */
            return R_OK;
        }
        int y = (tmp[0] - '0') * 1000 + (tmp[1] - '0') * 100 + (tmp[2] - '0') * 10 + (tmp[3] - '0');
        int m = (tmp[4] - '0') * 10 + (tmp[5] - '0');
        int d = (tmp[6] - '0') * 10 + (tmp[7] - '0');
        if (y < 1 || m < 1 || m > 12 || d < 1 || d > 31) {
            return R_ERR_DATE_RANGE;
        }
        out->u.d.y = (int16_t)y;
        out->u.d.m = (int16_t)m;
        out->u.d.d = (int16_t)d;
        return R_OK;
    }
    case RFT_T: {
        if (f->len < 8) {
            return R_ERR_DBF_FIELDS;
        }
        int32_t julian, ms;
        memcpy(&julian, raw, 4);
        memcpy(&ms, raw + 4, 4);
        out->t = RX_T_T;
        out->u.tm.ms = ms;
        /* VFP julian day 0 = 4713 BC; convert to civil date. */
        long j = julian + 2440588L - 1; /* unix epoch alignment */
        long a = (j + 32044L);
        long b = (4 * a + 3) / 146097;
        long c = a - (146097 * b) / 4;
        long dd = (4 * c + 3) / 1461;
        long e = c - (1461 * dd) / 4;
        long m = (5 * e + 2) / 153;
        out->u.tm.d.d = (int16_t)(e - (153 * m + 2) / 5 + 1);
        out->u.tm.d.m = (int16_t)(m + 3 - 12 * (m / 10));
        out->u.tm.d.y = (int16_t)(100 * b + dd - 4800 + m / 10);
        return R_OK;
    }
    case RFT_L: {
        out->t = RX_T_L;
        switch (raw[0]) {
        case 'Y':
        case 'y':
        case 'T':
        case 't':
            out->u.l = L_T;
            break;
        case 'N':
        case 'n':
        case 'F':
        case 'f':
            out->u.l = L_F;
            break;
        default:
            out->u.l = L_UNDEF;
            break;
        }
        return R_OK;
    }
    case RFT_M:
    case RFT_G:
    case RFT_P: {
        /* Memo/general/picture: M1 reads the block number only; the memo
           payload load lands with FPT/DBT support at M2. */
        char tmp[11];
        memcpy(tmp, raw, 10);
        tmp[10] = '\0';
        uint32_t blk = (uint32_t)strtoul(tmp, NULL, 10);
        if (f->type == RFT_M) {
            out->t = RX_T_M;
            out->u.memo.blk = blk;
            out->u.memo.loaded = NULL;
        } else {
            out->t = RX_T_G;
            out->u.g.n = blk; /* block number stashed until G payloads land */
            out->u.g.bytes = NULL;
        }
        return R_OK;
    }
    default:
        return R_ERR_DBF_FIELDS;
    }
}
