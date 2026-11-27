/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file status.h
 * @brief Error code registry (spec Appendix D) and the structured error type.
 *
 * Every fallible public function returns one of these codes. R_OK (0) is
 * success; all other values are positive and grouped by subsystem:
 * IO -1xx, DBF -2xx, CDX -3xx, Lock -4xx, Parse -5xx, Runtime -6xx,
 * OS -7xx. Internal assertion failures abort with exit code 4.
 */
#ifndef REYNARD_STATUS_H
#define REYNARD_STATUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* <unistd.h> defines R_OK for access(2); the Appendix D registry owns the
   name here, so undefine the POSIX one (callers use F_OK for access). */
#ifdef R_OK
#undef R_OK
#endif
#define R_OK 0

/* IO -1xx: open/close/read/write/seek failures */
#define R_ERR_IO_OPEN 101
#define R_ERR_IO_CLOSE 102
#define R_ERR_IO_READ 103
#define R_ERR_IO_WRITE 104
#define R_ERR_IO_SEEK 105
#define R_ERR_IO_EOF 106
#define R_ERR_IO_NOTFOUND 107
#define R_ERR_IO_PERM 108

/* DBF -2xx: bad magic/header/field layout, bad record offset, memo, code page */
#define R_ERR_DBF_MAGIC 201
#define R_ERR_DBF_HEADER 202
#define R_ERR_DBF_FIELDS 203
#define R_ERR_DBF_RECOFF 204
#define R_ERR_DBF_MEMO 205
#define R_ERR_DBF_CODEPAGE 206
#define R_ERR_DBF_BADRXP 207
#define R_ERR_DBF_TRUNCATED 208
#define R_ERR_DBF_UNSUPPORTED 209
#define R_ERR_DBF_NOTOPEN 210
#define R_ERR_DBF_RECNO 211

/* CDX -3xx: corrupt node, tag not found, incompatible variant */
#define R_ERR_CDX_NODE 301
#define R_ERR_CDX_TAG 302
#define R_ERR_CDX_VARIANT 303

/* Lock -4xx */
#define R_ERR_LOCK_CONFLICT 401
#define R_ERR_FILE_IN_USE 402
#define R_ERR_LOCK_PERM 403

/* Parse -5xx */
#define R_ERR_SYNTAX 501
#define R_ERR_VAR 502
#define R_ERR_FIELD 503
#define R_ERR_TYPE 504
#define R_ERR_DIVZERO 505
#define R_ERR_MACRO_DEPTH 506
#define R_ERR_UNDEF_FUNC 507
#define R_ERR_STACK 508
#define R_ERR_RANGE 509

/* Runtime -6xx */
#define R_ERR_NOUI 601
#define R_ERR_INTERACTIVE_ONLY 602
#define R_ERR_EMPTY 603
#define R_ERR_STR_OVERFLOW 604
#define R_ERR_DATE_RANGE 605
#define R_ERR_NO_CONTINUE 606
#define R_ERR_BROWSE 607

/* OS -7xx: filesystem errors surfaced with errno captured by the caller */
#define R_ERR_OS 700

/** Structured error record carried through the interpreter (spec §7.4). */
typedef struct r_err {
    int code;           /**< Appendix D code */
    int line;           /**< 1-based source line, 0 if not applicable */
    int col;            /**< 1-based source column, 0 if not applicable */
    char src_file[260]; /**< source file name, empty for ad-hoc input */
    char msg[512];      /**< human-readable message (English catalog, v1.0) */
} r_err;

/** Return the short catalog string for an Appendix D code. */
const char* rx_status_str(int code);

/** Fill @p msg with the catalog string for @p code (never NULL). */
void rx_status_message(int code, char* msg, size_t msgsz);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_STATUS_H */
