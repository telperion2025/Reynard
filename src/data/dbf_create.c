/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file dbf_create.c
 * @brief DBF creation (spec \u00a78.1 rx_dbf_create, \u00a78.2 header layout).
 *        Public rx_dbf_create writes a dBase III+ (0x03) file, or a FoxPro
 *        memo file (0xF5) when the layout has M/G/B/P fields, with the
 *        session code page mark. rx_dbf_create_full (internal) takes an
 *        explicit version/mark/date and is used by the round-trip suite.
 */
/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "data/data.h"
#include "util/util.h"

static void wr16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int valid_field(const rx_fielddef* f) {
    size_t n = strnlen(f->name, 10);
    if (n == 0) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = f->name[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return 0;
        }
        if (i == 0 && (c >= '0' && c <= '9')) {
            return 0; /* leading letter or _ */
        }
    }
    switch (f->type) {
    case RFT_C:
        return f->len >= 1 && f->len <= 254;
    case RFT_N:
    case RFT_F:
        return f->len >= 1 && f->len <= 20 && f->dec <= 15 && f->dec < f->len;
    case RFT_D:
    case RFT_L:
    case RFT_M:
    case RFT_G:
    case RFT_P:
    case RFT_I:
    case RFT_Y:
    case RFT_B:
    case RFT_T:
        return 1;
    default:
        return 0;
    }
}

static uint8_t forced_len(uint8_t type, uint8_t len) {
    switch (type) {
    case RFT_D:
        return 8;
    case RFT_L:
        return 1;
    case RFT_M:
    case RFT_G:
    case RFT_P:
        return 10;
    case RFT_I:
        return 4;
    case RFT_Y:
    case RFT_B:
    case RFT_T:
        return 8;
    default:
        return len;
    }
}

static int has_memo_field(const rx_fielddef* fields, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t t = fields[i].type;
        if (t == RFT_M || t == RFT_G || t == RFT_B || t == RFT_P) {
            return 1;
        }
    }
    return 0;
}

static uint8_t normalize_version(uint8_t version, int memo) {
    if (!memo) {
        return version == 0x30 || version == 0x31 || version == 0x32 || version == 0x04 ? version
                                                                                        : 0x03;
    }
    switch (version) {
    case 0x03:
        return 0x83;
    case 0x04:
        return 0x8B;
    case 0x83:
    case 0x8B:
    case 0xF5:
        return version; /* keep the caller's memo flavor */
    default:
        return 0xF5; /* FoxPro memo file */
    }
}

int rx_dbf_create_full(const char* path, const rx_fielddef* fields, size_t n, uint8_t version,
                       uint8_t cp_mark, int yy, int mm, int dd, rx_dbf** out) {
    if (!path || !fields || !out || n == 0 || n > 255) {
        return R_ERR_RANGE;
    }
    *out = NULL;
    for (size_t i = 0; i < n; i++) {
        if (!valid_field(&fields[i])) {
            return R_ERR_DBF_FIELDS;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(fields[i].name, fields[j].name) == 0) {
                return R_ERR_DBF_FIELDS; /* duplicate field name */
            }
        }
    }
    int memo = has_memo_field(fields, n);
    version = normalize_version(version, memo);

    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        return errno == EEXIST ? R_ERR_IO_PERM : R_ERR_IO_OPEN;
    }
    uint16_t header_size = (uint16_t)(32 + 32 * n + 1);
    uint16_t rec_size = 1;
    for (size_t i = 0; i < n; i++) {
        rec_size += forced_len(fields[i].type, fields[i].len);
    }
    uint8_t hdr[32];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = version;
    hdr[1] = (uint8_t)(yy - 1900);
    hdr[2] = (uint8_t)mm;
    hdr[3] = (uint8_t)dd;
    wr32le(hdr + 4, 0); /* nrec */
    wr16le(hdr + 8, header_size);
    wr16le(hdr + 10, rec_size);
    hdr[29] = cp_mark;
    if (write(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        close(fd);
        return R_ERR_IO_WRITE;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t d[32];
        memset(d, 0, sizeof(d));
        memcpy(d, fields[i].name, strnlen(fields[i].name, 10));
        d[11] = fields[i].type;
        d[16] = forced_len(fields[i].type, fields[i].len);
        d[17] = fields[i].dec;
        if (write(fd, d, sizeof(d)) != (ssize_t)sizeof(d)) {
            close(fd);
            return R_ERR_IO_WRITE;
        }
    }
    uint8_t term[2] = {0x0D, 0x1A};
    if (write(fd, term, sizeof(term)) != (ssize_t)sizeof(term)) {
        close(fd);
        return R_ERR_IO_WRITE;
    }
    close(fd);
    return rx_dbf_open(path, RD_EXCLUSIVE, out);
}

int rx_dbf_create(const char* path, const rx_fielddef* fields, size_t n, rx_dbf** out) {
    uint8_t version = has_memo_field(fields, n) ? 0xF5 : 0x03;
    int yy = 2000, mm = 1, dd = 1;
    if (!rx_data_test_mode()) {
        struct stat st;
        time_t t = 0;
        if (stat(".", &st) == 0) {
            t = st.st_mtime;
        }
        struct tm tmv;
        if (localtime_r(&t, &tmv)) {
            yy = tmv.tm_year + 1900;
            mm = tmv.tm_mon + 1;
            dd = tmv.tm_mday;
        }
    }
    return rx_dbf_create_full(path, fields, n, version, (uint8_t)rx_data_default_mark(), yy, mm, dd,
                              out);
}
