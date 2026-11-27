/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file ui.h
 * @brief Public UI entry point (spec §6). main() calls rx_ui_run() for the
 *        interactive mode; the UI drives lang through rx_ctx hooks and the
 *        terminal only through the term_backend vtable.
 */
#ifndef REYNARD_UI_H
#define REYNARD_UI_H

#include "lang/lang.h"
#include "term/term_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run the interactive full-screen shell (spec §3.3, §6): menubar, window
 * stack, Command window, status line. Owns the backend lifecycle
 * (init/shutdown). Returns R_OK on clean exit (QUIT / event-stream EOF),
 * R_ERR_NOUI when the backend cannot initialize, else a runtime code.
 */
int rx_ui_run(rx_ctx* ctx, const term_backend* tb);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_UI_H */
