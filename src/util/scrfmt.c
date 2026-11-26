/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file scrfmt.c
 * @brief Screen file formats (spec 9.2, M7): .SCR text model, .RSO compiled
 *        form, .PRG code generation. The .SCR text form is canonical (the
 *        writer emits exactly what the parser accepts, in a fixed field
 *        order), so a load/save round-trip is byte-identical; the .RSO adds a
 *        magic header, a sha256 of the canonical .SCR bytes, and fixed-width
 *        little-endian item records, so it is byte-reproducible across builds.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/scrfmt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================ model ============================ */

const char *scrf_type_name(int t)
{
    switch (t) {
    case SCR_SAY: return "SAY";
    case SCR_GET: return "GET";
    case SCR_BOX: return "BOX";
    case SCR_LINE: return "LINE";
    case SCR_BUTTON: return "BUTTON";
    case SCR_RADIO: return "RADIO";
    case SCR_CHECK: return "CHECK";
    case SCR_POPUP: return "POPUP";
    case SCR_SPIN: return "SPIN";
    case SCR_INVB: return "INVB";
    default: return "?";
    }
}

static int type_from_name(const char *s)
{
    if (!strcmp(s, "SAY")) return SCR_SAY;
    if (!strcmp(s, "GET")) return SCR_GET;
    if (!strcmp(s, "BOX")) return SCR_BOX;
    if (!strcmp(s, "LINE")) return SCR_LINE;
    if (!strcmp(s, "BUTTON")) return SCR_BUTTON;
    if (!strcmp(s, "RADIO")) return SCR_RADIO;
    if (!strcmp(s, "CHECK")) return SCR_CHECK;
    if (!strcmp(s, "POPUP")) return SCR_POPUP;
    if (!strcmp(s, "SPIN")) return SCR_SPIN;
    if (!strcmp(s, "INVB")) return SCR_INVB;
    return 0;
}

void scrf_init(scr_file *sf, const char *name)
{
    memset(sf, 0, sizeof(*sf));
    snprintf(sf->name, sizeof(sf->name), "%s", name ? name : "SCREEN");
    sf->h = 24;
    sf->w = 80;
}

void scrf_free(scr_file *sf)
{
    free(sf->items);
    sf->items = NULL;
    sf->nitems = sf->capitems = 0;
}

scr_item *scrf_add(scr_file *sf)
{
    if (sf->nitems == sf->capitems) {
        size_t nc = sf->capitems ? sf->capitems * 2 : 16;
        scr_item *ni = realloc(sf->items, nc * sizeof(scr_item));
        if (!ni) {
            RX_FATAL("out of memory");
        }
        sf->items = ni;
        sf->capitems = nc;
    }
    scr_item *it = &sf->items[sf->nitems++];
    memset(it, 0, sizeof(*it));
    it->h = 1;
    it->w = 10;
    it->scheme = 1;
    return it;
}

/* ============================ .SCR text ============================ */

static void put_quoted(strbuf *sb, const char *s)
{
    sb_putc(sb, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            sb_putc(sb, '\\');
        }
        sb_putc(sb, *p);
    }
    sb_putc(sb, '"');
}

