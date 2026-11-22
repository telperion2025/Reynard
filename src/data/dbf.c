/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file dbf.c
 * @brief DBF reader (spec §8.2). M1/M2 scope: read-only open of dBase III+,
 *        dBase IV, FoxPro and VFP-structural files with diagnostics on
 *        corruption — never silent best guesses. Writes land at M2.
 */
/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "data/data.h"
#include "util/util.h"

static int accepted_version(uint8_t v)
{
    switch (v) {
    case 0x03: /* dBase III+ */
    case 0x04: /* dBase IV */
    case 0x30: /* FoxPro */
    case 0x31:
    case 0x32: /* VFP */
    case 0x83: /* dBase III+ with memo */
    case 0x8B: /* dBase IV with memo */
    case 0xF5: /* FoxPro with memo */
        return 1;
    default:
        return 0;
    }
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int rx_dbf_open(const char *path, int mode, rx_dbf **out)
{
    if (!path || !out) {
        return R_ERR_RANGE;
    }
    *out = NULL;
    int flags = O_RDONLY;
    if (mode & RD_EXCLUSIVE) {
        flags = O_RDWR;
    }
    int fd = open(path, flags);
    if (fd < 0) {
        return errno == ENOENT ? R_ERR_IO_NOTFOUND : R_ERR_IO_OPEN;
    }
    rx_dbf *dbf = calloc(1, sizeof(*dbf));
    if (!dbf) {
        close(fd);
        RX_FATAL("out of memory");
    }
    dbf->fd = fd;
    dbf->mode = mode;
    dbf->readonly = !(mode & RD_EXCLUSIVE);
    snprintf(dbf->path, sizeof(dbf->path), "%s", path);

    uint8_t hdr[32];
    ssize_t got = read(fd, hdr, sizeof(hdr));
    if (got < 32) {
        rx_dbf_close(dbf);
        return got < 0 ? R_ERR_IO_READ : R_ERR_DBF_TRUNCATED;
    }
    dbf->version = hdr[0];
    if (!accepted_version(dbf->version)) {
        rx_dbf_close(dbf);
        return R_ERR_DBF_MAGIC;
    }
    dbf->nrec = rd32(hdr + 4);
    dbf->header_size = rd16(hdr + 8);
    dbf->rec_size = rd16(hdr + 10);
    dbf->lang_driver = hdr[29];
    if (dbf->header_size < 65 || dbf->rec_size < 1 || (dbf->header_size - 1) % 32 != 0) {
        rx_dbf_close(dbf);
        return R_ERR_DBF_HEADER;
    }
    int cp = rx_mark_to_cp(dbf->lang_driver);
    if (cp == -1) {
        rx_log(RX_LOG_WARN, "%s: unknown code page mark 0x%02X, no conversion", path,
               dbf->lang_driver);
        cp = 0;
    } else if (rx_mark_known_no_table(dbf->lang_driver)) {
        rx_log(RX_LOG_WARN, "%s: code page mark 0x%02X has no table yet, no conversion", path,
               dbf->lang_driver);
        cp = 0;
    }
    dbf->codepage = cp;

    size_t maxf = (dbf->header_size - 33) / 32 + 1;
    dbf->fields = calloc(maxf, sizeof(rx_fielddef));
    dbf->foff = calloc(maxf, sizeof(size_t));
    if (!dbf->fields || !dbf->foff) {
        rx_dbf_close(dbf);
        RX_FATAL("out of memory");
    }
    /* Field descriptors start at offset 32, each 32 bytes, terminated 0x0D. */
    size_t fsize = (size_t)dbf->header_size - 33;
    uint8_t *desc = malloc(fsize + 1);
    if (!desc) {
        rx_dbf_close(dbf);
        RX_FATAL("out of memory");
    }
    got = pread(fd, desc, fsize, (off_t)32);
    if (got < 0 || (size_t)got != fsize) {
        free(desc);
        rx_dbf_close(dbf);
        return R_ERR_DBF_TRUNCATED;
    }
    size_t nf = 0;
    size_t rec_off = 1; /* delete flag */
    for (size_t i = 0; i < fsize; i += 32) {
        if (desc[i] == 0x0D) {
            break;
        }
        rx_fielddef *f = &dbf->fields[nf];
        memset(f, 0, sizeof(*f));
        memcpy(f->name, desc + i, 11);
        f->name[10] = '\0';
        /* trim at first NUL */
        for (int k = 0; k < 10; k++) {
            if (f->name[k] == '\0') {
                f->name[k] = '\0';
                break;
            }
        }
        f->type = desc[i + 11];
        f->len = desc[i + 16];
        f->dec = desc[i + 17];
        rx_strtoupper(f->name);
        switch (f->type) {
        case RFT_C:
            if (f->len == 0) {
                f->len = 1;
            }
            break;
        case RFT_N:
        case RFT_F:
            if (f->len < 1 || f->len > 20 || f->dec > 15) {
                free(desc);
                rx_dbf_close(dbf);
                return R_ERR_DBF_FIELDS;
            }
            break;
        case RFT_D:
            f->len = 8;
            break;
        case RFT_L:
            f->len = 1;
            break;
        case RFT_M:
        case RFT_G:
        case RFT_P:
            f->len = 10;
            break;
        case RFT_I:
            f->len = 4;
            break;
        case RFT_Y:
            f->len = 8;
            break;
        case RFT_B:
            f->len = 8;
            break;
        case RFT_T:
            f->len = 8;
            break;
        default:
            free(desc);
            rx_dbf_close(dbf);
            return R_ERR_DBF_FIELDS;
        }
        dbf->foff[nf] = rec_off;
        rec_off += f->len;
        nf++;
        if (nf >= maxf) {
            free(desc);
            rx_dbf_close(dbf);
            return R_ERR_DBF_FIELDS;
        }
    }
    free(desc);
    dbf->nfld = nf;
    if (nf == 0) {
        rx_dbf_close(dbf);
        return R_ERR_DBF_FIELDS;
    }
    if (rec_off != dbf->rec_size) {
        rx_log(RX_LOG_WARN, "%s: computed record size %zu != header %u", path, rec_off,
               dbf->rec_size);
    }
    dbf->recbuf = malloc(dbf->rec_size);
    if (!dbf->recbuf) {
        rx_dbf_close(dbf);
        RX_FATAL("out of memory");
    }
    /* Default alias: file stem, uppercased. */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char stem[11];
    size_t sn = 0;
    for (size_t i = 0; base[i] && base[i] != '.' && sn < 10; i++) {
        stem[sn++] = base[i];
    }
    stem[sn] = '\0';
    rx_strtoupper(stem);
    snprintf(dbf->alias, sizeof(dbf->alias), "%s", stem);

    /* Initial position: record 1; empty table is both BOF and EOF. */
    dbf->recno = 1;
    dbf->bof = 0;
    dbf->eof = dbf->nrec == 0;
    dbf->deleted = 0;
    *out = dbf;
    return R_OK;
}

int rx_dbf_close(rx_dbf *dbf)
{
    if (!dbf) {
        return R_OK;
    }
    if (dbf->fd >= 0) {
        close(dbf->fd);
    }
    free(dbf->fields);
    free(dbf->foff);
    free(dbf->recbuf);
    free(dbf);
    return R_OK;
}

int rx_dbf_reopen(rx_dbf *dbf, int newmode)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->mode == newmode) {
        return R_OK;
    }
    /* M1: read-only engine; mode downgrade is a no-op. */
    dbf->mode = newmode;
    dbf->readonly = !(newmode & RD_EXCLUSIVE);
    return R_OK;
}

