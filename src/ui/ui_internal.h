/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file ui_internal.h
 * @brief Shared internals of src/ui (spec §6). Not installed; only src/ui
 *        files include this. Layering: ui -> {lang, term, util}, never upward.
 */
#ifndef REYNARD_UI_INTERNAL_H
#define REYNARD_UI_INTERNAL_H

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>

#include "lang/lang.h"
#include "term/term_if.h"
#include "util/util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FoxPro 16-color palette indices in FoxPro/CGA numeric order (§5.2);
   attr = fg | bg<<4 into 64 pairs. Both term backends map these to curses
   colors through the same CGA->curses table. */
enum {
    RXC_BLACK = 0, RXC_BLUE,  RXC_GREEN, RXC_CYAN,  RXC_RED,   RXC_MAGENTA, RXC_YELLOW,
    RXC_WHITE,     RXC_GRAY,  RXC_BBLUE, RXC_BGREEN, RXC_BCYAN, RXC_BRED,   RXC_BMAGENTA,
    RXC_BYELLOW,   RXC_BWHITE
};
#define RXA(fg, bg) ((uint16_t)((((bg) & 15) << 4) | ((fg) & 15)))

/* FoxPro 2.x for DOS look (reference screenshots): blue desktop, silver
   menubar/pulldowns/status with black text and white hotkeys, bright-cyan
   active title / selected item, cyan Command window, silver dialogs.
   Window borders and the x close button are black on the window's own
   background so windows read as opaque boxes. */
#define RXA_DESKTOP RXA(RXC_WHITE, RXC_BLUE)
#define RXA_MENU RXA(RXC_BLACK, RXC_WHITE)
#define RXA_MENU_HOT RXA(RXC_BWHITE, RXC_WHITE)
#define RXA_MENU_OPEN RXA(RXC_BLACK, RXC_BCYAN)
#define RXA_PULL RXA(RXC_BLACK, RXC_WHITE)
#define RXA_PULL_SEL RXA(RXC_BLACK, RXC_BCYAN)
#define RXA_PULL_DIS RXA(RXC_GRAY, RXC_WHITE)
#define RXA_PULL_HOT RXA(RXC_BYELLOW, RXC_WHITE)
#define RXA_STATUS RXA(RXC_BLACK, RXC_WHITE)
#define RXA_MSG RXA(RXC_BRED, RXC_WHITE)
#define RXA_DLG RXA(RXC_BLACK, RXC_WHITE)
#define RXA_DLG_BORDER RXA(RXC_BLACK, RXC_WHITE)
#define RXA_DLG_TITLE RXA(RXC_BWHITE, RXC_BLUE)
#define RXA_CMD RXA(RXC_BLACK, RXC_BCYAN)
#define RXA_CMD_BORDER RXA(RXC_BLACK, RXC_BCYAN)
#define RXA_CMD_TITLE RXA(RXC_BLACK, RXC_BCYAN)

/* ============================ screen (screen.c) ============================ */

typedef struct rx_screen {
    rcell *cells;
    int w, h;
    struct rrect *dmg;
    size_t ndmg, capdmg;
    uint8_t pal[256][2]; /**< slot = fg | bg<<4 */
} rx_screen;

void scr_init(rx_screen *s, int w, int h);
void scr_free(rx_screen *s);
void scr_resize(rx_screen *s, int w, int h);
void scr_put(rx_screen *s, int x, int y, uint32_t ch, uint16_t attr);
void scr_text(rx_screen *s, int x, int y, const char *utf8, uint16_t attr);
void scr_textn(rx_screen *s, int x, int y, const char *utf8, size_t n, uint16_t attr);
void scr_fill(rx_screen *s, int x, int y, int w, int h, uint32_t ch, uint16_t attr);
void scr_hline(rx_screen *s, int x, int y, int w, uint32_t ch, uint16_t attr);
void scr_box(rx_screen *s, int x, int y, int w, int h, const char *title, uint16_t attr);
void scr_set_pair(rx_screen *s, int idx, uint8_t fg, uint8_t bg);
void scr_damage(rx_screen *s, int x, int y, int w, int h); /**< force damage rect */
int scr_blit(rx_screen *s, const term_backend *tb); /**< 1 if painted */

/* ============================ windows (window.c) ============================ */

struct rx_ui;
typedef struct rx_win rx_win;

struct rx_win {
    char title[64];
    int x, y, w, h; /**< outer rect including the border */
    int min_w, min_h;
    int visible;
    int want_close; /**< set by a window to ask the loop to remove+free it */
    rx_win *next; /**< stack link; the stack head is the front window */
    void (*render)(struct rx_ui *ui, rx_win *w);
    int (*key)(struct rx_ui *ui, rx_win *w, const rkey *k);     /**< 1 = consumed */
    int (*mouse)(struct rx_ui *ui, rx_win *w, const rmouse *m); /**< 1 = consumed */
    void (*free_fn)(rx_win *w); /**< heap windows only (dialogs) */
    void *ud;
};

#define WIN_ZONE_NONE 0
#define WIN_ZONE_MOVE 1 /**< border drag */
#define WIN_ZONE_SIZE 2 /**< corner drag */