/* canonical serialization: fixed field order, one ITEM per line */
int scrf_serialize(const scr_file *sf, strbuf *sb)
{
    sb_printf(sb, "REYNARD SCREEN 1\n");
    sb_printf(sb, "NAME %s\n", sf->name);
    sb_printf(sb, "SIZE %d,%d\n", sf->h, sf->w);
    for (size_t i = 0; i < sf->nitems; i++) {
        const scr_item *it = &sf->items[i];
        switch (it->type) {
        case SCR_SAY:
            sb_printf(sb, "ITEM SAY %d,%d SCHEME %d ", it->row, it->col, it->scheme);
            put_quoted(sb, it->text);
            sb_putc(sb, '\n');
            break;
        case SCR_GET:
            sb_printf(sb, "ITEM GET %d,%d VAR %s", it->row, it->col, it->var);
            if (it->text[0]) {
                sb_puts(sb, " SAY ");
                put_quoted(sb, it->text);
            }
            sb_printf(sb, " SIZE %d,%d", it->h, it->w);
            if (it->pic[0]) {
                sb_puts(sb, " PICTURE ");
                put_quoted(sb, it->pic);
            }
            if (it->func[0]) {
                sb_puts(sb, " FUNCTION ");
                put_quoted(sb, it->func);
            }
            if (it->when[0]) {
                sb_printf(sb, " WHEN %s", it->when);
            }
            if (it->valid[0]) {
                sb_printf(sb, " VALID %s", it->valid);
            }
            if (it->rang[0]) {
                sb_printf(sb, " RANGE %s", it->rang);
            }
            if (it->deflt[0]) {
                sb_printf(sb, " DEFAULT %s", it->deflt);
            }
            sb_printf(sb, " SCHEME %d\n", it->scheme);
            break;
        case SCR_BOX:
        case SCR_LINE:
            sb_printf(sb, "ITEM %s %d,%d TO %d,%d SCHEME %d\n", scrf_type_name(it->type),
                      it->row, it->col, it->row2, it->col2, it->scheme);
            break;
        default: /* controls */
            sb_printf(sb, "ITEM %s %d,%d PROMPT ", scrf_type_name(it->type), it->row, it->col);
            put_quoted(sb, it->text);
            sb_printf(sb, " VAR %s", it->var);
            if (it->type == SCR_INVB) {
                sb_printf(sb, " SIZE %d,%d", it->h, it->w);
            }
            sb_printf(sb, " SCHEME %d\n", it->scheme);
            break;
        }
    }
    sb_puts(sb, "END\n");
    return R_OK;
}

/* tokenizer for one .SCR line: splits on spaces, honors "quoted" strings */
typedef struct {
    char tok[300];
} scr_tok;

static size_t scr_tokenize(char *line, scr_tok *toks, size_t max)
{
    size_t n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        char *out = toks[n].tok;
        size_t ol = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && ol < sizeof(toks[n].tok) - 1) {
                if (*p == '\\' && p[1]) {
                    p++;
                }
                out[ol++] = *p++;
            }
            if (*p == '"') {
                p++;
            }
        } else {
            while (*p && *p != ' ' && *p != '\t' && ol < sizeof(toks[n].tok) - 1) {
                out[ol++] = *p++;
            }
        }
        out[ol] = '\0';
        n++;
    }
    return n;
}

