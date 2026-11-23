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

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(RX_HAVE_NCURSESW_H) || defined(HAVE_NCURSESW_CURSES_H)
#include <ncursesw/curses.h>
#elif defined(RX_HAVE_NCURSESW_FLAT)
#include <curses.h> /* flat ncurses 6 header (MacPorts/BSD) + libncursesw */
#elif defined(RX_HAVE_NCURSES_H) || defined(HAVE_NCURSES_CURSES_H)
#include <ncurses/curses.h>
#else
#include <curses.h>
#endif

#include "term/mouse.h"
#include "util/util.h"

/* Write terminal-mode sequences with write(2), never printf: stdio
   buffering interleaves with curses' own output and splits escape
   sequences, which prints garbage like "?1002h" onto the screen. */
static void raw_write(const char *s)
{
    (void)write(STDOUT_FILENO, s, strlen(s));
}

static void nc_sync_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        if ((int)ws.ws_col != COLS || (int)ws.ws_row != LINES) {
            resizeterm((int)ws.ws_row, (int)ws.ws_col);
        }
    }
}

static int nc_init(void)
{
    if (!getenv("TERM") || !*getenv("TERM")) {
        return R_ERR_NOUI;
    }
    setlocale(LC_ALL, "");
    WINDOW *w = initscr();
    if (!w) {
        return R_ERR_NOUI;
    }
    nc_sync_size(); /* make curses' stdscr match the real terminal geometry */
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nonl();
    int no_mouse = getenv("REYNARD_NO_MOUSE") != NULL;
    if (!no_mouse) {
        mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    }
    if (has_colors()) {
        start_color();
    }
    /* ncurses emits its own XM mouse modes on the first refresh; emit ours
       after it so they actually stick (write(2), never printf, so the bytes
       cannot interleave with curses output). Enable every protocol tier
       (spec §5.4): SGR 1006, button-drag 1002, any-motion 1003, legacy
       1000, urxvt 1015 — the parser pins to whatever arrives first. */
    refresh();
    if (!no_mouse) {
        raw_write("\x1B[?1000h\x1B[?1002h\x1B[?1003h\x1B[?1015h\x1B[?1006h");
    }
    return R_OK;
}

static void nc_shutdown(void)
{
    if (!getenv("REYNARD_NO_MOUSE")) {
        mousemask(0, NULL);
    }
    endwin();
    /* after endwin(): curses is done writing, so this cannot interleave */
    raw_write("\x1B[?1006l\x1B[?1015l\x1B[?1003l\x1B[?1002l\x1B[?1000l");
}

#if !defined(RX_HAVE_NCURSESW_H) && !defined(RX_HAVE_NCURSESW_FLAT)
/* narrow ncurses (e.g. macOS system 5.7, no ncursesw): draw line-drawing
   via the alternate charset and transliterate the few other glyphs we use */
static int nc_acs_char(uint32_t ch)
{
    switch (ch) {
    case 0x2500: return ACS_HLINE;
    case 0x2502: return ACS_VLINE;
    case 0x250C: return ACS_ULCORNER;
    case 0x2510: return ACS_URCORNER;
    case 0x2514: return ACS_LLCORNER;
    case 0x2518: return ACS_LRCORNER;
    case 0x251C: return ACS_LTEE;
    case 0x2524: return ACS_RTEE;
    case 0x252C: return ACS_TTEE;
    case 0x2534: return ACS_BTEE;
    case 0x253C: return ACS_PLUS;
    default: return 0;
    }
}

static char ascii_fallback(uint32_t ch)
{
    switch (ch) {
    case 0x2013: /* – */
    case 0x2014: /* — */
        return '-';
    case 0x00D7: /* × close button */
        return 'x';
    case 0x221A: /* √ checkmark */
        return 'v';
    case 0x2026: /* … */
        return '.';
    default:
        return '?';
    }
}
#endif /* !RX_HAVE_NCURSESW_H */

