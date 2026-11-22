/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file fuzz_dbf.c
 * @brief libFuzzer harness for rx_dbf_open (spec §12): every found crash
 *        becomes a unit test at the exact input before the fix.
 *        Seed corpus: testdata/gen/fuzz-seeds/.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "reynard/dbf.h"
#include "reynard/status.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* write to a temp file and open read-only; never trust the bytes */
    char path[] = "/tmp/rx-fuzz-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return 0;
    }
    if (write(fd, data, size) != (ssize_t)size) {
        close(fd);
        unlink(path);
        return 0;
    }
    close(fd);
    rx_dbf *dbf = NULL;
    int rc = rx_dbf_open(path, RD_READONLY, &dbf);
    if (rc == R_OK) {
        uint32_t nrec = 0, recno = 0;
        size_t nf = 0;
        rx_dbf_recount(dbf, &nrec);
        rx_dbf_field_count(dbf, &nf);
        rx_dbf_goto(dbf, 1);
        for (uint32_t i = 0; i < nrec && i < 64; i++) {
            rx_dbf_recno(dbf, &recno);
            for (size_t f = 0; f < nf; f++) {
                rval v;
                rx_rval_undef(&v);
                if (rx_dbf_get(dbf, f, &v) == R_OK) {
                    rx_rval_free(&v);
                }
            }
            rx_dbf_skip(dbf, 1);
        }
        rx_dbf_close(dbf);
    }
    unlink(path);
    return 0;
}
