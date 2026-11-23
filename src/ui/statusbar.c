/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file statusbar.c
 * @brief Status line (spec §6.5): last row. Left: last message. Right:
 *        CAPS / NUM indicators, clock (SET CLOCK ON), work area + alias,
 *        record pointer N/M, DELETED indicator.
 */
#include "ui/ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int call0(rx_ctx *ctx, const char *name, rval *out)
{
    r_err err;
    memset(&err, 0, sizeof(err));
    int fid = rx_func_id(name, &err);
    if (fid < 0) {
        rx_rval_undef(out);
        return -1;
    }
    rx_rval_undef(out);
    if (rx_func_call(ctx, fid, 0, NULL, out, &err) != R_OK) {
        rx_rval_undef(out);
        return -1;
    }
    return 0;
}

static void call_str(rx_ctx *ctx, const char *name, char *buf, size_t bufsz)
{
    rval v;
    if (call0(ctx, name, &v) != 0 || v.t != RX_T_C) {
        if (v.t != RX_T_U) {
            rx_rval_free(&v);
        }
        buf[0] = '\0';
        return;
    }
    snprintf(buf, bufsz, "%s", v.u.c.utf8 ? v.u.c.utf8 : "");
    rx_rval_free(&v);
}

static long call_num(rx_ctx *ctx, const char *name, int *ok)
{
    rval v;
    *ok = call0(ctx, name, &v) == 0 && (v.t == RX_T_N || v.t == RX_T_I);
    long r = 0;
    if (*ok) {
        r = v.t == RX_T_N ? (long)v.u.n.d : (long)v.u.i;
    }
    if (v.t != RX_T_U) {
        rx_rval_free(&v);
    }
    return r;
}

void statusbar_render(struct rx_ui *ui)
{
    rx_screen *s = &ui->scr;
    int row = s->h - 1;
    scr_fill(s, 0, row, s->w, 1, ' ', RXA_STATUS);

    /* right side */
    char right[256];
    char alias[64] = "";
    char clock[16] = "";
    int used_ok = 0;
    long used = call_num(ui->ctx, "USED", &used_ok);
    if (used_ok && used) {
        call_str(ui->ctx, "ALIAS", alias, sizeof(alias));
    }
    if (ui->ctx->set.clock) {
        call_str(ui->ctx, "TIME", clock, sizeof(clock));
    }
    int wa_ok = 0;
    long wa = call_num(ui->ctx, "SELECT", &wa_ok);
    long rec = 0, recc = 0;
    int rec_ok = 0, recc_ok = 0;
    if (used_ok && used) {
        rec = call_num(ui->ctx, "RECNO", &rec_ok);
        recc = call_num(ui->ctx, "RECCOUNT", &recc_ok);
    }
    right[0] = '\0';
    size_t rl = 0;
    if (ui->caps) {
        rl += (size_t)snprintf(right + rl, sizeof(right) - rl, "CAPS ");
    }
    if (ui->num) {
        rl += (size_t)snprintf(right + rl, sizeof(right) - rl, "NUM ");
    }
    if (clock[0]) {
        rl += (size_t)snprintf(right + rl, sizeof(right) - rl, "%s ", clock);
    }
    if (wa_ok) {
        if (used_ok && used) {
            rl += (size_t)snprintf(right + rl, sizeof(right) - rl, "%ld %s ", wa,
                                   alias[0] ? alias : "");
            if (rec_ok && recc_ok) {
                rl += (size_t)snprintf(right + rl, sizeof(right) - rl, "%ld/%ld", rec, recc);
            }
        } else {
            rl += (size_t)snprintf(right + rl, sizeof(right) - rl, "%ld", wa);
        }
    }
    if (ui->ctx->set.deleted) {
        rl += (size_t)snprintf(right + rl, sizeof(right) - rl, " DEL");
    }
    (void)rl;
    if (right[0]) {
        int rx = s->w - 1 - (int)strlen(right);
        if (rx < 0) {
            rx = 0;
        }
        scr_text(s, rx, row, right, RXA_STATUS);
    }

    /* left: message */
    int maxw = s->w - 1 - (int)strlen(right) - 1;
    if (maxw < 0) {
        maxw = 0;
    }
    if (ui->msg[0]) {
        scr_textn(s, 0, row, ui->msg, strlen(ui->msg) > (size_t)maxw ? (size_t)maxw : strlen(ui->msg),
                  ui->msg[0] ? RXA_MSG : RXA_STATUS);
    }
}