static void nc_draw(int y, int x, rcell c)
{
    uint32_t ch = c.ch ? c.ch : ' ';
    short pair = (short)((c.attr & 0xFF) + 1); /* curses pairs are 1-based */
    attr_t a = COLOR_PAIR(pair);
#if defined(RX_HAVE_NCURSESW_H) || defined(RX_HAVE_NCURSESW_FLAT)
    if (ch < 0x80) {
        mvaddch(y, x, (chtype)(a | (uint8_t)ch));
    } else {
        cchar_t cc;
        wchar_t wch[2] = { (wchar_t)ch, 0 };
        setcchar(&cc, wch, a, pair, NULL);
        mvadd_wch(y, x, &cc);
    }
#else
    int ac = nc_acs_char(ch);
    if (ac) {
        attron(A_ALTCHARSET);
        mvaddch(y, x, (chtype)(a | ac));
        attroff(A_ALTCHARSET);
    } else if (ch < 0x80) {
        mvaddch(y, x, (chtype)(a | (uint8_t)ch));
    } else {
        mvaddch(y, x, (chtype)(a | (uint8_t)ascii_fallback(ch)));
    }
#endif
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
                nc_draw((int)y, (int)x, cells[y * w + x]);
            }
        }
    } else {
        for (size_t i = 0; i < n_dmg; i++) {
            for (uint16_t y = damage[i].y; y < damage[i].y + damage[i].h && y < h; y++) {
                for (uint16_t x = damage[i].x; x < damage[i].x + damage[i].w && x < w; x++) {
                    nc_draw((int)y, (int)x, cells[(size_t)y * w + x]);
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
    /* attr = fg | bg<<4 (FoxPro/CGA palette indices); map to curses colors
       with the same table term_script.c uses so both backends agree.
       Curses pairs are 1-based, so slot i gets pair i+1. */
    static const short cga2curses[16] = { COLOR_BLACK,       COLOR_BLUE,  COLOR_GREEN,  COLOR_CYAN,
                                          COLOR_RED,         COLOR_MAGENTA, COLOR_YELLOW, COLOR_WHITE,
                                          COLOR_BLACK + 8,   COLOR_BLUE + 8, COLOR_GREEN + 8, COLOR_CYAN + 8,
                                          COLOR_RED + 8,     COLOR_MAGENTA + 8, COLOR_YELLOW + 8, COLOR_WHITE + 8 };
    for (int i = 0; i < 256; i++) {
        if (i + 1 >= COLOR_PAIRS) {
            break; /* pair 0 is reserved; some terminals cap at 256 pairs */
        }
        short fg = cga2curses[i % 16];
        short bg = cga2curses[(i / 16) % 16];
        init_pair((short)(i + 1), fg, bg);
        set_pair(i, (uint8_t)fg, (uint8_t)bg);
    }
    return R_OK;
}

static int nc_btn_down = 0; /**< button-1 held: press repeats become drags */

static int nc_poll(revent *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof(*out));
    timeout((int)timeout_ms);
    int ch = getch();
    if (ch == ERR) {
        out->kind = EV_ERR; /* timeout: no event, caller polls again */
        return R_OK;
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
                /* with button-event tracking (1002) a drag arrives as a
                   repeated BUTTON1_PRESSED; translate to RM_DRAG */
                out->u.m.kind = nc_btn_down ? RM_DRAG : RM_PRESS;
                nc_btn_down = 1;
            } else if (ev.bstate & BUTTON1_RELEASED) {
                out->u.m.kind = RM_RELEASE;
                nc_btn_down = 0;
            } else if (ev.bstate & BUTTON1_CLICKED) {
                out->u.m.kind = RM_PRESS;
                out->u.m.dc = 1;
                nc_btn_down = 0;
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
        nc_sync_size();
        out->kind = EV_RESIZE;
        out->u.m.x = (uint16_t)COLS;
        out->u.m.y = (uint16_t)LINES;
        return R_OK;
    }
    if (ch == 27) {
        /* Brute-force mouse: a raw SGR/legacy sequence that terminfo's XM
           doesn't know arrives as ESC + bytes. Grab the rest nonblocking
           and run the mouse.c decoder; on failure push everything back. */
        uint8_t buf[40];
        size_t n = 0;
        buf[n++] = 27;
        nodelay(stdscr, TRUE);
        int c2;
        while (n < sizeof(buf) && (c2 = getch()) != ERR) {
            buf[n++] = (uint8_t)c2;
        }
        nodelay(stdscr, FALSE);
        timeout((int)timeout_ms);
        rmouse m;
        size_t used = 0;
        int r = n > 1 ? mouse_decode(buf, n, &m, &used) : -1;
        if (r == 1 && used >= 4 && used <= n) {
            out->kind = EV_MOUSE;
            out->u.m = m;
            return R_OK;
        }
        for (size_t i = n; i-- > 1;) {
            ungetch(buf[i]);
        }
        out->kind = EV_KEY;
        out->u.k.sym = RXK_ESC;
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

static void nc_size(int *w, int *h)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *w = (int)ws.ws_col;
        *h = (int)ws.ws_row;
    } else {
        *w = COLS;
        *h = LINES;
    }
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
    .size = nc_size,
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