/* parse one ITEM line; raw tail fields use the untokenized remainder */
static int scrf_parse_item(char *line, scr_item *it)
{
    /* find the raw tail for WHEN/VALID/RANGE/DEFAULT: locate keyword in the
       raw line (outside quotes) and copy the rest verbatim */
    scr_tok tk[24];
    size_t n = scr_tokenize(line, tk, 24);
    if (n < 2 || strcmp(tk[0].tok, "ITEM") != 0) {
        return R_ERR_SYNTAX;
    }
    it->type = type_from_name(tk[1].tok);
    if (!it->type || n < 4) {
        return R_ERR_SYNTAX;
    }
    if (sscanf(tk[2].tok, "%d,%d", &it->row, &it->col) != 2) {
        return R_ERR_SYNTAX;
    }
    size_t i = 3;
    if (it->type == SCR_BOX || it->type == SCR_LINE) {
        if (n < 6 || strcmp(tk[3].tok, "TO") != 0) {
            return R_ERR_SYNTAX;
        }
        if (sscanf(tk[4].tok, "%d,%d", &it->row2, &it->col2) != 2) {
            return R_ERR_SYNTAX;
        }
        i = 5;
        if (i < n && strcmp(tk[i].tok, "SCHEME") == 0 && i + 1 < n) {
            it->scheme = atoi(tk[i + 1].tok);
        }
        return R_OK;
    }
    if (it->type == SCR_SAY) {
        /* ITEM SAY r,c SCHEME n "text" */
        if (i < n && strcmp(tk[i].tok, "SCHEME") == 0 && i + 1 < n) {
            it->scheme = atoi(tk[i + 1].tok);
            i += 2;
        }
        if (i < n) {
            snprintf(it->text, sizeof(it->text), "%.255s", tk[i].tok);
        }
        return R_OK;
    }
    /* GET and controls */
    for (; i < n; i++) {
        const char *k = tk[i].tok;
        if (strcmp(k, "VAR") == 0 && i + 1 < n) {
            snprintf(it->var, sizeof(it->var), "%.63s", tk[i + 1].tok);
            i++;
        } else if (strcmp(k, "SAY") == 0 && i + 1 < n) {
            snprintf(it->text, sizeof(it->text), "%.255s", tk[i + 1].tok);
            i++;
        } else if (strcmp(k, "PROMPT") == 0 && i + 1 < n) {
            snprintf(it->text, sizeof(it->text), "%.255s", tk[i + 1].tok);
            i++;
        } else if (strcmp(k, "SIZE") == 0 && i + 1 < n) {
            sscanf(tk[i + 1].tok, "%d,%d", &it->h, &it->w);
            i++;
        } else if (strcmp(k, "PICTURE") == 0 && i + 1 < n) {
            snprintf(it->pic, sizeof(it->pic), "%.63s", tk[i + 1].tok);
            i++;
        } else if (strcmp(k, "FUNCTION") == 0 && i + 1 < n) {
            snprintf(it->func, sizeof(it->func), "%.15s", tk[i + 1].tok);
            i++;
        } else if (strcmp(k, "SCHEME") == 0 && i + 1 < n) {
            it->scheme = atoi(tk[i + 1].tok);
            i++;
        }
        /* WHEN/VALID/RANGE/DEFAULT are raw tail expressions, extracted below */
    }
    /* raw tail fields: each keyword's expression runs to the next clause
       keyword (canonical order WHEN, VALID, RANGE, DEFAULT, SCHEME) */
    static const char *kws[] = { "WHEN", "VALID", "RANGE", "DEFAULT", NULL };
    char *dsts[] = { it->when, it->valid, it->rang, it->deflt };
    size_t caps[] = { sizeof(it->when), sizeof(it->valid), sizeof(it->rang), sizeof(it->deflt) };
    static const char *stops[] = { " WHEN ", " VALID ", " RANGE ", " DEFAULT ", " SCHEME ", NULL };
    for (int wi = 0; kws[wi]; wi++) {
        const char *kw = strstr(line, kws[wi]);
        /* match as a keyword: preceded by space/start and followed by space */
        while (kw && !(kw == line || kw[-1] == ' ') ) {
            kw = strstr(kw + 1, kws[wi]);
        }
        if (!kw) {
            continue;
        }
        kw += strlen(kws[wi]);
        while (*kw == ' ') {
            kw++;
        }
        size_t tlen = strlen(kw);
        for (int si = 0; stops[si]; si++) {
            const char *hit = strstr(kw, stops[si]);
            if (hit && (size_t)(hit - kw) < tlen) {
                tlen = (size_t)(hit - kw);
            }
        }
        /* rtrim */
        while (tlen && (kw[tlen - 1] == ' ' || kw[tlen - 1] == '\t')) {
            tlen--;
        }
        if (tlen > caps[wi] - 1) {
            tlen = caps[wi] - 1;
        }
        snprintf(dsts[wi], caps[wi], "%.*s", (int)tlen, kw);
    }
    return R_OK;
}

int scrf_load_text(const char *text, scr_file *sf)
{
    scrf_init(sf, "SCREEN");
    char *copy = rx_strdup(text);
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) {
            line[--l] = '\0';
        }
        if (!l || line[0] == '#') {
            continue;
        }
        if (strncmp(line, "REYNARD SCREEN", 14) == 0) {
            continue;
        }
        if (strncmp(line, "NAME ", 5) == 0) {
            snprintf(sf->name, sizeof(sf->name), "%s", line + 5);
            continue;
        }
        if (strncmp(line, "SIZE ", 5) == 0) {
            sscanf(line + 5, "%d,%d", &sf->h, &sf->w);
            continue;
        }
        if (strcmp(line, "END") == 0) {
            break;
        }
        if (strncmp(line, "ITEM ", 5) == 0) {
            scr_item *it = scrf_add(sf);
            int rc = scrf_parse_item(line, it);
            if (rc != R_OK) {
                free(copy);
                return rc;
            }
            continue;
        }
        free(copy);
        return R_ERR_SYNTAX;
    }
    free(copy);
    return R_OK;
}

