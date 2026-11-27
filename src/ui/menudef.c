/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file menudef.c
 * @brief Declarative menubar definition (spec §6.3): FoxPro 2.x menu set.
 *        Every item carries an action plus optional disabled/checked hooks;
 *        every menu operation is also reachable by keystroke or command
 *        (accessibility rule), so actions never hold hidden state.
 */
#include "ui/ui_internal.h"

#include <stdio.h>
#include <string.h>

static int db_open(rx_ctx* ctx) {
    r_err err;
    memset(&err, 0, sizeof(err));
    int fid = rx_func_id("USED", &err);
    if (fid < 0) {
        return 0;
    }
    rval v;
    rx_rval_undef(&v);
    int rc = rx_func_call(ctx, fid, 0, NULL, &v, &err);
    int used = rc == R_OK && (v.t == RX_T_L && v.u.l == L_T);
    if (v.t != RX_T_U) {
        rx_rval_free(&v);
    }
    return used;
}

static void trace(const char* menu, const char* item) {
    rx_ui* ui = rx_ui_current();
    if (!ui) {
        return;
    }
    char line[300];
    snprintf(line, sizeof(line), "%s > %s\n", menu, item);
    cmdwin_out(ui->cmd, line, strlen(line));
    ui_set_msg(ui, "%s > %s", menu, item);
    ui->dirty = 1;
}

static int act_about(rx_ctx* ctx) {
    (void)ctx;
    trace("System", "About Reynard");
    dlg_about(rx_ui_current());
    return R_OK;
}

static int act_macros(rx_ctx* ctx) {
    (void)ctx;
    trace("System", "Macros");
    dlg_macros(rx_ui_current());
    return R_OK;
}

static int act_filer(rx_ctx* ctx) {
    (void)ctx;
    trace("System", "Filer");
    dlg_filer(rx_ui_current());
    return R_OK;
}

static int act_calc(rx_ctx* ctx) {
    (void)ctx;
    trace("System", "Calculator");
    dlg_calc(rx_ui_current());
    return R_OK;
}

static int act_calendar(rx_ctx* ctx) {
    (void)ctx;
    trace("System", "Calendar/Diary");
    dlg_calendar(rx_ui_current());
    return R_OK;
}

static int act_ascii(rx_ctx* ctx) {
    (void)ctx;
    trace("System", "ASCII Chart");
    dlg_ascii(rx_ui_current());
    return R_OK;
}

static int act_help(rx_ctx* ctx) {
    (void)ctx;
    trace("System", "Help");
    dlg_help(rx_ui_current());
    return R_OK;
}

static int act_quit(rx_ctx* ctx) {
    trace("File", "Quit");
    ctx->quit = 1;
    return R_OK;
}

static int act_msg(const char* item, const char* when) {
    rx_ui* ui = rx_ui_current();
    if (ui) {
        trace("Menu", item);
        ui_set_msg(ui, "%s lands at %s", item, when);
    }
    return R_OK;
}

static int act_file_new(rx_ctx* ctx) {
    (void)ctx;
    return act_msg("New", "M7");
}
static int act_file_open(rx_ctx* ctx) {
    (void)ctx;
    return act_msg("Open", "M5");
}
static int act_file_close(rx_ctx* ctx) {
    trace("File", "Close");
    rx_run_source(ctx, "CLOSE ALL", "<menu>");
    return R_OK;
}
static int act_file_save(rx_ctx* ctx) {
    (void)ctx;
    return act_msg("Save", "M4");
}
static int act_file_saveas(rx_ctx* ctx) {
    (void)ctx;
    return act_msg("Save As", "M4");
}
static int act_file_revert(rx_ctx* ctx) {
    (void)ctx;
    return act_msg("Revert", "M4");
}
static int act_file_printer(rx_ctx* ctx) {
    (void)ctx;
    return act_msg("Printer Setup", "M8");
}
static int act_file_print(rx_ctx* ctx) {
    (void)ctx;
    return act_msg("Print", "M8");
}