void win_push(struct rx_ui *ui, rx_win *w);
void win_remove(struct rx_ui *ui, rx_win *w);
void win_bring_top(struct rx_ui *ui, rx_win *w);
rx_win *win_top(struct rx_ui *ui);
rx_win *win_at(struct rx_ui *ui, int x, int y);
int win_border_zone(const rx_win *w, int x, int y);
void win_box(struct rx_ui *ui, const rx_win *w, uint16_t border, uint16_t title_attr);
int win_close_hit(const rx_win *w, int x, int y); /**< title-bar × button */
void win_clamp(struct rx_ui *ui, rx_win *w);

/* ============================ getsay (getsay.c) ============================ */

#define GS_MAXHIST 1000

typedef struct getsay {
    char *buf;
    size_t len, cap;
    size_t cur; /**< cursor byte offset */
    char **hist;
    size_t nhist, caphist, hpos;
    size_t maxhist;
    char hist_path[300];
    int comp; /**< completion cycle state, -1 off */
    char comp_base[256];
} getsay;

void gs_init(getsay *g, size_t maxhist);
void gs_free(getsay *g);
void gs_clear(getsay *g);
void gs_insert_cp(getsay *g, uint32_t cp);
void gs_insert(getsay *g, const char *s, size_t n);
int gs_backspace(getsay *g);
int gs_delete(getsay *g);
void gs_left(getsay *g);
void gs_right(getsay *g);
void gs_home(getsay *g);
void gs_end(getsay *g);
void gs_kill_end(getsay *g);
int gs_hist_up(getsay *g);
int gs_hist_down(getsay *g);
void gs_hist_add(getsay *g, const char *line);
void gs_hist_path(getsay *g, char *out, size_t outsz);
void gs_hist_load(getsay *g);
void gs_hist_save(getsay *g);
void gs_reset_hist_nav(getsay *g); /**< back to the fresh line after exec/abort */
void gs_complete(getsay *g);
void gs_render(getsay *g, rx_screen *s, int x, int y, int w, uint16_t attr);

/* ============================ command window (cmdwin.c) ============================ */

#define CMDWIN_MAXLINES 5000

typedef struct rx_cmdwin {
    rx_win w;
    char **lines; /**< scrollback ring */
    size_t nlines, caplines, head;
    size_t maxlines;
    char pend[2048]; /**< partial line accumulator for out_hook */
    size_t npend;
    getsay ed;
    int drag; /**< WIN_ZONE_* while dragging */
    int ddx, ddy;
} rx_cmdwin;

rx_cmdwin *cmdwin_new(struct rx_ui *ui);
void cmdwin_free(rx_cmdwin *c);
void cmdwin_out(rx_cmdwin *c, const char *s, size_t n);
int cmdwin_out_hook(void *ud, const char *s, size_t n);
void cmdwin_layout(struct rx_ui *ui, rx_cmdwin *c);

/* ============================ menubar (menudef.c, menubar.c) ============================ */

typedef struct menu_item {
    const char *label;
    int hotkey; /**< uppercase letter or 0 */
    int (*action)(rx_ctx *ctx);
    int (*disabled)(rx_ctx *ctx);
    int (*checked)(rx_ctx *ctx);
    int sep; /**< separator line */
} menu_item;

typedef struct menu_def {
    const char *title;
    int hotkey;
    const menu_item *items;
    size_t nitems;
} menu_def;

extern const menu_def rx_menus[];
extern const size_t rx_menus_n;

void menubar_render(struct rx_ui *ui);
int menubar_key(struct rx_ui *ui, const rkey *k);
int menubar_mouse(struct rx_ui *ui, const rmouse *m);
void menubar_close(struct rx_ui *ui);
int menubar_title_at(const struct rx_ui *ui, int x); /**< row 0 hit, -1 none */
void menubar_pulldown_geom(const struct rx_ui *ui, int *x, int *y, int *w, int *h);

/* ============================ status line (statusbar.c) ============================ */

void statusbar_render(struct rx_ui *ui);

/* ============================ dialogs (dialog.c, helpwin.c) ============================ */

rx_win *dlg_new(struct rx_ui *ui, const char *title, int w, int h);
void dlg_add_line(rx_win *w, const char *line);
void dlg_set_editor(rx_win *w); /**< bottom row becomes a getsay input line */
void dlg_about(struct rx_ui *ui);
void dlg_macros(struct rx_ui *ui);
void dlg_filer(struct rx_ui *ui);
void dlg_calc(struct rx_ui *ui);
void dlg_calendar(struct rx_ui *ui);
void dlg_ascii(struct rx_ui *ui);
void dlg_help(struct rx_ui *ui);

/* ============================ ui state (ui.c) ============================ */

typedef struct rx_ui {
    rx_ctx *ctx;
    const term_backend *tb;
    rx_screen scr;
    rx_win *wins; /**< pane stack, head = front */
    rx_cmdwin *cmd;
    int menu_open;  /**< index into rx_menus, -1 closed */
    int menu_kb;    /**< menubar keyboard-active (F10/Alt) */
    int menu_sel;   /**< selected item in the open pulldown */
    rx_win *mouse_cap; /**< window that captured the button press (drag) */
    char msg[300];  /**< message line (status bar left) */
    int caps, num;  /**< status indicators */
    int paused;     /**< Ctrl+S pause */
    int dirty;
} rx_ui;

rx_ui *rx_ui_current(void);
void ui_set_msg(rx_ui *ui, const char *fmt, ...);
void ui_render(rx_ui *ui);
void ui_msgline_hook(void *ud, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_UI_INTERNAL_H */
