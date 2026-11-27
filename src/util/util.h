/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file util.h
 * @brief Shared utilities: dynamic buffers, strings, UTF-8, code pages,
 *        numeric formatting, error catalog, logging, config files.
 *        util is the bottom layer: it includes nothing from the project.
 */
#ifndef REYNARD_UTIL_H
#define REYNARD_UTIL_H

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "reynard/rval.h"
#include "reynard/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- strbuf.c: growable byte buffer ---- */
typedef struct strbuf {
    char* buf;
    size_t len;
    size_t cap;
} strbuf;

void sb_init(strbuf* sb);
void sb_free(strbuf* sb);
int sb_putc(strbuf* sb, char c);
int sb_put(strbuf* sb, const char* s, size_t n);
int sb_puts(strbuf* sb, const char* s);
int sb_printf(strbuf* sb, const char* fmt, ...);
int sb_putcp(strbuf* sb, uint32_t cp); /* encode UTF-8 */

/* ---- str.c: r_str and C-string helpers ---- */
/** Duplicate an r_str into a freshly allocated one (utf8 owned). */
int rstr_dup(r_str* dst, const char* utf8, size_t n);
/** Compare two r_str byte-wise. */
int rstr_cmp(const r_str* a, const r_str* b);
/** Build an rval string value taking ownership of utf8 (may be NULL). */
void rval_set_str(rval* v, char* utf8, size_t n);
/** Case-insensitive ASCII compare of NUL-terminated strings. */
int rx_strcasecmp(const char* a, const char* b);
/* FoxPro LIKE/EXCEPT wildcard match: * and ? (M5) */
int rx_wildmatch(const char* pat, const char* str);
/** Uppercase ASCII in place. */
void rx_strtoupper(char* s);
/** Duplicate a C string (NULL-safe). */
char* rx_strdup(const char* s);
/** Portable mkdtemp(3) replacement (macOS hides mkdtemp under strict
 *  _POSIX_C_SOURCE): replaces the trailing XXXXXX with pid+counter. */
int rx_mkdtemp(char* tmpl);
/** Portable mkstemp(3) replacement; see rx_mkdtemp. */
int rx_mkstemp(char* tmpl);
/** Truncate an open file (util owns the platform call, spec \u00a72.2). */
int rx_ftruncate(int fd, long len);
/** Remove a file (util owns the platform call, spec \u00a72.2). */
int rx_unlink(const char* path);

/* ---- utf8.c ---- */
/** Decode one codepoint at s; returns bytes consumed (1..4), *cp set. */
size_t utf8_decode(const char* s, size_t n, uint32_t* cp);
/** Encode cp into out (needs >= 4 bytes); returns bytes written. */
size_t utf8_encode(uint32_t cp, char* out);
/** Number of codepoints in a UTF-8 string. */
size_t utf8_length(const char* s, size_t n);
/** Display width: 1 per codepoint, 0 for combining marks (minimal table). */
int utf8_cp_width(uint32_t cp);

/* ---- cp_tables.c: single-byte code pages <-> UTF-8 ---- */
#define RX_CP437 437
#define RX_CP850 850
#define RX_CP852 852
#define RX_CP866 866
#define RX_CP1252 1252
/** Convert single-byte buffer to UTF-8. Unknown bytes map to U+FFFD. */
int cp_to_utf8(int cp, const uint8_t* in, size_t n, char** out, size_t* outn);
/** Convert UTF-8 to single-byte; unmappable codepoints become '?'. */
int utf8_to_cp(int cp, const char* in, size_t n, uint8_t* out, size_t* outn);

/* ---- ddatecnv.c: civil date <-> julian day number ---- */
/** Julian day number for a civil date (proleptic Gregorian). */
long rx_julian(int y, int m, int d);
/** Inverse of rx_julian. */
void rx_civil(long j, int* y, int* m, int* d);
/** 1 when y-m-d is a real calendar date. */
int rx_date_valid(int y, int m, int d);

/* ---- numeric.c: FoxPro numeric display (SET DECIMALS / SET FIXED) ---- */
/** Format d per FoxPro display rules with `dec` decimals (SET DECIMALS). */
void rx_format_num(char* out, size_t outsz, double d, int dec, int fixed);
/** Parse an ASCII numeric field; all-blank returns 0 and sets *blank. */
double rx_parse_num(const char* s, size_t n, int* blank);

/* ---- errlist.c ---- */
/* implemented in errlist.c; declared in reynard/status.h */

/* ---- sha256.c ---- */
void rx_sha256(const void* data, size_t n, uint8_t out[32]);

/* ---- log.c: RX_LOG trace ---- */
#define RX_LOG_ERROR 1
#define RX_LOG_WARN 2
#define RX_LOG_INFO 3
#define RX_LOG_DEBUG 4
void rx_log_init(void); /* reads RX_LOG once */
void rx_log(int level, const char* fmt, ...);

/* ---- cfg.c: reynard.cfg parsing ---- */
typedef struct rx_cfg {
    char path[260];
    long history;     /* HISTORY = n */
    int codepage;     /* CODEPAGE_DEFAULT = n, 0 = unset */
    char mouse[16];   /* MOUSE = auto|sgr|... */
    int utf8;         /* -1 unset, 0 off, 1 on */
    char** set_lines; /* raw SET statements to replay at startup */
    size_t n_set;
    char** warnings; /* parse warnings (line-numbered) */
    size_t n_warnings;
} rx_cfg;

int rx_cfg_load(rx_cfg* cfg, const char* override_path);
void rx_cfg_free(rx_cfg* cfg);
/** Default config path: $REYNARD_CFG or $XDG_CONFIG_HOME/reynard/reynard.cfg. */
int rx_cfg_default_path(char* out, size_t outsz);

/** Fatal out-of-memory / assertion path: writes reynard-crash.log, aborts. */
void rx_fatal(const char* what, const char* file, int line);
#define RX_FATAL(what) rx_fatal((what), __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_UTIL_H */
