/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file helpwin.c
 * @brief Help window (spec §6.3 Help menu): a scrollable contents view on
 *        the pane stack. Full topic search lands with M9.
 */
#include "ui/ui_internal.h"

void dlg_help(struct rx_ui *ui)
{
    static const char *const lines[] = {
        "Reynard Help — Contents",
        "",
        "  System    About, Macros, Filer, Calculator, Calendar, ASCII, Help",
        "  File      New, Open, Close, Save, Print, Quit",
        "  Edit      Undo, Cut, Copy, Paste, Find, Replace (Command window)",
        "  Database  Setup, Browse, Append/Copy/Sort, Pack, Reindex",
        "  Record    Append, Change, Goto, Locate, Replace, Delete, Recall",
        "  Program   Do, Compile, Generate",
        "  Window    Hide, Clear, Cycle, Command (Ctrl+F2), Zoom",
        "",
        "  Keys: F10 or Alt activates the menubar; arrows navigate;",
        "        Enter selects; Esc backs out. In the Command window:",
        "        Ctrl+Up/Down history, Tab completion, Ctrl+Y kill-to-end,",
        "        Ctrl+S pause, Esc abort (SET ESCAPE).",
        "",
        "  Batch mode: reynard script.prg | reynard -c \"? 1+1\" | reynard -",
        "",
        "Press Esc to close.",
    };
    rx_win *w = dlg_new(ui, "Help", 64, 18);
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        dlg_add_line(w, lines[i]);
    }
}
