/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file field.c
 * @brief Decode one raw DBF field into an rval (spec §8.2 field rules).
 */
#include "data/data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/util.h"

static int decode_char(const rx_dbf* dbf, const uint8_t* raw, uint8_t len, rval* out) {
    char* utf8 = NULL;
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

int rx_field_decode(const rx_dbf* dbf, size_t fldidx, const uint8_t* raw, rval* out) {
    const rx_fielddef* f = &dbf->fields[fldidx];
    rx_rval_undef(out);
    switch (f->type) {
    case RFT_C:
        return decode_char(dbf, raw, f->len, out);
    case RFT_N:
    case RFT_F: {
        int blank;
        double d = rx_parse_num((const char*)raw, f->len, &blank);
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

/* ============================ encode (M2 write path) ============================ */

static double rval_num_or(const rval* v, int* ok) {
    switch (v->t) {
    case RX_T_N:
        *ok = 1;
        return v->u.n.d;
    case RX_T_I:
        *ok = 1;
        return v->u.i;
    case RX_T_Y:
        *ok = 1;
        return v->u.y / 10000.0;
    case RX_T_B:
        *ok = 1;
        return v->u.b;
    case RX_T_C: {
        int blank;
        double d = rx_parse_num(v->u.c.utf8 ? v->u.c.utf8 : "", v->u.c.n, &blank);
        *ok = !blank;
        return d;
    }
    default:
        *ok = 0;
        return 0;
    }
}

int rx_field_encode(rx_dbf* dbf, size_t fldidx, const rval* in, uint8_t* raw) {
    const rx_fielddef* f = &dbf->fields[fldidx];
    memset(raw, ' ', f->len);
    switch (f->type) {
    case RFT_C: {
        const char* s = "";
        size_t n = 0;
        if (in->t == RX_T_C) {
            s = in->u.c.utf8 ? in->u.c.utf8 : "";
            n = in->u.c.n;
        } else if (in->t == RX_T_M && in->u.memo.loaded) {
            s = in->u.memo.loaded->utf8 ? in->u.memo.loaded->utf8 : "";
            n = in->u.memo.loaded->n;
        } else if (in->t != RX_T_U) {
            return R_ERR_TYPE;
        }
        if (dbf->codepage > 0) {
            uint8_t* cp = malloc(n ? n : 1); /* UTF-8 -> single byte never expands */
            if (!cp) {
                RX_FATAL("out of memory");
            }
            size_t cn = 0;
            int rc = utf8_to_cp(dbf->codepage, s, n, cp, &cn);
            if (rc != R_OK) {
                free(cp);
                return rc;
            }
            if (cn > f->len) {
                cn = f->len; /* character fields truncate (FoxPro behavior) */
            }
            memcpy(raw, cp, cn);
            free(cp);
        } else {
            if (n > f->len) {
                n = f->len;
            }
            memcpy(raw, s, n);
        }
        return R_OK;
    }
    case RFT_N:
    case RFT_F: {
        int ok = 0;
        double d = rval_num_or(in, &ok);
        if (in->t == RX_T_U) {
            return R_OK; /* stays blank */
        }
        if (!ok) {
            return R_ERR_TYPE;
        }
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%.*f", f->dec, d);
        size_t nl = strlen(tmp);
        if (nl > f->len) {
            memset(raw, '*', f->len); /* numeric overflow: asterisks */
        } else {
            memcpy(raw + (f->len - nl), tmp, nl);
        }
        return R_OK;
    }
    case RFT_I: {
        int ok = 0;
        double d = rval_num_or(in, &ok);
        if (in->t == RX_T_U) {
            memset(raw, 0, 4);
            return R_OK;
        }
        if (!ok || f->len < 4) {
            return R_ERR_TYPE;
        }
        int32_t v = (int32_t)d;
        memcpy(raw, &v, 4);
        return R_OK;
    }
    case RFT_Y: {
        int ok = 0;
        double d = rval_num_or(in, &ok);
        if (in->t == RX_T_U) {
            memset(raw, 0, 8);
            return R_OK;
        }
        if (!ok || f->len < 8) {
            return R_ERR_TYPE;
        }
        int64_t v = (int64_t)(d * 10000.0 + (d < 0 ? -0.5 : 0.5));
        memcpy(raw, &v, 8);
        return R_OK;
    }
    case RFT_B: {
        int ok = 0;
        double d = rval_num_or(in, &ok);
        if (in->t == RX_T_U) {
            memset(raw, 0, 8);
            return R_OK;
        }
        if (!ok || f->len < 8) {
            return R_ERR_TYPE;
        }
        memcpy(raw, &d, 8);
        return R_OK;
    }
    case RFT_D: {
        if (in->t == RX_T_U) {
            return R_OK; /* 8 spaces = empty date */
        }
        if (in->t != RX_T_D) {
            return R_ERR_TYPE;
        }
        if (in->u.d.y == 0) {
            return R_OK;
        }
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%04d%02d%02d", (int)in->u.d.y, (int)in->u.d.m, (int)in->u.d.d);
        memcpy(raw, tmp, 8);
        return R_OK;
    }
    case RFT_T: {
        if (in->t == RX_T_U) {
            memset(raw, 0, 8);
            return R_OK;
        }
        if (in->t != RX_T_T || f->len < 8) {
            return R_ERR_TYPE;
        }
        long j = rx_julian(in->u.tm.d.y, in->u.tm.d.m, in->u.tm.d.d) + 2440588L - 1;
        int32_t jul = (int32_t)j;
        memcpy(raw, &jul, 4);
        memcpy(raw + 4, &in->u.tm.ms, 4);
        return R_OK;
    }
    case RFT_L: {
        if (in->t == RX_T_U) {
            raw[0] = '?';
            return R_OK;
        }
        if (in->t != RX_T_L) {
            return R_ERR_TYPE;
        }
        raw[0] = in->u.l == L_T ? 'T' : in->u.l == L_F ? 'F' : '?';
        return R_OK;
    }
    case RFT_M:
    case RFT_G:
    case RFT_P:
        /* M is handled by the caller via the memo sidecar; G/P read-only */
        return R_ERR_DBF_UNSUPPORTED;
    default:
        return R_ERR_DBF_FIELDS;
    }
}
