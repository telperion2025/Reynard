/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file keymap.h
 * @brief Logical key symbols (spec §5.3). RXK_F1..Z style symbols plus
 *        RXK_CTL(c)/RXK_ALT(c) canonical translations for Ctrl/Alt+letter.
 */
#ifndef REYNARD_KEYMAP_H
#define REYNARD_KEYMAP_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RXK_NONE = 0,
    RXK_ENTER,
    RXK_ESC,
    RXK_TAB,
    RXK_BACKTAB,
    RXK_BS, /**< backspace */
    RXK_UP,
    RXK_DOWN,
    RXK_LEFT,
    RXK_RIGHT,
    RXK_HOME,
    RXK_END,
    RXK_PGUP,
    RXK_PGDN,
    RXK_INS,
    RXK_DEL,
    RXK_F1,
    RXK_F2,
    RXK_F3,
    RXK_F4,
    RXK_F5,
    RXK_F6,
    RXK_F7,
    RXK_F8,
    RXK_F9,
    RXK_F10,
    RXK_F11,
    RXK_F12,
    RXK_RESIZE = 0x100, /**< synthetic: window resized (EV_RESIZE uses kind) */
    RXK_BASE = 0x200    /**< printable/special keys start here */
};

#define RXK_CTL(c) (RXK_BASE + 0x100 + ((c) & 0x1F)) /**< Ctrl+A..Z canonical */
#define RXK_ALT(c) (RXK_BASE + 0x200 + (unsigned char)(c))

#define RXK_MOD_SHIFT 0x01
#define RXK_MOD_CTRL 0x02
#define RXK_MOD_ALT 0x04

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_KEYMAP_H */