static int act_edit_clear(rx_ctx* ctx) {
    (void)ctx;
    rx_ui* ui = rx_ui_current();
    if (ui && ui->cmd) {
        gs_clear(&ui->cmd->ed);
        ui->dirty = 1;
        trace("Edit", "Clear");
    }
    return R_OK;
}
static int act_edit_msg(const char* item) {
    return act_msg(item, "M4");
}
static int act_edit_undo(rx_ctx* ctx) {
    (void)ctx;
    return act_edit_msg("Undo");
}
static int act_edit_redo(rx_ctx* ctx) {
    (void)ctx;
    return act_edit_msg("Redo");
}
static int act_edit_cut(rx_ctx* ctx) {
    (void)ctx;
    return act_edit_msg("Cut");
}
static int act_edit_copy(rx_ctx* ctx) {
    (void)ctx;
    return act_edit_msg("Copy");
}
static int act_edit_paste(rx_ctx* ctx) {
    (void)ctx;
    return act_edit_msg("Paste");
}
static int act_edit_selall(rx_ctx* ctx) {
    (void)ctx;
    return act_edit_msg("Select All");
}
static int act_edit_find(rx_ctx* ctx) {
    (void)ctx;
    return act_edit_msg("Find");
}
static int act_edit_findagain(rx_ctx* ctx) {
    (void)ctx;
    return act_edit_msg("Find Again");
}
static int act_edit_replace(rx_ctx* ctx) {
    (void)ctx;
    return act_edit_msg("Replace");
}

static int act_db(const char* item, const char* when) {
    return act_msg(item, when);
}
static int act_db_setup(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Setup", "M4");
}

/* run a data command through the interpreter so BROWSE opens the M4 grid */
static int act_run(rx_ctx* ctx, const char* menu, const char* item, const char* cmd) {
    trace(menu, item);
    rx_run_source(ctx, cmd, "<menu>");
    ctx->abort = 0; /* an interactive error must not poison the next command */
    ctx->err.code = R_OK;
    ctx->err_printed = 0;
    return R_OK;
}
static int act_db_browse(rx_ctx* ctx) {
    return act_run(ctx, "Database", "Browse", "BROWSE");
}
static int act_db_append(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Append From", "M4");
}
static int act_db_copy(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Copy To", "M2 (COPY TO)");
}
static int act_db_sort(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Sort", "M5");
}
static int act_db_total(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Total", "M5");
}
static int act_db_avg(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Average", "M5");
}
static int act_db_count(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Count", "M5");
}
static int act_db_sum(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Sum", "M5");
}
static int act_db_calc(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Calculate", "M5");
}
static int act_db_report(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Report", "M8");
}
static int act_db_label(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Label", "M8");
}
static int act_db_pack(rx_ctx* ctx) {
    trace("Database", "Pack");
    rx_run_source(ctx, "PACK", "<menu>");
    return R_OK;
}
static int act_db_reindex(rx_ctx* ctx) {
    (void)ctx;
    return act_db("Reindex", "M6");
}

static int act_rec(const char* item) {
    return act_msg(item, "M4");
}
static int act_rec_append(rx_ctx* ctx) {
    return act_run(ctx, "Record", "Append", "APPEND");
}
static int act_rec_change(rx_ctx* ctx) {
    (void)ctx;
    return act_rec("Change");
}
static int act_rec_goto(rx_ctx* ctx) {
    (void)ctx;
    return act_rec("Goto");
}
static int act_rec_locate(rx_ctx* ctx) {
    (void)ctx;
    return act_rec("Locate");
}
static int act_rec_cont(rx_ctx* ctx) {
    (void)ctx;
    return act_rec("Continue");
}
static int act_rec_seek(rx_ctx* ctx) {
    (void)ctx;
    return act_rec("Seek");
}
static int act_rec_replace(rx_ctx* ctx) {
    (void)ctx;
    return act_rec("Replace");
}
static int act_rec_delete(rx_ctx* ctx) {
    (void)ctx;
    return act_rec("Delete");
}
static int act_rec_recall(rx_ctx* ctx) {
    (void)ctx;
    return act_rec("Recall");
}