static int load_rec(rx_dbf *dbf)
{
    if (dbf->eof || dbf->recno < 1 || dbf->recno > dbf->nrec) {
        return R_ERR_DBF_RECNO;
    }
    off_t off = (off_t)dbf->header_size + (off_t)(dbf->recno - 1) * dbf->rec_size;
    ssize_t got = pread(dbf->fd, dbf->recbuf, dbf->rec_size, off);
    if (got < 0) {
        return R_ERR_IO_READ;
    }
    if ((size_t)got != dbf->rec_size) {
        return R_ERR_DBF_TRUNCATED;
    }
    dbf->deleted = dbf->recbuf[0] == 0x2A;
    return R_OK;
}

int rx_dbf_goto(rx_dbf *dbf, uint32_t recno)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->nrec == 0) {
        dbf->recno = 1;
        dbf->bof = 1;
        dbf->eof = 1;
        return R_OK;
    }
    if (recno < 1 || recno > dbf->nrec) {
        return R_ERR_DBF_RECNO;
    }
    dbf->recno = recno;
    dbf->bof = 0;
    dbf->eof = 0;
    return load_rec(dbf);
}

int rx_dbf_skip(rx_dbf *dbf, int32_t delta)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->nrec == 0) {
        dbf->recno = 1;
        dbf->bof = 1;
        dbf->eof = 1;
        return R_OK;
    }
    /* logical position: past-end counts as nrec+1 (spec §7.6 BOF/EOF rules) */
    int64_t base = dbf->eof ? (int64_t)dbf->nrec + 1 : (int64_t)dbf->recno;
    int64_t target = base + delta;
    if (target < 1) {
        dbf->recno = 1;
        dbf->bof = 1;
        dbf->eof = 0;
        return load_rec(dbf);
    }
    if (target > (int64_t)dbf->nrec) {
        dbf->recno = dbf->nrec + 1;
        dbf->eof = 1;
        dbf->bof = 0;
        dbf->deleted = 0;
        return R_OK;
    }
    dbf->recno = (uint32_t)target;
    dbf->bof = 0;
    dbf->eof = 0;
    return load_rec(dbf);
}

