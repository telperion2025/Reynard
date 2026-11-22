/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/** @file mouse.h — protocol normalizer interface (spec §5.4). */
#ifndef REYNARD_MOUSE_H
#define REYNARD_MOUSE_H

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>

#include "term/term_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode one mouse event from a byte stream.
 * @return 1 event decoded, 0 need more bytes, -1 malformed.
 */
int mouse_decode(const uint8_t *buf, size_t n, rmouse *out, size_t *used);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_MOUSE_H */