static char *read_all(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    strbuf sb;
    sb_init(&sb);
    char tmp[4096];
    size_t got;
    while ((got = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        sb_put(&sb, tmp, got);
    }
    fclose(f);
    if (n) {
        *n = sb.len;
    }
    if (!sb.buf) {
        sb.buf = rx_strdup("");
        sb.len = 0;
    }
    sb.buf[sb.len] = '\0';
    return sb.buf;
}

int scrf_load(const char *path, scr_file *sf)
{
    size_t n = 0;
    char *text = read_all(path, &n);
    if (!text) {
        return R_ERR_IO_NOTFOUND;
    }
    int rc = scrf_load_text(text, sf);
    free(text);
    return rc;
}

int scrf_save(const char *path, const scr_file *sf)
{
    strbuf sb;
    sb_init(&sb);
    scrf_serialize(sf, &sb);
    FILE *f = fopen(path, "w");
    if (!f) {
        sb_free(&sb);
        return R_ERR_IO_OPEN;
    }
    int rc = R_OK;
    if (fwrite(sb.buf ? sb.buf : "", 1, sb.len, f) != sb.len) {
        rc = R_ERR_IO_WRITE;
    }
    fclose(f);
    sb_free(&sb);
    return rc;
}

/* ============================ .RSO binary ============================ */

#define RSO_MAGIC 0x4F535252u /* "RRSO" LE */
#define RSO_VERSION 1u
#define RSO_ITEM_REC 896u

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_str(uint8_t *dst, size_t cap, const char *s)
{
    memset(dst, 0, cap);
    if (s) {
        size_t n = strlen(s);
        if (n > cap - 1) {
            n = cap - 1;
        }
        memcpy(dst, s, n);
    }
}
static void get_str(const uint8_t *src, size_t cap, char *out)
{
    size_t n = 0;
    while (n < cap && src[n]) {
        n++;
    }
    memcpy(out, src, n);
    out[n < cap ? n : cap - 1] = '\0';
}

static void item_to_rec(const scr_item *it, uint8_t *rec)
{
    memset(rec, 0, RSO_ITEM_REC);
    put_u16(rec + 0, (uint16_t)it->type);
    put_u16(rec + 2, (uint16_t)it->row);
    put_u16(rec + 4, (uint16_t)it->col);
    put_u16(rec + 6, (uint16_t)it->row2);
    put_u16(rec + 8, (uint16_t)it->col2);
    put_u16(rec + 10, (uint16_t)it->h);
    put_u16(rec + 12, (uint16_t)it->w);
    put_u16(rec + 14, (uint16_t)it->scheme);
    put_str(rec + 16, 256, it->text);
    put_str(rec + 272, 64, it->var);
    put_str(rec + 336, 64, it->pic);
    put_str(rec + 400, 16, it->func);
    put_str(rec + 416, 128, it->when);
    put_str(rec + 544, 128, it->valid);
    put_str(rec + 672, 96, it->rang);
    put_str(rec + 768, 96, it->deflt);
}

static void item_from_rec(const uint8_t *rec, scr_item *it)
{
    memset(it, 0, sizeof(*it));
    it->type = get_u16(rec + 0);
    it->row = get_u16(rec + 2);
    it->col = get_u16(rec + 4);
    it->row2 = get_u16(rec + 6);
    it->col2 = get_u16(rec + 8);
    it->h = get_u16(rec + 10);
    it->w = get_u16(rec + 12);
    it->scheme = get_u16(rec + 14);
    get_str(rec + 16, 256, it->text);
    get_str(rec + 272, 64, it->var);
    get_str(rec + 336, 64, it->pic);
    get_str(rec + 400, 16, it->func);
    get_str(rec + 416, 128, it->when);
    get_str(rec + 544, 128, it->valid);
    get_str(rec + 672, 96, it->rang);
    get_str(rec + 768, 96, it->deflt);
}

int scrf_compile(const char *rsopath, const scr_file *sf)
{
    strbuf sb;
    sb_init(&sb);
    scrf_serialize(sf, &sb);
    uint8_t hash[32];
    rx_sha256(sb.buf ? sb.buf : "", sb.len, hash);
    FILE *f = fopen(rsopath, "wb");
    if (!f) {
        sb_free(&sb);
        return R_ERR_IO_OPEN;
    }
    /* layout: 0 magic, 4 ver, 8 name[64] -> 72 h, 76 w, 80 nitems, 84 recrec, 88 hash[32] -> 120 */
    uint8_t hdr[120];
    memset(hdr, 0, sizeof(hdr));
    put_u32(hdr + 0, RSO_MAGIC);
    put_u32(hdr + 4, RSO_VERSION);
    put_str(hdr + 8, 64, sf->name);
    put_u32(hdr + 72, (uint32_t)sf->h);
    put_u32(hdr + 76, (uint32_t)sf->w);
    put_u32(hdr + 80, (uint32_t)sf->nitems);
    put_u32(hdr + 84, RSO_ITEM_REC);
    memcpy(hdr + 88, hash, 32);
    int rc = R_OK;
    if (fwrite(hdr, 1, 120, f) != 120) {
        rc = R_ERR_IO_WRITE;
    }
    uint8_t *rec = calloc(1, RSO_ITEM_REC);
    if (!rec) {
        RX_FATAL("out of memory");
    }
    for (size_t i = 0; i < sf->nitems && rc == R_OK; i++) {
        item_to_rec(&sf->items[i], rec);
        if (fwrite(rec, 1, RSO_ITEM_REC, f) != RSO_ITEM_REC) {
            rc = R_ERR_IO_WRITE;
        }
    }
    free(rec);
    fclose(f);
    sb_free(&sb);
    return rc;
}

int scrf_load_rso(const char *rsopath, scr_file *sf)
{
    size_t n = 0;
    uint8_t *buf = (uint8_t *)read_all(rsopath, &n);
    if (!buf) {
        return R_ERR_IO_NOTFOUND;
    }
    if (n < 120 || get_u32(buf) != RSO_MAGIC || get_u32(buf + 4) != RSO_VERSION) {
        free(buf);
        return R_ERR_CDX_VARIANT; /* reuse generic "not our format" */
    }
    scrf_init(sf, "SCREEN");
    get_str(buf + 8, 64, sf->name);
    sf->h = (int)get_u32(buf + 72);
    sf->w = (int)get_u32(buf + 76);
    uint32_t ni = get_u32(buf + 80);
    uint32_t rec = get_u32(buf + 84);
    if (rec != RSO_ITEM_REC || n < 120 + (size_t)ni * RSO_ITEM_REC) {
        free(buf);
        return R_ERR_CDX_VARIANT;
    }
    for (uint32_t i = 0; i < ni; i++) {
        scr_item *it = scrf_add(sf);
        item_from_rec(buf + 120 + (size_t)i * RSO_ITEM_REC, it);
    }
    free(buf);
    return R_OK;
}

/* ============================ .PRG codegen ============================ */

static void gen_quoted(strbuf *sb, const char *s)
{
    put_quoted(sb, s);
}

int scrf_generate(const scr_file *sf, strbuf *out)
{
    sb_printf(out, "* Reynard generated screen %s (from %s.scr)\n", sf->name, sf->name);
    sb_puts(out, "* Regenerate with GENERATE SCREEN; edit the .SCR, not this file.\n");
    for (size_t i = 0; i < sf->nitems; i++) {
        const scr_item *it = &sf->items[i];
        switch (it->type) {
        case SCR_SAY:
            sb_printf(out, "@ %d,%d SAY ", it->row, it->col);
            gen_quoted(out, it->text);
            sb_putc(out, '\n');
            break;
        case SCR_GET: {
            sb_printf(out, "@ %d,%d ", it->row, it->col);
            if (it->text[0]) {
                sb_puts(out, "SAY ");
                gen_quoted(out, it->text);
                sb_putc(out, ' ');
            }
            sb_printf(out, "GET %s SIZE %d,%d", it->var, it->h, it->w);
            if (it->pic[0]) {
                sb_puts(out, " PICTURE ");
                gen_quoted(out, it->pic);
            }
            if (it->func[0]) {
                sb_puts(out, " FUNCTION ");
                gen_quoted(out, it->func);
            }
            if (it->when[0]) {
                sb_printf(out, " WHEN %s", it->when);
            }
            if (it->valid[0]) {
                sb_printf(out, " VALID %s", it->valid);
            }
            if (it->rang[0]) {
                sb_printf(out, " RANGE %s", it->rang);
            }
            if (it->deflt[0]) {
                sb_printf(out, " DEFAULT %s", it->deflt);
            }
            sb_putc(out, '\n');
            break;
        }
        case SCR_BOX:
            sb_printf(out, "@ %d,%d BOX TO %d,%d\n", it->row, it->col, it->row2, it->col2);
            break;
        case SCR_LINE:
            sb_printf(out, "@ %d,%d TO %d,%d\n", it->row, it->col, it->row2, it->col2);
            break;
        default:
            /* v1: FoxPro control types are stored in the .SCR and rendered by
               the designer; generated code carries them as comments
               (DEVIATIONS) */
            sb_printf(out, "* CONTROL %s %d,%d ", scrf_type_name(it->type), it->row, it->col);
            gen_quoted(out, it->text);
            sb_printf(out, " VAR %s\n", it->var);
            break;
        }
    }
    sb_puts(out, "READ CYCLE\n");
    return R_OK;
}