int rx_dbf_bof(const rx_dbf *dbf, int *out)
{
    *out = dbf ? dbf->bof : 1;
    return R_OK;
}

int rx_dbf_eof(const rx_dbf *dbf, int *out)
{
    *out = dbf ? dbf->eof : 1;
    return R_OK;
}

int rx_dbf_recount(const rx_dbf *dbf, uint32_t *out)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    *out = dbf->nrec;
    return R_OK;
}

int rx_dbf_recno(const rx_dbf *dbf, uint32_t *out)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    *out = dbf->recno;
    return R_OK;
}

int rx_dbf_get(rx_dbf *dbf, size_t fldidx, rval *out)
{
    if (!dbf || !out) {
        return R_ERR_RANGE;
    }
    if (fldidx >= dbf->nfld) {
        return R_ERR_FIELD;
    }
    if (dbf->eof || dbf->nrec == 0) {
        /* FoxPro: reading fields past EOF yields blank values, not an error */
        const rx_fielddef *f = &dbf->fields[fldidx];
        rx_rval_undef(out);
        switch (f->type) {
        case RFT_C:
            rval_set_str(out, rx_strdup(""), 0);
            break;
        case RFT_N:
        case RFT_F:
            out->t = RX_T_N;
            out->u.n.d = 0;
            out->u.n.dec = f->dec;
            break;
        case RFT_L:
            out->t = RX_T_L;
            out->u.l = L_UNDEF;
            break;
        case RFT_D:
            out->t = RX_T_D;
            memset(&out->u.d, 0, sizeof(r_date));
            break;
        default:
            break;
        }
        return R_OK;
    }
    int rc = load_rec(dbf);
    if (rc != R_OK) {
        return rc;
    }
    return rx_field_decode(dbf, fldidx, dbf->recbuf + dbf->foff[fldidx], out);
}

int rx_dbf_put(rx_dbf *dbf, size_t fldidx, const rval *in)
{
    (void)dbf;
    (void)fldidx;
    (void)in;
    return R_ERR_DBF_UNSUPPORTED; /* writes land at M2 */
}

int rx_flush(rx_dbf *dbf)
{
    (void)dbf;
    return R_OK;
}

