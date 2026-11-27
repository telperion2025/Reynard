/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_txn.c
 * @brief BEGIN TRANSACTION / END TRANSACTION / ROLLBACK (spec 8.6, M9):
 *        per-work-area before-image journaling via src/data/journal.c
 *        (<dbfstem>.RJN sidecar); commit/rollback on the current table.
 */
#include <string.h>

#include "lang/lang.h"
#include "reynard/dbf.h"
#include "util/util.h"

static int cur_dbf(rx_ctx* ctx, rx_stmt* s, rx_dbf** out) {
    int rc = rx_wa_dbf(0, out);
    if (rc != R_OK) {
        return rx_raise(ctx, R_ERR_DBF_NOTOPEN, s->line, s->col, "No table in use");
    }
    return R_OK;
}

int rx_cmd_txn(rx_ctx* ctx, rx_stmt* s) {
    rx_dbf* dbf;
    int rc = cur_dbf(ctx, s, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    switch (s->kind) {
    case ST_BEGIN_TXN:
        rc = rx_begin(dbf);
        if (rc == R_OK && ctx->set.talk) {
            rx_outf(ctx, "Transaction started.\n");
        }
        break;
    case ST_COMMIT_TXN:
        rc = rx_commit(dbf);
        if (rc == R_OK && ctx->set.talk) {
            rx_outf(ctx, "Transaction committed.\n");
        }
        break;
    case ST_ROLLBACK:
        rc = rx_rollback(dbf);
        if (rc == R_OK && ctx->set.talk) {
            rx_outf(ctx, "Transaction rolled back.\n");
        }
        break;
    default:
        rc = R_ERR_SYNTAX;
        break;
    }
    if (rc != R_OK) {
        return rx_raise(ctx, rc, s->line, s->col, "Transaction failed");
    }
    return R_OK;
}
