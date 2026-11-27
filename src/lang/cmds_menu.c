/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cmds_menu.c
 * @brief Menu generator (spec 9.5, M9): DEFINE MENU/PAD/POPUP/BAR, ON PAD,
 *        ON SELECTION PAD/BAR, ACTIVATE/SHOW/DEACTIVATE/RELEASE MENU.
 *        The model lives in rx_ctx->menus; ACTIVATE pushes the menu as the
 *        app pad on the menubar through ctx->menu_sync_fn (ui menudef.c).
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "lang/lang.h"
#include "util/util.h"

static rx_menu_def *menu_find(rx_ctx *ctx, const char *name)
{
    char up[64];
    snprintf(up, sizeof(up), "%.*s", (int)sizeof(up) - 1, name);
    for (char *c = up; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    for (size_t i = 0; i < ctx->nmenus; i++) {
        if (strcmp(ctx->menus[i].name, up) == 0) {
            return &ctx->menus[i];
        }
    }
    return NULL;
}

static rx_menu_def *menu_ensure(rx_ctx *ctx, const char *name, int is_popup)
{
    rx_menu_def *m = menu_find(ctx, name);
    if (m) {
        return m;
    }
    if (ctx->nmenus == ctx->capmenus) {
        size_t nc = ctx->capmenus ? ctx->capmenus * 2 : 8;
        rx_menu_def *nm = realloc(ctx->menus, nc * sizeof(rx_menu_def));
        if (!nm) {
            RX_FATAL("out of memory");
        }
        ctx->menus = nm;
        ctx->capmenus = nc;
    }
    m = &ctx->menus[ctx->nmenus++];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%.*s", (int)sizeof(m->name) - 1, name);
    for (char *c = m->name; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    m->is_popup = is_popup;
    return m;
}

static rx_menu_item_def *item_ensure(rx_menu_def *m, const char *name)
{
    char up[64];
    snprintf(up, sizeof(up), "%.*s", (int)sizeof(up) - 1, name);
    for (char *c = up; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    for (size_t i = 0; i < m->nitems; i++) {
        if (strcmp(m->items[i].name, up) == 0) {
            return &m->items[i];
        }
    }
    if (m->nitems >= 32) {
        return NULL;
    }
    rx_menu_item_def *it = &m->items[m->nitems++];
    memset(it, 0, sizeof(*it));
    snprintf(it->name, sizeof(it->name), "%s", up);
    snprintf(it->label, sizeof(it->label), "%.*s", (int)sizeof(it->label) - 1, up); /* default prompt = name */
    return it;
}

/* re-sync the menubar's app pad from ctx (ui hook) */
void rx_menu_sync(rx_ctx *ctx)
{
    if (ctx->menu_sync_fn) {
        ctx->menu_sync_fn(ctx->menu_sync_ud);
    }
}

/* called by the ui hook with the ui as ud; the model is on ui->ctx */
int rx_cmd_menu(rx_ctx *ctx, rx_stmt *s)
{
    switch (s->kind) {
    case ST_DEFINE_MENU: {
        /* strs: name */
        if (s->nstrs < 1) {
            return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "DEFINE MENU needs a name");
        }
        menu_ensure(ctx, s->strs[0], 0);
        return R_OK;
    }
    case ST_DEFINE_POPUP: {
        if (s->nstrs < 1) {
            return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "DEFINE POPUP needs a name");
        }
        menu_ensure(ctx, s->strs[0], 1);
        return R_OK;
    }
    case ST_DEFINE_PAD: {
        /* strs: pad, menu, prompt */
        if (s->nstrs < 2) {
            return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "DEFINE PAD p OF m PROMPT ...");
        }
        rx_menu_def *m = menu_ensure(ctx, s->strs[1], 0);
        rx_menu_item_def *it = item_ensure(m, s->strs[0]);
        if (!it) {
            return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "Too many pads");
        }
        if (s->nstrs > 2) {
            snprintf(it->label, sizeof(it->label), "%.*s", (int)sizeof(it->label) - 1, s->strs[2]);
        }
        /* KEY clause hotkey */
        for (size_t i = 0; i < s->nclause; i++) {
            if (strcmp(s->clauses[i].key, "KEY") == 0 && s->clauses[i].word[0]) {
                it->hotkey = toupper((unsigned char)s->clauses[i].word[0]);
            }
        }
        return R_OK;
    }
    case ST_DEFINE_BAR: {
        /* strs: barno, popup, prompt */
        if (s->nstrs < 2) {
            return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "DEFINE BAR n OF p PROMPT ...");
        }
        rx_menu_def *m = menu_ensure(ctx, s->strs[1], 1);
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "%.*s", (int)sizeof(lbl) - 1, s->strs[0]); /* bar number as label key */
        rx_menu_item_def *it = item_ensure(m, lbl);
        if (!it) {
            return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "Too many bars");
        }
        if (s->nstrs > 2) {
            snprintf(it->label, sizeof(it->label), "%.*s", (int)sizeof(it->label) - 1, s->strs[2]);
        }
        return R_OK;
    }
    case ST_ON_PAD: {
        /* strs: pad, menu; clause CMD = command */
        if (s->nstrs < 2) {
            return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "ON PAD p OF m cmd");
        }
        rx_menu_def *m = menu_find(ctx, s->strs[1]);
        if (!m) {
            return rx_raise(ctx, R_ERR_VAR, s->line, s->col, "Undefined menu %s", s->strs[1]);
        }
        /* ON PAD p OF m cmd: the command runs when pad p is selected
           (menubar run_item in the UI, or ACTIVATE MENU m PAD p) */
        rx_menu_item_def *it = item_ensure(m, s->strs[0]);
        if (it) {
            for (size_t i = 0; i < s->nclause; i++) {
                if (strcmp(s->clauses[i].key, "CMD") == 0) {
                    snprintf(it->cmd, sizeof(it->cmd), "%.*s", (int)sizeof(it->cmd) - 1, s->clauses[i].word);
                }
            }
        }
        return R_OK;
    }
    case ST_ON_SELECTION: {
        /* strs: kind(PAD|BAR|ALL), name, menu; clause CMD */
        const char *cmd = "";
        for (size_t i = 0; i < s->nclause; i++) {
            if (strcmp(s->clauses[i].key, "CMD") == 0) {
                cmd = s->clauses[i].word;
            }
        }
        if (s->nstrs < 3) {
            return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "ON SELECTION PAD/BAR ...");
        }
        rx_menu_def *m = menu_find(ctx, s->strs[2]);
        if (!m) {
            return rx_raise(ctx, R_ERR_VAR, s->line, s->col, "Undefined menu %s", s->strs[2]);
        }
        if (strcmp(s->strs[0], "ALL") == 0) {
            for (size_t i = 0; i < m->nitems; i++) {
                snprintf(m->items[i].cmd, sizeof(m->items[i].cmd), "%.*s", (int)sizeof(m->items[i].cmd) - 1, cmd);
            }
        } else {
            rx_menu_item_def *it = item_ensure(m, s->strs[1]);
            if (it) {
                snprintf(it->cmd, sizeof(it->cmd), "%.*s", (int)sizeof(it->cmd) - 1, cmd);
            }
        }
        return R_OK;
    }
    case ST_ACTIVATE_MENU: {
        if (s->nstrs < 1) {
            return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "ACTIVATE MENU needs a name");
        }
        rx_menu_def *m = menu_find(ctx, s->strs[0]);
        if (!m) {
            return rx_raise(ctx, R_ERR_VAR, s->line, s->col, "Undefined menu %s", s->strs[0]);
        }
        snprintf(ctx->active_menu, sizeof(ctx->active_menu), "%.*s", (int)sizeof(ctx->active_menu) - 1, m->name);
        m->active = 1;
        m->shown = 1;
        rx_menu_sync(ctx);
        /* ACTIVATE MENU m PAD p selects pad p immediately: run its ON PAD
           command (same path the menubar's run_item takes in the UI) */
        const char *pad = "";
        for (size_t i = 0; i < s->nclause; i++) {
            if (strcmp(s->clauses[i].key, "PAD") == 0) {
                pad = s->clauses[i].word;
            }
        }
        if (pad[0]) {
            for (size_t i = 0; i < m->nitems; i++) {
                if (rx_strcasecmp(m->items[i].name, pad) == 0) {
                    if (m->items[i].cmd[0]) {
                        return rx_run_source(ctx, m->items[i].cmd, "<menu>");
                    }
                    break;
                }
            }
        }
        return R_OK;
    }
    case ST_SHOW_MENU: {
        if (s->nstrs < 1) {
            return R_OK;
        }
        rx_menu_def *m = menu_find(ctx, s->strs[0]);
        if (m) {
            m->shown = 1;
            snprintf(ctx->active_menu, sizeof(ctx->active_menu), "%.*s", (int)sizeof(ctx->active_menu) - 1, m->name);
            rx_menu_sync(ctx);
        }
        return R_OK;
    }
    case ST_DEACTIVATE_MENU: {
        for (size_t i = 0; i < ctx->nmenus; i++) {
            ctx->menus[i].active = 0;
        }
        ctx->active_menu[0] = '\0';
        rx_menu_sync(ctx);
        return R_OK;
    }
    case ST_RELEASE_MENU: {
        if (s->nstrs < 1) {
            /* RELEASE MENU (all) */
            ctx->nmenus = 0;
            ctx->active_menu[0] = '\0';
            rx_menu_sync(ctx);
            return R_OK;
        }
        rx_menu_def *m = menu_find(ctx, s->strs[0]);
        if (m) {
            size_t idx = (size_t)(m - ctx->menus);
            memmove(&ctx->menus[idx], &ctx->menus[idx + 1],
                    (ctx->nmenus - idx - 1) * sizeof(rx_menu_def));
            ctx->nmenus--;
            if (strcmp(ctx->active_menu, m->name) == 0) {
                ctx->active_menu[0] = '\0';
            }
            rx_menu_sync(ctx);
        }
        return R_OK;
    }
    default:
        return rx_raise(ctx, R_ERR_SYNTAX, s->line, s->col, "Unknown menu command");
    }
}
