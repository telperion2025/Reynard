/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file term_ncurses.c
 * @brief Production ncurses backend (spec §5). Built only when ncurses
 *        headers are available (CMake RX_HAVE_NCURSES); otherwise
 *        rx_term_ncurses() returns NULL and only the script backend exists.
 *
 * Mouse: SGR 1006 is enabled first (spec §5.4 tier 1); if the terminal
 * answers with a legacy 1000 burst the parser pins to that mode for the
 * session. GPM/sysmouse are probed by mouse.c callers on their consoles.
 */
/* Wide-curses API (cchar_t/setcchar) needs _XOPEN_SOURCE_EXTENDED and must
   be requested before any system header. */
#if defined(RX_HAVE_NCURSES) && !defined(_XOPEN_SOURCE_EXTENDED)
#define _XOPEN_SOURCE_EXTENDED 1
#endif

#include "term/term_if.h"

#if defined(RX_HAVE_NCURSES)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HAVE_NCURSESW_CURSES_H)
#include <ncursesw/curses.h>
#elif defined(HAVE_NCURSES_CURSES_H)
#include <ncurses/curses.h>
#else
#include <curses.h>
#endif

#include "term/mouse.h"
#include "util/util.h"

static int nc_init(void)
{
    if (!getenv("TERM") || !*getenv("TERM")) {
        return R_ERR_NOUI;
    }
    WINDOW *w = initscr();
    if (!w) {
        return R_ERR_NOUI;
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nonl();
    /* Mouse: request SGR 1006 + 1002 (button+drag) + 1003 fallback handled
       by the parser; disable any default reporting first. */
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    printf("\x1B[?1006h\x1B[?1002h\x1B[?1003h");
    fflush(stdout);
    if (has_colors()) {
        start_color();
    }
    return R_OK;
}

static void nc_shutdown(void)
{
    printf("\x1B[?1003l\x1B[?1002l\x1B[?1006l");
    fflush(stdout);
    mousemask(0, NULL);
    endwin();
}

static int nc_blit(rcell *cells, size_t w, size_t h, struct rrect *damage, size_t n_dmg)
{
    size_t total = w * h;
    size_t dmg_area = 0;
    for (size_t i = 0; i < n_dmg; i++) {
        dmg_area += (size_t)damage[i].w * damage[i].h;
    }
    int full = n_dmg == 0 || dmg_area * 10 >= total * 3; /* >30% -> full paint */
    if (full) {
        for (size_t y = 0; y < h; y++) {
            for (size_t x = 0; x < w; x++) {
                rcell c = cells[y * w + x];
                uint32_t ch = c.ch ? c.ch : ' ';
                attr_t a = COLOR_PAIR(c.attr & 0x3F);
                if (ch < 0x80) {
                    mvaddch((int)y, (int)x, (chtype)(a | (uint8_t)ch));
                } else {
#if defined(RX_HAVE_NCURSESW_H)
                    cchar_t cc;
                    wchar_t wch[2] = { (wchar_t)ch, 0 };
                    setcchar(&cc, wch, a, (short)(c.attr & 0x3F), NULL);
                    mvadd_wch((int)y, (int)x, &cc);
#else
                    char tmp[5];
                    size_t n = utf8_encode(ch, tmp);
                    tmp[n] = '\0';
                    mvaddstr((int)y, (int)x, tmp);
#endif
                }
            }
        }
    } else {
        for (size_t i = 0; i < n_dmg; i++) {
            for (uint16_t y = damage[i].y; y < damage[i].y + damage[i].h && y < h; y++) {
                for (uint16_t x = damage[i].x; x < damage[i].x + damage[i].w && x < w; x++) {
                    rcell c = cells[(size_t)y * w + x];
                    uint32_t ch = c.ch ? c.ch : ' ';
                    attr_t a = COLOR_PAIR(c.attr & 0x3F);
                    if (ch < 0x80) {
                        mvaddch((int)y, (int)x, (chtype)(a | (uint8_t)ch));
                    } else {
                        char tmp[5];
                        size_t n = utf8_encode(ch, tmp);
                        tmp[n] = '\0';
                        mvaddstr((int)y, (int)x, tmp);
                    }
                }
            }
        }
    }
    refresh();
    return R_OK;
}

static int nc_palette(void (*set_pair)(int idx, uint8_t fg, uint8_t bg))
{
    if (!has_colors()) {
        return R_OK;
    }
    static const short ansi16[16] = { COLOR_BLACK,   COLOR_RED,     COLOR_GREEN, COLOR_YELLOW,
                                       COLOR_BLUE,    COLOR_MAGENTA, COLOR_CYAN,  COLOR_WHITE,
                                       COLOR_BLACK + 8, COLOR_RED + 8, COLOR_GREEN + 8, COLOR_YELLOW + 8,
                                       COLOR_BLUE + 8, COLOR_MAGENTA + 8, COLOR_CYAN + 8, COLOR_WHITE + 8 };
    for (int i = 0; i < 64; i++) {
        short fg = ansi16[i % 16];
        short bg = ansi16[(i / 16) % 16];
        init_pair((short)(i + 1), fg, bg);
        set_pair(i, (uint8_t)fg, (uint8_t)bg);
    }
    return R_OK;
}

static int nc_poll(revent *out, uint32_t timeout_ms)
{
    timeout((int)timeout_ms);
    int ch = getch();
    if (ch == ERR) {
        return R_OK; /* timeout: caller polls again */
    }
    if (ch == KEY_MOUSE) {
        MEVENT ev;
        if (getmouse(&ev) == OK) {
            out->kind = EV_MOUSE;
            out->u.m.x = (uint16_t)ev.x;
            out->u.m.y = (uint16_t)ev.y;
            out->u.m.button = 1;
            out->u.m.dc = 0;
            if (ev.bstate & BUTTON1_PRESSED) {
                out->u.m.kind = RM_PRESS;
            } else if (ev.bstate & BUTTON1_RELEASED) {
                out->u.m.kind = RM_RELEASE;
            } else if (ev.bstate & BUTTON1_CLICKED) {
                out->u.m.kind = RM_PRESS;
                out->u.m.dc = 1;
            } else if (ev.bstate & BUTTON4_PRESSED) {
                out->u.m.kind = RM_WHEEL_UP;
            } else if (
#ifdef BUTTON5_PRESSED
                (ev.bstate & BUTTON5_PRESSED) ||
#endif
                (ev.bstate & BUTTON2_PRESSED)) {
                out->u.m.kind = RM_WHEEL_DN;
            } else {
                out->u.m.kind = RM_MOVE;
            }
            return R_OK;
        }
        return R_OK;
    }
    if (ch == KEY_RESIZE) {
        out->kind = EV_RESIZE;
        out->u.m.x = (uint16_t)COLS;
        out->u.m.y = (uint16_t)LINES;
        return R_OK;
    }
    out->kind = EV_KEY;
    memset(&out->u.k, 0, sizeof(out->u.k));
    switch (ch) {
    case '\n':
    case '\r':
    case KEY_ENTER:
        out->u.k.sym = RXK_ENTER;
        break;
    case 27:
        out->u.k.sym = RXK_ESC;
        break;
    case '\t':
        out->u.k.sym = RXK_TAB;
        break;
    case KEY_BTAB:
        out->u.k.sym = RXK_BACKTAB;
        break;
    case KEY_BACKSPACE:
    case 127:
    case 8:
        out->u.k.sym = RXK_BS;
        break;
    case KEY_UP:
        out->u.k.sym = RXK_UP;
        break;
    case KEY_DOWN:
        out->u.k.sym = RXK_DOWN;
        break;
    case KEY_LEFT:
        out->u.k.sym = RXK_LEFT;
        break;
    case KEY_RIGHT:
        out->u.k.sym = RXK_RIGHT;
        break;
    case KEY_HOME:
        out->u.k.sym = RXK_HOME;
        break;
    case KEY_END:
        out->u.k.sym = RXK_END;
        break;
    case KEY_PPAGE:
        out->u.k.sym = RXK_PGUP;
        break;
    case KEY_NPAGE:
        out->u.k.sym = RXK_PGDN;
        break;
    case KEY_IC:
        out->u.k.sym = RXK_INS;
        break;
    case KEY_DC:
        out->u.k.sym = RXK_DEL;
        break;
    default:
        if (ch >= KEY_F(1) && ch <= KEY_F(12)) {
            out->u.k.sym = RXK_F1 + (ch - KEY_F(1));
        } else if (ch >= 1 && ch <= 26) {
            out->u.k.sym = RXK_CTL('A' + ch - 1);
        } else if (ch >= 32 && ch < 127) {
            out->u.k.sym = RXK_BASE;
            out->u.k.cp = (uint32_t)ch;
        } else if (ch >= 128) {
            out->u.k.sym = RXK_BASE;
            out->u.k.cp = (uint32_t)ch;
        } else {
            out->u.k.sym = RXK_NONE;
        }
        break;
    }
    return R_OK;
}

static void nc_bell(void)
{
    beep();
}

static void nc_cbreak_on(void)
{
    cbreak();
}

static void nc_cbreak_off(void)
{
    nocbreak();
}

static const term_backend ncurses_backend = {
    .init = nc_init,
    .shutdown = nc_shutdown,
    .blit = nc_blit,
    .palette = nc_palette,
    .poll = nc_poll,
    .bell = nc_bell,
    .cbreak_on = nc_cbreak_on,
    .cbreak_off = nc_cbreak_off,
    .name = "ncurses",
};

const term_backend *rx_term_ncurses(void)
{
    return &ncurses_backend;
}

#else /* !RX_HAVE_NCURSES */

const term_backend *rx_term_ncurses(void)
{
    return NULL;
}

#endif /* RX_HAVE_NCURSES */
