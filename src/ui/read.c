/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file read.c
 * @brief @ SAY painting + the modal READ loop over pending GETs
 *        (spec §9.2, M7). Runs its own event loop (like BROWSE does) because
 *        READ is invoked from inside command execution; deterministic under
 *        the script backend for golden tests.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui_internal.h"
#include "lang/funcs.h"
#include "reynard/dbf.h"

/* ---------------- @ SAY painting ---------------- */

int ui_say(void *ud, int row, int col, const char *text)
{
    rx_ui *ui = ud;
    if (!ui) {
        return R_OK;
    }
    if (text[0] == '\x01' && strcmp(text + 1, "CLEAR") == 0) {
        for (size_t i = 0; i < ui->nsays; i++) {
            free(ui->says[i]);
        }
        ui->nsays = 0;
        scr_damage(&ui->scr, 0, 1, ui->scr.w, ui->scr.h - 2);
        ui->dirty = 1;
        return R_OK;
    }
    if (ui->nsays == ui->capsays) {
        size_t nc = ui->capsays ? ui->capsays * 2 : 16;
        char **nt = realloc(ui->says, nc * sizeof(char *));
        int (*nxy)[2] = realloc(ui->say_xy, nc * sizeof(*ui->say_xy));
        if (!nt || !nxy) {
            RX_FATAL("out of memory");
        }
        ui->says = nt;
        ui->say_xy = nxy;
        ui->capsays = nc;
    }
    ui->says[ui->nsays] = rx_strdup(text);
    ui->say_xy[ui->nsays][0] = row;
    ui->say_xy[ui->nsays][1] = col;
    ui->nsays++;
    int y = row + 1, x = col; /* row 0 is the menubar */
    if (y >= 1 && y < ui->scr.h - 1) {
        scr_damage(&ui->scr, x, y, (int)strlen(text), 1);
    }
    ui->dirty = 1;
    return R_OK;
}

void ui_says_render(rx_ui *ui)
{
    for (size_t i = 0; i < ui->nsays; i++) {
        int y = ui->say_xy[i][0] + 1, x = ui->say_xy[i][1];
        if (y >= 1 && y < ui->scr.h - 1) {
            scr_text(&ui->scr, x, y, ui->says[i], RXA_DESKTOP);
        }
    }
}

/* draw pending GET values (read-only) on the desktop — FoxPro leaves the
   form visible after READ until CLEAR */
void ui_gets_render(rx_ui *ui)
{
    rx_ctx *ctx = ui->ctx;
    for (size_t i = 0; i < ctx->ngets; i++) {
        rx_get *g = &ctx->gets[i];
        int y = g->row + 1, x = g->col;
        int w = g->size_w > 0 ? g->size_w : 10;
        if (y < 1 || y >= ui->scr.h - 1) {
            continue;
        }
        scr_fill(&ui->scr, x, y, w, 1, ' ', RXA(RXC_BLACK, RXC_WHITE));
        strbuf sb;
        sb_init(&sb);
        if (g->picture) {
            rx_picture_render(ctx, &g->val, g->picture, g->function, &sb);
        } else {
            rx_vm_display(ctx, &g->val, &sb);
        }
        scr_textn_clip(&ui->scr, x, y, sb.buf ? sb.buf : "", sb.len, RXA(RXC_BLACK, RXC_WHITE), w);
        sb_free(&sb);
    }
}

void ui_says_free(rx_ui *ui)
{
    for (size_t i = 0; i < ui->nsays; i++) {
        free(ui->says[i]);
    }
    free(ui->says);
    free(ui->say_xy);
    ui->says = NULL;
    ui->say_xy = NULL;
    ui->nsays = ui->capsays = 0;
}

/* ---------------- READ ---------------- */

typedef struct {
    getsay ed;     /* edit buffer (raw text) */
    int dirty_ed;  /* editor modified since load */
    int touched;   /* a printable key arrived since load: first one replaces all
                      (FoxPro highlights the whole field when entering a GET) */
} read_ed;

/* display a value for editing: character values are right-stripped (the
   stored field pads on write, the editor must not) */
static void display_for_edit(rx_ctx *ctx, const rval *v, strbuf *sb)
{
    sb_init(sb);
    rx_vm_display(ctx, v, sb);
    if (v->t == RX_T_C && sb->buf) {
        while (sb->len && sb->buf[sb->len - 1] == ' ') {
            sb->len--;
        }
        sb->buf[sb->len] = '\0';
    }
}

