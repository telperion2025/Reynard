/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file help.c
 * @brief Help DBF session (spec 9.7): SET HELP TO targets a custom DBF with
 *        columns TOPIC C, KEYWORDS M, BODY M, SEEALSO M; the ui help browser
 *        (ui/helpwin.c) drives this through a private read-only work area.
 *        Substring search over TOPIC+KEYWORDS is case-insensitive.
 */
#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <string.h>

#include "lang/lang.h"
#include "reynard/dbf.h"
#include "util/util.h"

static const char *rx_strcasestr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) {
        return hay;
    }
    for (const char *h = hay; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) {
            return h;
        }
    }
    return NULL;
}

static int fld_index(rx_dbf *d, const char *name)
{
    size_t nf = 0;
    if (rx_dbf_field_count(d, &nf) != R_OK) {
        return -1;
    }
    for (size_t i = 0; i < nf; i++) {
        rx_fielddef fd;
        if (rx_dbf_field_def(d, i, &fd) == R_OK && rx_strcasecmp(fd.name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int help_dbf(rx_ctx *ctx, rx_dbf **out)
{
    if (ctx->help_area < 0) {
        return R_ERR_DBF_NOTOPEN;
    }
    return rx_wa_dbf(ctx->help_area, out);
}

int rx_help_open(rx_ctx *ctx, const char *path)
{
    if (ctx->help_area >= 0) {
        rx_wa_close(ctx->help_area);
        ctx->help_area = -1;
    }
    int area = -1;
    for (int i = 1; i <= 255; i++) {
        if (!rx_wa_used(i)) {
            area = i;
            break;
        }
    }
    if (area < 0) {
        return R_ERR_FILE_IN_USE;
    }
    int rc = rx_wa_open(area, path, RD_READONLY | RD_SHARED, "RXHELP");
    if (rc != R_OK) {
        return rc;
    }
    rx_dbf *d;
    if (rx_wa_dbf(area, &d) != R_OK || fld_index(d, "TOPIC") < 0 || fld_index(d, "BODY") < 0) {
        rx_wa_close(area);
        return R_ERR_DBF_FIELDS;
    }
    ctx->help_area = area;
    return R_OK;
}

void rx_help_close(rx_ctx *ctx)
{
    if (ctx->help_area >= 0) {
        rx_wa_close(ctx->help_area);
        ctx->help_area = -1;
    }
}

size_t rx_help_count(rx_ctx *ctx)
{
    rx_dbf *d;
    if (help_dbf(ctx, &d) != R_OK) {
        return 0;
    }
    uint32_t n = 0;
    rx_dbf_recount(d, &n);
    return n;
}

static int help_goto(rx_ctx *ctx, size_t idx, rx_dbf **out)
{
    rx_dbf *d = NULL;
    int rc = help_dbf(ctx, &d);
    if (rc != R_OK) {
        return rc;
    }
    if (out) {
        *out = d;
    }
    return rx_dbf_goto(d, (uint32_t)idx + 1);
}

int rx_help_topic(rx_ctx *ctx, size_t idx, char *out, size_t outsz)
{
    rx_dbf *d;
    int rc = help_goto(ctx, idx, &d);
    if (rc != R_OK) {
        return rc;
    }
    int fi = fld_index(d, "TOPIC");
    rval v;
    rx_rval_undef(&v);
    rc = rx_dbf_get(d, (size_t)fi, &v);
    if (rc == R_OK) {
        if (v.t == RX_T_C) {
            snprintf(out, outsz, "%s", v.u.c.utf8 ? v.u.c.utf8 : "");
        } else {
            strbuf sb;
            sb_init(&sb);
            rx_vm_display(ctx, &v, &sb);
            snprintf(out, outsz, "%.*s", (int)outsz - 1, sb.buf ? sb.buf : "");
            sb_free(&sb);
        }
    }
    rx_rval_free(&v);
    return rc;
}

int rx_help_body(rx_ctx *ctx, size_t idx, struct strbuf *out)
{
    rx_dbf *d;
    int rc = help_goto(ctx, idx, &d);
    if (rc != R_OK) {
        return rc;
    }
    int fi = fld_index(d, "BODY");
    rval v;
    rx_rval_undef(&v);
    rc = rx_dbf_get(d, (size_t)fi, &v);
    if (rc == R_OK && v.t == RX_T_C && v.u.c.utf8) {
        sb_put(out, v.u.c.utf8, v.u.c.n);
    }
    rx_rval_free(&v);
    return rc;
}

int rx_help_seealso(rx_ctx *ctx, size_t idx, char *out, size_t outsz)
{
    rx_dbf *d;
    int rc = help_goto(ctx, idx, &d);
    if (rc != R_OK) {
        return rc;
    }
    int fi = fld_index(d, "SEEALSO");
    if (fi < 0) {
        out[0] = '\0';
        return R_OK;
    }
    rval v;
    rx_rval_undef(&v);
    rc = rx_dbf_get(d, (size_t)fi, &v);
    if (rc == R_OK && v.t == RX_T_C) {
        snprintf(out, outsz, "%s", v.u.c.utf8 ? v.u.c.utf8 : "");
    }
    rx_rval_free(&v);
    return rc;
}

int rx_help_keywords(rx_ctx *ctx, size_t idx, char *out, size_t outsz)
{
    rx_dbf *d;
    int rc = help_goto(ctx, idx, &d);
    if (rc != R_OK) {
        return rc;
    }
    int fi = fld_index(d, "KEYWORDS");
    if (fi < 0) {
        out[0] = '\0';
        return R_OK;
    }
    rval v;
    rx_rval_undef(&v);
    rc = rx_dbf_get(d, (size_t)fi, &v);
    if (rc == R_OK && v.t == RX_T_C) {
        snprintf(out, outsz, "%s", v.u.c.utf8 ? v.u.c.utf8 : "");
    }
    rx_rval_free(&v);
    return rc;
}

/* first record whose TOPIC or KEYWORDS contains needle (case-insensitive) */
int rx_help_search(rx_ctx *ctx, const char *needle, size_t *out_idx)
{
    rx_dbf *d;
    int rc = help_dbf(ctx, &d);
    if (rc != R_OK) {
        return rc;
    }
    int fi_t = fld_index(d, "TOPIC");
    int fi_k = fld_index(d, "KEYWORDS");
    uint32_t n = 0;
    rx_dbf_recount(d, &n);
    for (uint32_t r = 1; r <= n; r++) {
        rc = rx_dbf_goto(d, r);
        if (rc != R_OK) {
            return rc;
        }
        for (int fi = 0; fi < 2; fi++) {
            int idx = fi == 0 ? fi_t : fi_k;
            if (idx < 0) {
                continue;
            }
            rval v;
            rx_rval_undef(&v);
            rc = rx_dbf_get(d, (size_t)idx, &v);
            if (rc == R_OK && v.t == RX_T_C && v.u.c.utf8 && rx_strcasestr(v.u.c.utf8, needle)) {
                rx_rval_free(&v);
                *out_idx = r - 1;
                return R_OK;
            }
            rx_rval_free(&v);
        }
    }
    return R_ERR_IO_NOTFOUND;
}
