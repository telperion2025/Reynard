/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file rpp.c
 * @brief .RPP container: deterministic stored-ZIP writer + reader (spec 9.6).
 *        See util/rpp.h. All multi-byte fields little-endian; entries use
 *        method 0 (stored), fixed DOS date/time 0 so output is byte-stable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/rpp.h"

/* ---------------- CRC-32 ---------------- */

static uint32_t crc_tab[256];
static int crc_ready = 0;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_tab[i] = c;
    }
    crc_ready = 1;
}

uint32_t rx_crc32(const void *data, size_t n)
{
    if (!crc_ready) {
        crc_init();
    }
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) {
        c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* ---------------- little-endian helpers ---------------- */

static void put16(strbuf *b, uint16_t v)
{
    sb_putc(b, (char)(v & 0xFF));
    sb_putc(b, (char)((v >> 8) & 0xFF));
}

static void put32(strbuf *b, uint32_t v)
{
    sb_putc(b, (char)(v & 0xFF));
    sb_putc(b, (char)((v >> 8) & 0xFF));
    sb_putc(b, (char)((v >> 16) & 0xFF));
    sb_putc(b, (char)((v >> 24) & 0xFF));
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ---------------- writer ---------------- */

typedef struct zip_ent {
    char name[128];
    uint32_t crc;
    uint32_t size;
    uint32_t lho;
    const uint8_t *data; /* borrowed during build */
} zip_ent;

struct rx_zipw {
    zip_ent *ents;
    size_t n, cap;
    uint8_t **blobs; /* owned copies: callers may free their buffers early */
};

rx_zipw *rx_zipw_new(void)
{
    rx_zipw *w = calloc(1, sizeof(*w));
    return w;
}

int rx_zipw_add(rx_zipw *w, const char *name, const void *data, size_t n)
{
    if (!w || !name || n > 0xFFFFFFF0u) {
        return R_ERR_RANGE;
    }
    if (w->n == w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 16;
        zip_ent *ne = realloc(w->ents, nc * sizeof(zip_ent));
        uint8_t **nb = realloc(w->blobs, nc * sizeof(uint8_t *));
        if (!ne || !nb) {
            RX_FATAL("out of memory");
        }
        w->ents = ne;
        w->blobs = nb;
        w->cap = nc;
    }
    zip_ent *e = &w->ents[w->n];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%.*s", 127, name);
    uint8_t *copy = malloc(n ? n : 1);
    if (!copy) {
        RX_FATAL("out of memory");
    }
    if (n) {
        memcpy(copy, data, n);
    }
    e->crc = rx_crc32(copy, n);
    e->size = (uint32_t)n;
    w->blobs[w->n] = copy;
    w->n++;
    return R_OK;
}

size_t rx_zipw_count(const rx_zipw *w)
{
    return w ? w->n : 0;
}

int rx_zipw_finish(rx_zipw *w, char **out, size_t *outn)
{
    if (!w || !out || !outn) {
        return R_ERR_RANGE;
    }
    strbuf b;
    sb_init(&b);
    for (size_t i = 0; i < w->n; i++) {
        zip_ent *e = &w->ents[i];
        e->lho = (uint32_t)b.len;
        put32(&b, 0x04034b50u);       /* local header sig */
        put16(&b, 20);                /* version needed */
        put16(&b, 0);                 /* flags */
        put16(&b, 0);                 /* method: stored */
        put16(&b, 0);                 /* mod time (fixed) */
        put16(&b, 0x21);              /* mod date: 1980-01-01 */
        put32(&b, e->crc);
        put32(&b, e->size);
        put32(&b, e->size);
        put16(&b, (uint16_t)strlen(e->name));
        put16(&b, 0);                 /* extra len */
        sb_put(&b, e->name, strlen(e->name));
        if (e->size) {
            sb_put(&b, (const char *)w->blobs[i], e->size);
        }
    }
    uint32_t cd_off = (uint32_t)b.len;
    for (size_t i = 0; i < w->n; i++) {
        zip_ent *e = &w->ents[i];
        put32(&b, 0x02014b50u);       /* central dir sig */
        put16(&b, 20);                /* version made by */
        put16(&b, 20);                /* version needed */
        put16(&b, 0);
        put16(&b, 0);
        put16(&b, 0);
        put16(&b, 0x21);
        put32(&b, e->crc);
        put32(&b, e->size);
        put32(&b, e->size);
        put16(&b, (uint16_t)strlen(e->name));
        put16(&b, 0);
        put16(&b, 0);
        put16(&b, 0);
        put16(&b, 0);
        put32(&b, 0);                 /* external attrs */
        put32(&b, e->lho);
        sb_put(&b, e->name, strlen(e->name));
    }
    uint32_t cd_size = (uint32_t)b.len - cd_off;
    put32(&b, 0x06054b50u);           /* EOCD */
    put16(&b, 0);
    put16(&b, 0);
    put16(&b, (uint16_t)w->n);
    put16(&b, (uint16_t)w->n);
    put32(&b, cd_size);
    put32(&b, cd_off);
    put16(&b, 0);
    if (!b.buf) {
        sb_putc(&b, '\0');
        b.len = 0;
    }
    *out = b.buf;
    *outn = b.len;
    return R_OK;
}

void rx_zipw_free(rx_zipw *w)
{
    if (!w) {
        return;
    }
    for (size_t i = 0; i < w->n; i++) {
        free(w->blobs[i]);
    }
    free(w->ents);
    free(w->blobs);
    free(w);
}

/* ---------------- reader ---------------- */

int rx_rpp_open_mem(const void *data, size_t n, rx_rpp *out)
{
    if (!data || n < 22 || !out) {
        return R_ERR_RANGE;
    }
    memset(out, 0, sizeof(*out));
    const uint8_t *b = data;
    /* locate EOCD (scan backwards, allowing a 64k comment) */
    size_t eocd = n;
    int found = 0;
    size_t lim = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (size_t i = n - 22;; i--) {
        if (get32(b + i) == 0x06054b50u) {
            eocd = i;
            found = 1;
            break;
        }
        if (i == lim) {
            break;
        }
    }
    if (!found) {
        return R_ERR_DBF_TRUNCATED;
    }
    uint16_t nent = get16(b + eocd + 10);
    uint32_t cd_off = get32(b + eocd + 16);
    if ((size_t)cd_off + 46u * nent > n + 46) {
        return R_ERR_DBF_TRUNCATED;
    }
    out->ents = calloc(nent ? nent : 1, sizeof(rx_rpp_ent));
    if (!out->ents) {
        RX_FATAL("out of memory");
    }
    out->nents = nent;
    size_t p = cd_off;
    for (size_t i = 0; i < nent; i++) {
        if (p + 46 > n || get32(b + p) != 0x02014b50u) {
            free(out->ents);
            out->ents = NULL;
            return R_ERR_DBF_TRUNCATED;
        }
        uint16_t method = get16(b + p + 10);
        uint32_t crc = get32(b + p + 16);
        uint32_t csize = get32(b + p + 20);
        uint32_t usize = get32(b + p + 24);
        uint16_t nlen = get16(b + p + 28);
        uint16_t xlen = get16(b + p + 30);
        uint32_t lho = get32(b + p + 42);
        if (p + 46 + nlen > n) {
            free(out->ents);
            out->ents = NULL;
            return R_ERR_DBF_TRUNCATED;
        }
        if (method != 0) {
            free(out->ents);
            out->ents = NULL;
            return R_ERR_DBF_UNSUPPORTED; /* compressed entry: diagnostic */
        }
        rx_rpp_ent *e = &out->ents[i];
        if (nlen >= sizeof(e->name)) {
            nlen = sizeof(e->name) - 1;
        }
        memcpy(e->name, b + p + 46, nlen);
        e->name[nlen] = '\0';
        e->crc = crc;
        e->size = csize != 0 ? csize : usize;
        e->lho = lho;
        p += 46 + nlen + xlen + get16(b + p + 32);
    }
    out->data = malloc(n ? n : 1);
    if (!out->data) {
        RX_FATAL("out of memory");
    }
    memcpy(out->data, data, n);
    out->len = n;
    return R_OK;
}

int rx_rpp_open(const char *path, rx_rpp *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return R_ERR_IO_NOTFOUND;
    }
    strbuf b;
    sb_init(&b);
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        sb_put(&b, buf, got);
    }
    fclose(f);
    if (!b.buf || b.len == 0) {
        sb_free(&b);
        return R_ERR_IO_READ;
    }
    int rc = rx_rpp_open_mem(b.buf, b.len, out);
    sb_free(&b);
    return rc;
}

