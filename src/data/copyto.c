/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file copyto.c
 * @brief SDF / DELIMITED import-export serializers (spec \u00a78.8). They work
 *        on raw record bytes, so files land in the DBF's own code page with
 *        no re-conversion. Layout choices (documented in docs/formats.md):
 *        SDF = fixed-width fields + CRLF; DELIMITED = comma separator with
 *        double-quoted character fields (quotes doubled); WITH BLANK/TAB use
 *        space/tab separators; dates are YYYYMMDD, logicals T/F, memo and
 *        G/B/P fields are skipped.
 */
#include <stdlib.h>
#include <string.h>

#include "data/data.h"
#include "util/util.h"

static int is_memoish(uint8_t t) {
    return t == RFT_M || t == RFT_G || t == RFT_P || t == RFT_B;
}

static size_t line_len(const rx_dbf* dbf, const size_t* flds, size_t nf, int delim_mode,
                       int quote) {
    (void)0;
    size_t n = 2; /* CRLF */
    for (size_t i = 0; i < nf; i++) {
        const rx_fielddef* f = &dbf->fields[flds[i]];
        if (is_memoish(f->type)) {
            continue;
        }
        if (delim_mode) {
            if (i) {
                n += 1; /* separator */
            }
            if (f->type == RFT_C && quote) {
                n += 2; /* quotes */
            }
            n += f->len;
        } else {
            n += f->len;
        }
    }
    return n + 8;
}

int rx_sdf_write_line(const rx_dbf* dbf, const size_t* flds, size_t nf, const uint8_t* rec,
                      char* out, size_t outsz, size_t* outn) {
    size_t need = line_len(dbf, flds, nf, 0, 0);
    if (outsz < need) {
        return R_ERR_RANGE;
    }
    size_t p = 0;
    for (size_t i = 0; i < nf; i++) {
        const rx_fielddef* f = &dbf->fields[flds[i]];
        if (is_memoish(f->type)) {
            continue;
        }
        memcpy(out + p, rec + dbf->foff[flds[i]], f->len);
        p += f->len;
    }
    out[p++] = '\r';
    out[p++] = '\n';
    *outn = p;
    return R_OK;
}

/* store a token into a raw field: C left-aligned padded, N/F right-aligned,
   D/L single-char or 8-char formats, overflow fills '*' */
static void store_token(const rx_dbf* dbf, size_t fi, const char* tok, size_t tn, uint8_t* raw) {
    const rx_fielddef* f = &dbf->fields[fi];
    uint8_t* dst = raw + dbf->foff[fi];
    memset(dst, ' ', f->len);
    /* trim surrounding spaces */
    while (tn > 0 && tok[0] == ' ') {
        tok++;
        tn--;
    }
    while (tn > 0 && tok[tn - 1] == ' ') {
        tn--;
    }
    if (tn == 0) {
        if (f->type == RFT_L) {
            dst[0] = '?';
        }
        if (f->type == RFT_D) {
            memset(dst, ' ', 8);
        }
        return;
    }
    switch (f->type) {
    case RFT_C:
        if (tn > f->len) {
            tn = f->len;
        }
        memcpy(dst, tok, tn);
        break;
    case RFT_N:
    case RFT_F:
        if (tn > f->len) {
            memset(dst, '*', f->len);
        } else {
            memcpy(dst + (f->len - tn), tok, tn);
        }
        break;
    case RFT_D:
        if (tn >= 8) {
            memcpy(dst, tok, 8);
        } else {
            memset(dst, '*', 8);
        }
        break;
    case RFT_L:
        switch (tok[0]) {
        case 'T':
        case 't':
        case 'Y':
        case 'y':
            dst[0] = 'T';
            break;
        case 'F':
        case 'f':
        case 'N':
        case 'n':
            dst[0] = 'F';
            break;
        default:
            dst[0] = '?';
            break;
        }
        break;
    default:
        if (tn > f->len) {
            tn = f->len;
        }
        memcpy(dst, tok, tn);
        break;
    }
}