/* parse editor text into an rval of the current value's type */
static int text_to_val(rx_ctx *ctx, const char *txt, const rval *cur, rval *out)
{
    rx_rval_undef(out);
    switch (cur->t) {
    case RX_T_N:
    case RX_T_I:
    case RX_T_B: {
        char *end = NULL;
        double d = strtod(txt, &end);
        if (end == txt) {
            d = 0;
        }
        out->t = RX_T_N;
        out->u.n.d = d;
        out->u.n.dec = cur->t == RX_T_N ? cur->u.n.dec : 0;
        return R_OK;
    }
    case RX_T_L: {
        char c = txt[0] ? (char)toupper((unsigned char)txt[0]) : 'F';
        out->t = RX_T_L;
        out->u.l = (c == 'T' || c == 'Y') ? L_T : L_F;
        return R_OK;
    }
    case RX_T_D: {
        int m = 0, d = 0, y = 0;
        if (sscanf(txt, "%d/%d/%d", &m, &d, &y) == 3 || sscanf(txt, "%d-%d-%d", &y, &m, &d) == 3) {
            if (y < 100) {
                y += 1900;
            }
            out->t = RX_T_D;
            out->u.d.y = (int16_t)y;
            out->u.d.m = (int16_t)m;
            out->u.d.d = (int16_t)d;
            return R_OK;
        }
        out->t = RX_T_D;
        return R_OK;
    }
    default: {
        out->t = RX_T_C;
        out->u.c.utf8 = rx_strdup(txt);
        out->u.c.n = strlen(txt);
        return R_OK;
    }
    }
    (void)ctx;
}

static void render_read(rx_ui *ui, read_ed *eds, int cur)
{
    rx_ctx *ctx = ui->ctx;
    scr_fill(&ui->scr, 0, 1, ui->scr.w, ui->scr.h - 2, ' ', RXA_DESKTOP);
    ui_says_render(ui);
    for (size_t i = 0; i < ctx->ngets; i++) {
        rx_get *g = &ctx->gets[i];
        int y = g->row + 1, x = g->col;
        if (g->say && g->say_row >= 0) {
            scr_text(&ui->scr, g->say_col, g->say_row + 1, g->say, RXA_DESKTOP);
        }
        int w = g->size_w > 0 ? g->size_w : 10;
        if (y < 1 || y >= ui->scr.h - 1) {
            continue;
        }
        /* field body */
        uint16_t attr = (int)i == cur ? RXA(RXC_BLACK, RXC_BCYAN) : RXA(RXC_BLACK, RXC_WHITE);
        if (!g->enabled) {
            attr = RXA(RXC_GRAY, RXC_WHITE);
        }
        scr_fill(&ui->scr, x, y, w, 1, ' ', attr);
        char txt[512];
        if ((int)i == cur) {
            snprintf(txt, sizeof(txt), "%s", eds[i].ed.buf ? eds[i].ed.buf : "");
        } else {
            strbuf sb;
            sb_init(&sb);
            if (g->picture) {
                rx_picture_render(ctx, &g->val, g->picture, g->function, &sb);
            } else {
                rx_vm_display(ctx, &g->val, &sb);
            }
            snprintf(txt, sizeof(txt), "%.500s", sb.buf ? sb.buf : "");
            sb_free(&sb);
        }
        scr_textn_clip(&ui->scr, x, y, txt, strlen(txt), attr, w);
    }
    menubar_render(ui);
    statusbar_render(ui);
}

