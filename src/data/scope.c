/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file scope.c
 * @brief Positional scope resolution (spec \u00a78.7): ALL / REST / NEXT n /
 *        RECORD n as an inclusive record range against the current pointer.
 *        FOR/WHILE/FILTER evaluation happens in the lang layer (it owns the
 *        VM); this module is the pure positional half.
 */
#include "data/data.h"

int rx_scope_range(const rx_dbf* dbf, const rx_scope* sc, uint32_t* start, uint32_t* end,
                   int* restore) {
    if (!dbf || !sc || !start || !end) {
        return R_ERR_RANGE;
    }
    uint32_t nrec = dbf->nrec;
    uint32_t cur = dbf->eof ? nrec + 1 : (dbf->recno ? dbf->recno : 1);
    *restore = 0;
    switch (sc->kind) {
    case 1: /* ALL */
        *start = 1;
        *end = nrec;
        break;
    case 2: /* REST */
        *start = cur;
        *end = nrec;
        break;
    case 3: { /* NEXT n */
        uint32_t n = sc->n;
        *start = cur;
        uint64_t e = (uint64_t)cur + n - 1;
        *end = e > nrec ? nrec : (uint32_t)e;
        if (cur > nrec) {
            *start = nrec + 1;
            *end = nrec; /* empty range */
        }
        break;
    }
    case 4: { /* RECORD n */
        *restore = 1;
        if (sc->n >= 1 && sc->n <= nrec) {
            *start = sc->n;
            *end = sc->n;
        } else {
            *start = 1;
            *end = 0; /* out of range: empty */
        }
        break;
    }
    default: /* none: current record only */
        *start = cur;
        *end = cur > nrec ? nrec : cur;
        if (cur > nrec) {
            *end = nrec;
            if (*start > *end) {
                *start = *end + 1;
            }
        }
        break;
    }
    if (*start > *end) {
        *start = *end + 1; /* empty range */
    }
    return R_OK;
}