static int act_prog(const char* item, const char* when) {
    return act_msg(item, when);
}
static int act_prog_do(rx_ctx* ctx) {
    (void)ctx;
    return act_prog("Do", "M5");
}
static int act_prog_cancel(rx_ctx* ctx) {
    (void)ctx;
    return act_prog("Cancel", "M5");
}
static int act_prog_resume(rx_ctx* ctx) {
    (void)ctx;
    return act_prog("Resume", "M5");
}
static int act_prog_compile(rx_ctx* ctx) {
    (void)ctx;
    return act_prog("Compile", "M5");
}
static int act_prog_generate(rx_ctx* ctx) {
    (void)ctx;
    return act_prog("Generate", "M9");
}
static int act_prog_filer(rx_ctx* ctx) {
    (void)ctx;
    return act_filer(ctx);
}

static int act_win_hide(rx_ctx* ctx) {
    (void)ctx;
    rx_ui* ui = rx_ui_current();
    if (ui && ui->cmd) {
        ui->cmd->w.visible = 0;
        ui->dirty = 1;
        trace("Window", "Hide");
    }
    return R_OK;
}

static int act_win_command(rx_ctx* ctx) {
    (void)ctx;
    rx_ui* ui = rx_ui_current();
    if (ui && ui->cmd) {
        ui->cmd->w.visible = 1;
        win_bring_top(ui, &ui->cmd->w);
        ui->dirty = 1;
        trace("Window", "Command");
    }
    return R_OK;
}

static int act_win_cycle(rx_ctx* ctx) {
    (void)ctx;
    rx_ui* ui = rx_ui_current();
    if (ui && ui->wins && ui->wins->next) {
        rx_win* front = ui->wins;
        win_remove(ui, front);
        /* append at the tail */
        rx_win** pp = &ui->wins;
        while (*pp) {
            pp = &(*pp)->next;
        }
        *pp = front;
        front->next = NULL;
        ui->dirty = 1;
        trace("Window", "Cycle");
    }
    return R_OK;
}

static int act_win_clear(rx_ctx* ctx) {
    (void)ctx;
    rx_ui* ui = rx_ui_current();
    if (ui && ui->cmd) {
        for (size_t i = 0; i < ui->cmd->nlines; i++) {
            free(ui->cmd->lines[(ui->cmd->head + i) % ui->cmd->caplines]);
        }
        ui->cmd->nlines = 0;
        ui->cmd->npend = 0;
        ui->dirty = 1;
        trace("Window", "Clear");
    }
    return R_OK;
}

static int act_win_zoom(rx_ctx* ctx) {
    (void)ctx;
    rx_ui* ui = rx_ui_current();
    if (ui && ui->cmd) {
        rx_win* w = &ui->cmd->w;
        if (w->x == 1 && w->y == 1 && w->w == ui->scr.w - 2 && w->h == ui->scr.h - 3) {
            cmdwin_layout(ui, ui->cmd);
            w->x = 2;
            w->y = ui->scr.h - 1 - 8;
            w->w = ui->scr.w - 4;
            w->h = 8;
            win_clamp(ui, w);
        } else {
            w->x = 1;
            w->y = 1;
            w->w = ui->scr.w - 2;
            w->h = ui->scr.h - 3;
        }
        ui->dirty = 1;
        trace("Window", "Zoom");
    }
    return R_OK;
}

static int act_win_view(rx_ctx* ctx) {
    (void)ctx;
    return act_msg("View", "M5");
}
static int act_win_min(rx_ctx* ctx) {
    (void)ctx;
    return act_win_hide(ctx);
}

static int act_help_contents(rx_ctx* ctx) {
    (void)ctx;
    trace("Help", "Contents");
    dlg_help(rx_ui_current());
    return R_OK;
}
static int act_help_search(rx_ctx* ctx) {
    (void)ctx;
    return act_msg("Search for Help On", "M9");
}
static int act_help_how(rx_ctx* ctx) {
    (void)ctx;
    return act_msg("How to Use Help", "M9");
}

static int dis_no_db(rx_ctx* ctx) {
    return !db_open(ctx);
}

