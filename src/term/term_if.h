/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file term_if.h
 * @brief Terminal backend vtable (spec §5.1). The UI talks only to this
 *        interface; production is term_ncurses.c, CI determinism is
 *        term_script.c.
 */
#ifndef REYNARD_TERM_IF_H
#define REYNARD_TERM_IF_H

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>

#include "term/keymap.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One virtual cell: Unicode codepoint + palette pair index (§5.2). */
typedef struct rcell {
    uint32_t ch;   /**< Unicode codepoint */
    uint16_t attr; /**< palette pair index (§5.2) */
} rcell;

/** Rectangle in cell coordinates. */
struct rrect {
    uint16_t x, y, w, h;
};

typedef enum { EV_KEY, EV_MOUSE, EV_RESIZE, EV_EOF, EV_ERR } ev_kind;

typedef struct rkey {
    int sym;      /**< RXK_* from keymap.h */
    uint32_t cp;  /**< codepoint if printable, else 0 */
    uint8_t mods; /**< SHIFT, CTRL, ALT bits */
} rkey;

#define RM_PRESS 1
#define RM_RELEASE 2
#define RM_DRAG 3
#define RM_WHEEL_UP 4
#define RM_WHEEL_DN 5
#define RM_MOVE 6

typedef struct rmouse {
    uint16_t x, y; /**< 0-based cell coordinates */
    uint8_t kind;  /**< RM_* */
    uint8_t button; /**< 1..5 */
    uint8_t dc;    /**< 1 if double-click (400ms window, configurable) */
} rmouse;

typedef struct revent {
    ev_kind kind;
    union {
        rkey k;
        rmouse m;
    } u;
} revent;

typedef struct term_backend {
    int (*init)(void);
    void (*shutdown)(void);
    int (*blit)(rcell *cells, size_t w, size_t h, struct rrect *damage, size_t n_dmg);
    int (*palette)(void (*set_pair)(int idx, uint8_t fg, uint8_t bg));
    int (*poll)(revent *out, uint32_t timeout_ms); /**< nonblocking */
    void (*bell)(void);
    void (*cbreak_on)(void);
    void (*cbreak_off)(void);
    const char *name;
} term_backend;

/** Script (event-replay) backend; NULL init failure if RX_EVENTS unset. */
const term_backend *rx_term_script(void);
/** ncurses production backend; NULL when built without ncurses. */
const term_backend *rx_term_ncurses(void);
/** Diagnostic: number of blit calls since script backend init. */
unsigned long rx_term_script_blit_count(void);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_TERM_IF_H */