int rx_sdf_parse_line(const rx_dbf* dbf, const size_t* flds, size_t nf, const char* line, size_t n,
                      uint8_t* rec) {
    size_t pos = 0;
    for (size_t i = 0; i < nf; i++) {
        size_t fi = flds[i];
        const rx_fielddef* f = &dbf->fields[fi];
        if (is_memoish(f->type)) {
            memset(rec + dbf->foff[fi], ' ', f->len);
            continue;
        }
        size_t take = f->len;
        if (pos + take > n) {
            take = n - pos;
        }
        uint8_t* dst = rec + dbf->foff[fi];
        memset(dst, ' ', f->len);
        if (take) {
            memcpy(dst, line + pos, take);
        }
        pos += f->len;
        if (pos > n) {
            pos = n;
        }
    }
    return R_OK;
}

int rx_delim_write_line(const rx_dbf* dbf, const size_t* flds, size_t nf, const uint8_t* rec,
                        char delim, int quote, char* out, size_t outsz, size_t* outn) {
    size_t need = line_len(dbf, flds, nf, 1, quote);
    (void)delim;
    if (outsz < need) {
        return R_ERR_RANGE;
    }
    size_t p = 0;
    int first = 1;
    for (size_t i = 0; i < nf; i++) {
        size_t fi = flds[i];
        const rx_fielddef* f = &dbf->fields[fi];
        if (is_memoish(f->type)) {
            continue;
        }
        if (!first) {
            out[p++] = delim;
        }
        first = 0;
        const uint8_t* raw = rec + dbf->foff[fi];
        if (f->type == RFT_C && quote) {
            out[p++] = '"';
            for (size_t k = 0; k < f->len; k++) {
                if (raw[k] == '"') {
                    out[p++] = '"'; /* double embedded quotes */
                }
                out[p++] = (char)raw[k];
            }
            out[p++] = '"';
        } else if (f->type == RFT_C && delim == ' ') {
            /* WITH BLANK: unquoted, trailing spaces trimmed */
            size_t e = f->len;
            while (e > 0 && raw[e - 1] == ' ') {
                e--;
            }
            memcpy(out + p, raw, e);
            p += e;
        } else if (f->type == RFT_N || f->type == RFT_F) {
            /* numeric fields export trimmed in DELIMITED (leading blanks off) */
            size_t st = 0;
            while (st < f->len && raw[st] == ' ') {
                st++;
            }
            memcpy(out + p, raw + st, f->len - st);
            p += f->len - st;
        } else {
            memcpy(out + p, raw, f->len);
            p += f->len;
        }
    }
    out[p++] = '\r';
    out[p++] = '\n';
    *outn = p;
    return R_OK;
}

int rx_delim_parse_line(const rx_dbf* dbf, const size_t* flds, size_t nf, const char* line,
                        size_t n, char delim, int quote, uint8_t* rec) {
    size_t pos = 0;
    for (size_t i = 0; i < nf; i++) {
        size_t fi = flds[i];
        const rx_fielddef* f = &dbf->fields[fi];
        /* extract the next token (heap copy only on the quoted path) */
        const char* tok = "";
        size_t tn = 0;
        char* owned = NULL;
        if (pos < n && quote && line[pos] == '"') {
            pos++;
            strbuf sb;
            sb_init(&sb);
            while (pos < n) {
                if (line[pos] == '"') {
                    if (pos + 1 < n && line[pos + 1] == '"') {
                        sb_putc(&sb, '"');
                        pos += 2;
                        continue;
                    }
                    pos++;
                    break;
                }
                sb_putc(&sb, line[pos++]);
            }
            if (!sb.buf) {
                sb_putc(&sb, '\0');
                sb.len = 0;
            }
            owned = sb.buf;
            tok = sb.buf;
            tn = sb.len;
            if (pos < n && line[pos] == delim) {
                pos++;
            }
        } else if (pos < n) {
            size_t start = pos;
            while (pos < n && line[pos] != delim) {
                pos++;
            }
            tok = line + start;
            tn = pos - start;
            if (pos < n) {
                pos++; /* separator */
            }
        }
        if (is_memoish(f->type)) {
            memset(rec + dbf->foff[fi], ' ', f->len); /* memo left blank */
        } else {
            store_token(dbf, fi, tok, tn, rec);
        }
        free(owned);
    }
    return R_OK;
}