static const menu_item system_items[] = {
    {"About Reynard...", 'A', act_about, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Macros...", 'M', act_macros, NULL, NULL, 0, NULL},
    {"Filer", 'F', act_filer, NULL, NULL, 0, NULL},
    {"Calculator", 'C', act_calc, NULL, NULL, 0, NULL},
    {"Calendar/Diary", 'D', act_calendar, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"ASCII Chart", 'S', act_ascii, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Help", 'H', act_help, NULL, NULL, 0, NULL},
};

static const menu_item file_items[] = {
    {"New...", 'N', act_file_new, dis_no_db, NULL, 0, NULL},
    {"Open...", 'O', act_file_open, NULL, NULL, 0, NULL},
    {"Close", 'C', act_file_close, dis_no_db, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Save", 'S', act_file_save, dis_no_db, NULL, 0, NULL},
    {"Save As...", 'A', act_file_saveas, dis_no_db, NULL, 0, NULL},
    {"Revert", 'R', act_file_revert, dis_no_db, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Printer Setup...", 'P', act_file_printer, NULL, NULL, 0, NULL},
    {"Print...", 'P', act_file_print, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Quit", 'Q', act_quit, NULL, NULL, 0, NULL},
};

static const menu_item edit_items[] = {
    {"Undo", 'U', act_edit_undo, NULL, NULL, 0, NULL},
    {"Redo", 'R', act_edit_redo, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Cut", 'T', act_edit_cut, NULL, NULL, 0, NULL},
    {"Copy", 'C', act_edit_copy, NULL, NULL, 0, NULL},
    {"Paste", 'P', act_edit_paste, NULL, NULL, 0, NULL},
    {"Clear", 'L', act_edit_clear, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Select All", 'A', act_edit_selall, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Find...", 'F', act_edit_find, NULL, NULL, 0, NULL},
    {"Find Again", 'G', act_edit_findagain, NULL, NULL, 0, NULL},
    {"Replace...", 'E', act_edit_replace, NULL, NULL, 0, NULL},
};

static const menu_item database_items[] = {
    {"Setup...", 'S', act_db_setup, dis_no_db, NULL, 0, NULL},
    {"Browse", 'B', act_db_browse, dis_no_db, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Append From...", 'A', act_db_append, NULL, NULL, 0, NULL},
    {"Copy To...", 'C', act_db_copy, dis_no_db, NULL, 0, NULL},
    {"Sort...", 'O', act_db_sort, dis_no_db, NULL, 0, NULL},
    {"Total...", 'T', act_db_total, dis_no_db, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Average...", 'V', act_db_avg, dis_no_db, NULL, 0, NULL},
    {"Count", 'N', act_db_count, dis_no_db, NULL, 0, NULL},
    {"Sum...", 'U', act_db_sum, dis_no_db, NULL, 0, NULL},
    {"Calculate...", 'K', act_db_calc, dis_no_db, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Report...", 'R', act_db_report, dis_no_db, NULL, 0, NULL},
    {"Label...", 'L', act_db_label, dis_no_db, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Pack", 'P', act_db_pack, dis_no_db, NULL, 0, NULL},
    {"Reindex", 'I', act_db_reindex, dis_no_db, NULL, 0, NULL},
};

static const menu_item record_items[] = {
    {"Append", 'A', act_rec_append, dis_no_db, NULL, 0, NULL},
    {"Change", 'C', act_rec_change, dis_no_db, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Goto...", 'G', act_rec_goto, dis_no_db, NULL, 0, NULL},
    {"Locate...", 'L', act_rec_locate, dis_no_db, NULL, 0, NULL},
    {"Continue", 'K', act_rec_cont, dis_no_db, NULL, 0, NULL},
    {"Seek...", 'S', act_rec_seek, dis_no_db, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Replace...", 'R', act_rec_replace, dis_no_db, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Delete...", 'D', act_rec_delete, dis_no_db, NULL, 0, NULL},
    {"Recall...", 'E', act_rec_recall, dis_no_db, NULL, 0, NULL},
};

static const menu_item program_items[] = {
    {"Do...", 'D', act_prog_do, NULL, NULL, 0, NULL},
    {"Cancel", 'C', act_prog_cancel, NULL, NULL, 0, NULL},
    {"Resume", 'R', act_prog_resume, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Compile...", 'P', act_prog_compile, NULL, NULL, 0, NULL},
    {"Generate...", 'G', act_prog_generate, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Filer", 'F', act_prog_filer, NULL, NULL, 0, NULL},
};

static const menu_item window_items[] = {
    {"Hide", 'H', act_win_hide, NULL, NULL, 0, NULL},
    {"Clear", 'C', act_win_clear, NULL, NULL, 0, NULL},
    {"Cycle", 'Y', act_win_cycle, NULL, NULL, 0, NULL},
    {"Command ^F2", 'O', act_win_command, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"View...", 'V', act_win_view, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"Zoom ^F10", 'Z', act_win_zoom, NULL, NULL, 0, NULL},
    {"Minimize ^F9", 'M', act_win_min, NULL, NULL, 0, NULL},
};

static const menu_item help_items[] = {
    {"Contents", 'C', act_help_contents, NULL, NULL, 0, NULL},
    {"Search for Help On...", 'S', act_help_search, NULL, NULL, 0, NULL},
    {"How to Use Help", 'H', act_help_how, NULL, NULL, 0, NULL},
    {NULL, 0, NULL, NULL, NULL, 1, NULL},
    {"About Reynard...", 'A', act_about, NULL, NULL, 0, NULL},
};

const menu_def rx_menus[] = {
    {"System", 'S', system_items, sizeof(system_items) / sizeof(system_items[0])},
    {"File", 'F', file_items, sizeof(file_items) / sizeof(file_items[0])},
    {"Edit", 'E', edit_items, sizeof(edit_items) / sizeof(edit_items[0])},
    {"Database", 'D', database_items, sizeof(database_items) / sizeof(database_items[0])},
    {"Record", 'R', record_items, sizeof(record_items) / sizeof(record_items[0])},
    {"Program", 'P', program_items, sizeof(program_items) / sizeof(program_items[0])},
    {"Window", 'W', window_items, sizeof(window_items) / sizeof(window_items[0])},
    {"Help", 'H', help_items, sizeof(help_items) / sizeof(help_items[0])},
};

const size_t rx_menus_n = sizeof(rx_menus) / sizeof(rx_menus[0]);

/* M9 9.5: dynamic app menu appended after the system menus (the Run pad) */
const menu_def* g_menus = rx_menus;
size_t g_menus_n;
static void init_gmenus(void) __attribute__((constructor));
static void init_gmenus(void) {
    g_menus_n = rx_menus_n;
}
static menu_def g_app_def;
static menu_item g_app_items[64];
static char g_app_label_buf[64][64];
static char g_app_cmd_buf[64][256];

void menudef_set_app(const char* title, int hotkey, const char* const* labels, const int* hks,
                     const char* const* cmds, const int* seps, size_t n) {
    if (n > 64) {
        n = 64;
    }
    g_app_def.title = title;
    g_app_def.hotkey = hotkey;
    g_app_def.items = g_app_items;
    g_app_def.nitems = n;
    for (size_t i = 0; i < n; i++) {
        snprintf(g_app_label_buf[i], sizeof(g_app_label_buf[i]), "%s", labels[i] ? labels[i] : "");
        snprintf(g_app_cmd_buf[i], sizeof(g_app_cmd_buf[i]), "%s", cmds[i] ? cmds[i] : "");
        g_app_items[i].label = g_app_label_buf[i];
        g_app_items[i].hotkey = hks[i];
        g_app_items[i].action = NULL;
        g_app_items[i].disabled = NULL;
        g_app_items[i].checked = NULL;
        g_app_items[i].sep = seps[i];
        g_app_items[i].cmd = g_app_cmd_buf[i];
    }
    static menu_def all[16];
    for (size_t i = 0; i < rx_menus_n && i < 15; i++) {
        all[i] = rx_menus[i];
    }
    all[rx_menus_n] = g_app_def;
    g_menus = all;
    g_menus_n = rx_menus_n + 1;
}

void menudef_clear_app(void) {
    g_menus = rx_menus;
    g_menus_n = rx_menus_n;
}