int rx_dbf_field_count(const rx_dbf *dbf, size_t *out)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    *out = dbf->nfld;
    return R_OK;
}

int rx_dbf_field_def(const rx_dbf *dbf, size_t fldidx, rx_fielddef *out)
{
    if (!dbf || fldidx >= dbf->nfld) {
        return R_ERR_FIELD;
    }
    *out = dbf->fields[fldidx];
    return R_OK;
}

int rx_dbf_field_index(const rx_dbf *dbf, const char *name, size_t *out)
{
    if (!dbf || !name) {
        return R_ERR_RANGE;
    }
    char up[11];
    snprintf(up, sizeof(up), "%s", name);
    rx_strtoupper(up);
    for (size_t i = 0; i < dbf->nfld; i++) {
        if (strcmp(dbf->fields[i].name, up) == 0) {
            *out = i;
            return R_OK;
        }
    }
    return R_ERR_FIELD;
}

int rx_dbf_alias(const rx_dbf *dbf, const char **out)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    *out = dbf->alias;
    return R_OK;
}

int rx_dbf_set_alias(rx_dbf *dbf, const char *alias)
{
    if (!dbf || !alias) {
        return R_ERR_RANGE;
    }
    char up[11];
    snprintf(up, sizeof(up), "%s", alias);
    rx_strtoupper(up);
    snprintf(dbf->alias, sizeof(dbf->alias), "%s", up);
    return R_OK;
}

int rx_dbf_deleted(const rx_dbf *dbf, int *out)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    *out = dbf->deleted;
    return R_OK;
}

int rx_dbf_path(const rx_dbf *dbf, const char **out)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    *out = dbf->path;
    return R_OK;
}

int rx_dbf_codepage(const rx_dbf *dbf, int *out)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    *out = dbf->codepage;
    return R_OK;
}

/* ---- Locks / transactions / indexes: M2+ stubs, honest codes ---- */

int rx_rlock(rx_dbf *dbf, uint32_t recno)
{
    (void)dbf;
    (void)recno;
    return R_ERR_DBF_UNSUPPORTED;
}

int rx_flock(rx_dbf *dbf)
{
    (void)dbf;
    return R_ERR_DBF_UNSUPPORTED;
}

void rx_unlock(rx_dbf *dbf, uint32_t recno)
{
    (void)dbf;
    (void)recno;
}

int rx_rlock_query(const rx_dbf *dbf, uint32_t recno)
{
    (void)dbf;
    (void)recno;
    return 0;
}

int rx_begin(rx_dbf *dbf)
{
    (void)dbf;
    return R_ERR_DBF_UNSUPPORTED;
}

int rx_commit(rx_dbf *dbf)
{
    (void)dbf;
    return R_ERR_DBF_UNSUPPORTED;
}

int rx_rollback(rx_dbf *dbf)
{
    (void)dbf;
    return R_ERR_DBF_UNSUPPORTED;
}

int rx_index_attach(rx_dbf *dbf, const char *cdxpath, rx_idx **out)
{
    (void)dbf;
    (void)cdxpath;
    (void)out;
    return R_ERR_DBF_UNSUPPORTED;
}

int rx_index_order(rx_dbf *dbf, const char *tag)
{
    (void)dbf;
    (void)tag;
    return R_ERR_DBF_UNSUPPORTED;
}

int rx_seek(rx_cursor *cur, rval *key, int *found)
{
    (void)cur;
    (void)key;
    (void)found;
    return R_ERR_DBF_UNSUPPORTED;
}

int rx_index_create(rx_dbf *dbf, const char *tag, const char *expr, int descending, int unique)
{
    (void)dbf;
    (void)tag;
    (void)expr;
    (void)descending;
    (void)unique;
    return R_ERR_DBF_UNSUPPORTED;
}

int rx_dbf_create(const char *path, const rx_fielddef *fields, size_t n, rx_dbf **out)
{
    (void)path;
    (void)fields;
    (void)n;
    (void)out;
    return R_ERR_DBF_UNSUPPORTED; /* M2 */
}