/* commit editor text into g->val; returns 0 ok, 1 = VALID failed */
static int commit_get(rx_ctx *ctx, rx_get *g, read_ed *ed)
{
    rval nv;
    text_to_val(ctx, ed->ed.buf ? ed->ed.buf : "", &g->val, &nv);
    /* RANGE check (numeric/date) */
    if (g->has_range && g->range_lo.t != RX_T_U) {
        int ok = 1;
        if (nv.t == RX_T_N) {
            double d, lo = 0, hi = 0;
            rv_to_num(&nv, &d);
            rv_to_num(&g->range_lo, &lo);
            if (d < lo) {
                ok = 0;
            }
            if (g->range_hi.t != RX_T_U) {
                rv_to_num(&g->range_hi, &hi);
                if (d > hi) {
                    ok = 0;
                }
            }
        }
        if (!ok) {
            rx_rval_free(&nv);
            return 2; /* range */
        }
    }
    rx_rval_free(&g->val);
    g->val = nv;
    /* VALID runs with the GET variable already holding the edited value
       (FoxPro semantics): stage it into the field/memvar first, restore the
       old value if VALID rejects */
    if (g->has_valid) {
        rval old;
        rx_rval_undef(&old);
        int staged = 0;
        if (g->is_field) {
            rx_dbf *dbf;
            size_t idx;
            if (rx_wa_dbf(0, &dbf) == R_OK && rx_dbf_field_index(dbf, g->var, &idx) == R_OK) {
                if (rx_dbf_get(dbf, idx, &old) == R_OK) {
                    if (rx_dbf_put(dbf, idx, &g->val) == R_OK) {
                        staged = 1;
                    }
                }
            }
        } else {
            rx_memvar *mv = rx_memvar_find(ctx, g->var);
            if (mv && !mv->is_array) {
                rx_rval_deep(&old, &mv->val);
                if (rx_memvar_set(ctx, g->var, &g->val) == R_OK) {
                    staged = 1;
                }
            }
        }
        rval v;
        rx_rval_undef(&v);
        int rc = rx_vm_run(ctx, &g->valid, &v, &ctx->err);
        rx_logic l;
        int ok = rc == R_OK && rv_to_logic(&v, &l) && l == L_T;

        rx_rval_free(&v);
        if (staged) {
            if (g->is_field) {
                rx_dbf *dbf;
                size_t idx;
                if (rx_wa_dbf(0, &dbf) == R_OK && rx_dbf_field_index(dbf, g->var, &idx) == R_OK) {
                    rx_dbf_put(dbf, idx, ok ? &g->val : &old);
                }
            } else {
                rx_memvar_set(ctx, g->var, ok ? &g->val : &old);
            }
        }
        rx_rval_free(&old);
        if (!ok) {
            return 1;
        }
    }
    return 0;
}

static int when_ok(rx_ctx *ctx, rx_get *g)
{
    if (!g->has_when) {
        return 1;
    }
    rval v;
    rx_rval_undef(&v);
    int rc = rx_vm_run(ctx, &g->when, &v, &ctx->err);
    if (rc != R_OK) {
        return 0;
    }
    rx_logic l;
    int ok = rv_to_logic(&v, &l);
    rx_rval_free(&v);
    return ok && l == L_T;
}