const rx_rpp_ent *rx_rpp_find(const rx_rpp *r, const char *name)
{
    if (!r || !name) {
        return NULL;
    }
    for (size_t i = 0; i < r->nents; i++) {
        if (strcmp(r->ents[i].name, name) == 0) {
            return &r->ents[i];
        }
    }
    return NULL;
}

int rx_rpp_read(const rx_rpp *r, const rx_rpp_ent *e, uint8_t **out, size_t *outn)
{
    if (!r || !e || !out || !outn) {
        return R_ERR_RANGE;
    }
    size_t lho = e->lho;
    if (lho + 30 > r->len || get32(r->data + lho) != 0x04034b50u) {
        return R_ERR_DBF_TRUNCATED;
    }
    uint16_t nlen = get16(r->data + lho + 26);
    uint16_t xlen = get16(r->data + lho + 28);
    size_t off = lho + 30 + nlen + xlen;
    if (off + e->size > r->len) {
        return R_ERR_DBF_TRUNCATED;
    }
    uint8_t *buf = malloc(e->size ? e->size : 1);
    if (!buf) {
        RX_FATAL("out of memory");
    }
    memcpy(buf, r->data + off, e->size);
    if (rx_crc32(buf, e->size) != e->crc) {
        free(buf);
        return R_ERR_DBF_TRUNCATED; /* CRC mismatch */
    }
    *out = buf;
    *outn = e->size;
    return R_OK;
}

void rx_rpp_close(rx_rpp *r)
{
    if (!r) {
        return;
    }
    free(r->data);
    free(r->ents);
    memset(r, 0, sizeof(*r));
}