int ui_read(void *ud)
{
    rx_ui *ui = ud;
    rx_ctx *ctx = ui->ctx;
    size_t n = ctx->ngets;
    read_ed *eds = calloc(n, sizeof(read_ed));
    if (!eds) {
        RX_FATAL("out of memory");
    }
    for (size_t i = 0; i < n; i++) {
        gs_init(&eds[i].ed, 0);
        rx_get *g = &ctx->gets[i];
        strbuf sb;
        display_for_edit(ctx, &g->val, &sb);
        if (sb.buf) {
            gs_insert(&eds[i].ed, sb.buf, sb.len);
        }
        sb_free(&sb);
        g->enabled = when_ok(ctx, g);
    }
    /* first enabled get */
    int cur = -1;
    for (size_t i = 0; i < n; i++) {
        if (ctx->gets[i].enabled) {
            cur = (int)i;
            break;
        }
    }
    if (cur < 0) {
        cur = 0;
    }
    ctx->cur_obj = cur;
    int done = 0;
    while (!done) {
        render_read(ui, eds, cur);
        scr_blit(&ui->scr, ui->tb);
        revent ev;
        memset(&ev, 0, sizeof(ev));
        int rc = ui->tb->poll(&ev, 250);
        if (rc != R_OK) {
            break; /* backend gone */
        }
        if (ev.kind == EV_EOF) {
            ctx->read_abort = 1;
            break;
        }
        if (ev.kind == EV_RESIZE) {
            int w = ui->scr.w, h = ui->scr.h;
            ui->tb->size(&w, &h);
            scr_resize(&ui->scr, w, h);
            continue;
        }
        if (ev.kind == EV_MOUSE) {
            const rmouse *m = &ev.u.m;
            if (m->kind == RM_PRESS && m->button == 1) {
                for (size_t i = 0; i < n; i++) {
                    rx_get *g = &ctx->gets[i];
                    int y = g->row + 1, x = g->col;
                    int w = g->size_w > 0 ? g->size_w : 10;
                    if ((int)m->y == y && (int)m->x >= x && (int)m->x < x + w && g->enabled) {
                        if (cur >= 0) {
                            int crc = commit_get(ctx, &ctx->gets[cur], &eds[cur]);
                            if (crc == 1) {
                                ui_set_msg(ui, "Invalid input (VALID)");
                                break;
                            }
                            if (crc == 2) {
                                ui_set_msg(ui, "Value out of RANGE");
                                break;
                            }
                        }
                        cur = (int)i;
                        ctx->cur_obj = cur;
                        break;
                    }
                }
            }
            continue;
        }
        if (ev.kind != EV_KEY) {
            continue;
        }
        const rkey *k = &ev.u.k;
        int nav_next = (k->sym == RXK_TAB || k->sym == RXK_ENTER || k->sym == RXK_DOWN);
        int nav_prev = (k->sym == RXK_UP || k->sym == RXK_BACKTAB);
        if (k->sym == RXK_ESC) {
            ctx->read_abort = 1;
            done = 1;
            break;
        }
        if (k->sym == RXK_CTL('W') || ((k->mods & RXK_MOD_CTRL) && k->sym == RXK_END)) {
            /* Ctrl+W / Ctrl+End: commit + exit */
            if (cur >= 0) {
                commit_get(ctx, &ctx->gets[cur], &eds[cur]);
            }
            done = 1;
            break;
        }
        if (nav_next || nav_prev) {
            if (cur >= 0) {
                int crc = commit_get(ctx, &ctx->gets[cur], &eds[cur]);
                if (crc == 1) {
                    ui_set_msg(ui, "Invalid input (VALID)");
                    continue;
                }
                if (crc == 2) {
                    ui_set_msg(ui, "Value out of RANGE");
                    continue;
                }
            }
            int nxt = cur + (nav_next ? 1 : -1);
            if (nav_next && nxt >= (int)n) {
                if (ctx->read_cycle) {
                    nxt = 0;
                } else {
                    done = 1; /* leaving the last GET ends the READ */
                    break;
                }
            }
            if (nav_prev && nxt < 0) {
                if (ctx->read_cycle) {
                    nxt = (int)n - 1;
                } else {
                    done = 1;
                    break;
                }
            }
            /* skip disabled (WHEN false) fields */
            int guard = 0;
            while (nxt >= 0 && nxt < (int)n && !ctx->gets[nxt].enabled && guard++ < (int)n) {
                ctx->gets[nxt].enabled = when_ok(ctx, &ctx->gets[nxt]);
                if (ctx->gets[nxt].enabled) {
                    break;
                }
                nxt += nav_next ? 1 : -1;
            }
            if (nxt < 0 || nxt >= (int)n) {
                if (ctx->read_cycle) {
                    nxt = nav_next ? 0 : (int)n - 1;
                } else {
                    done = 1;
                    break;
                }
            }
            cur = nxt;
            ctx->cur_obj = cur;
            /* load the new field's value into its editor */
            rx_get *g = &ctx->gets[cur];
            gs_clear(&eds[cur].ed);
            eds[cur].touched = 0;
            strbuf sb;
            display_for_edit(ctx, &g->val, &sb);
            if (sb.buf) {
                gs_insert(&eds[cur].ed, sb.buf, sb.len);
            }
            sb_free(&sb);
            gs_end(&eds[cur].ed);
            continue;
        }
        /* editing keys go to the current editor */
        if (cur < 0) {
            continue;
        }
        getsay *ed = &eds[cur].ed;
        eds[cur].dirty_ed = 1;
        /* the field is highlighted on entry: the first printable key (or a
           delete) replaces the whole contents; cursor keys just collapse the
           highlight (FoxPro behavior) */
        int wipe = !eds[cur].touched &&
                   (k->sym == RXK_BS || k->sym == RXK_DEL || (k->cp && k->cp >= 32));
        if (wipe) {
            gs_clear(ed);
            eds[cur].touched = 1;
        }
        switch (k->sym) {
        case RXK_BS:
            gs_backspace(ed);
            break;
        case RXK_DEL:
            gs_delete(ed);
            break;
        case RXK_LEFT:
            gs_left(ed);
            eds[cur].touched = 1;
            break;
        case RXK_RIGHT:
            gs_right(ed);
            eds[cur].touched = 1;
            break;
        case RXK_HOME:
            gs_home(ed);
            eds[cur].touched = 1;
            break;
        case RXK_END:
            gs_end(ed);
            eds[cur].touched = 1;
            break;
        case RXK_CTL('Y'):
            gs_kill_end(ed);
            break;
        default:
            if (k->cp && k->cp >= 32) {
                gs_insert_cp(ed, k->cp);
            }
            break;
        }
    }
    /* commit the last field */
    if (cur >= 0 && !ctx->read_abort) {
        commit_get(ctx, &ctx->gets[cur], &eds[cur]);
    }
    for (size_t i = 0; i < n; i++) {
        gs_free(&eds[i].ed);
    }
    free(eds);
    ui->dirty = 1;
    return R_OK;
}
