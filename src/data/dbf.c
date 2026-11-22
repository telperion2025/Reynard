/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file dbf.c
 * @brief DBF engine (spec \u00a78.2): tolerant read of dBase III+/IV, FoxPro and
 *        VFP-structural files, plus the M2 write path (create/append/put/
 *        flush/delete/pack/zap) with header maintenance, lazy memo files,
 *        fcntl locking (rlock.c) and transaction journaling (journal.c).
 *        Diagnostics on corruption \u2014 never silent best guesses.
 */
/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
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

/* ---- session state (spec \u00a78.1: only the work-area table is global; this
   pair is the documented session configuration set once from main) ---- */
static int g_test_mode = 0;
static int g_cp_mark = 0x01;
static int g_reprocess = 0;

void rx_data_session(int test_mode, int cp_mark)
{
    g_test_mode = test_mode;
    g_cp_mark = cp_mark ? cp_mark : 0x01;
}

int rx_data_default_mark(void)
{
    return g_cp_mark;
}

int rx_data_test_mode(void)
{
    return g_test_mode;
}

void rx_data_set_reprocess(int n)
{
    g_reprocess = n;
}

int rx_data_reprocess(void)
{
    return g_reprocess;
}

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

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* memo kind implied by the version byte (spec \u00a78.2/\u00a78.3) */
static int memo_kind_for(uint8_t version)
{
    switch (version) {
    case 0xF5:
    case 0x30:
    case 0x31:
    case 0x32:
        return RX_MEMO_FPT;
    default:
        return RX_MEMO_DBT;
    }
}

/* dbf path with the extension replaced by the memo extension */
static void memo_path_for(const char *path, int kind, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s", path);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) {
        *dot = '\0';
    }
    snprintf(out + strlen(out), outsz - strlen(out), ".%s", kind == RX_MEMO_FPT ? "FPT" : "DBT");
}

static void rjn_path_for(const char *path, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s", path);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) {
        *dot = '\0';
    }
    snprintf(out + strlen(out), outsz - strlen(out), ".RJN");
}

/* stamp the header last-update date: frozen in -t, real date otherwise */
static void touch_date(rx_dbf *dbf)
{
    if (g_test_mode) {
        dbf->hdr_yy = 100;
        dbf->hdr_mm = 1;
        dbf->hdr_dd = 1;
        return;
    }
    struct stat st;
    time_t t = 0;
    if (stat(".", &st) == 0) {
        t = st.st_mtime;
    }
    struct tm tmv;
    if (localtime_r(&t, &tmv)) {
        dbf->hdr_yy = (uint8_t)(tmv.tm_year + 1900 - 1900);
        dbf->hdr_mm = (uint8_t)(tmv.tm_mon + 1);
        dbf->hdr_dd = (uint8_t)tmv.tm_mday;
    }
}

int rx_dbf_write_header(rx_dbf *dbf)
{
    uint8_t hdr[32];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = dbf->version;
    hdr[1] = dbf->hdr_yy;
    hdr[2] = dbf->hdr_mm;
    hdr[3] = dbf->hdr_dd;
    wr32(hdr + 4, dbf->nrec);
    wr16(hdr + 8, dbf->header_size);
    wr16(hdr + 10, dbf->rec_size);
    /* byte 28: production MDX flag (M6); byte 29: language driver;
       byte 30: incomplete-transaction stamp (spec \u00a78.6) */
    hdr[29] = dbf->lang_driver;
    hdr[30] = dbf->in_txn ? 1 : 0;
    ssize_t put = pwrite(dbf->fd, hdr, sizeof(hdr), 0);
    if (put != (ssize_t)sizeof(hdr)) {
        return put < 0 ? R_ERR_IO_WRITE : R_ERR_IO_WRITE;
    }
    return R_OK;
}

static int open_flags_for(int mode)
{
    return (mode & RD_READONLY) ? O_RDONLY : O_RDWR;
}

int rx_dbf_open(const char *path, int mode, rx_dbf **out)
{
    if (!path || !out) {
        return R_ERR_RANGE;
    }
    *out = NULL;
    int fd = open(path, open_flags_for(mode));
    if (fd < 0) {
        if (errno == ENOENT) {
            return R_ERR_IO_NOTFOUND;
        }
        if (errno == EACCES || errno == EBUSY) {
            return R_ERR_FILE_IN_USE;
        }
        if (errno == EROFS) {
            return R_ERR_IO_PERM;
        }
        return R_ERR_IO_OPEN;
    }
    rx_dbf *dbf = calloc(1, sizeof(*dbf));
    if (!dbf) {
        close(fd);
        RX_FATAL("out of memory");
    }
    dbf->fd = fd;
    dbf->mode = mode;
    dbf->readonly = (mode & RD_READONLY) != 0;
    dbf->exclusive = (mode & RD_EXCLUSIVE) != 0;
    dbf->memo_fd = -1;
    dbf->memo_ro = 0;
    dbf->rjn_fd = -1;
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
    dbf->hdr_yy = hdr[1];
    dbf->hdr_mm = hdr[2];
    dbf->hdr_dd = hdr[3];
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
        if (f->type == RFT_M || f->type == RFT_G || f->type == RFT_P) {
            dbf->has_memo = 1;
        }
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
    if (dbf->has_memo) {
        dbf->memo_kind = memo_kind_for(dbf->version);
        memo_path_for(path, dbf->memo_kind, dbf->memo_path, sizeof(dbf->memo_path));
    }
    rjn_path_for(path, dbf->rjn_path, sizeof(dbf->rjn_path));

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

    /* incomplete-transaction stamp: complete-or-rollback on open (\u00a78.6) */
    if (hdr[30] != 0) {
        int rc = rx_journal_recover(dbf);
        if (rc != R_OK) {
            rx_log(RX_LOG_WARN, "%s: transaction recovery failed (%d)", path, rc);
        }
        dbf->in_txn = 0;
        hdr[30] = 0;
        if (pwrite(fd, hdr, 1, 30) != 1) {
            /* non-fatal: the stamp is cleared on next write */
        }
    }
    *out = dbf;
    return R_OK;
}

int rx_dbf_close(rx_dbf *dbf)
{
    if (!dbf) {
        return R_OK;
    }
    if (dbf->in_txn) {
        rx_rollback(dbf); /* closing with an open transaction rolls back */
    }
    if (dbf->dirty && !dbf->readonly) {
        rx_dbf_write_rec(dbf);
        rx_dbf_write_header(dbf);
    }
    rx_unlock(dbf, 0); /* release every lock this process holds */
    if (dbf->memo_fd >= 0) {
        close(dbf->memo_fd);
    }
    if (dbf->fd >= 0) {
        close(dbf->fd);
    }
    free(dbf->fields);
    free(dbf->foff);
    free(dbf->recbuf);
    free(dbf->locks);
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
    if (dbf->dirty && !dbf->readonly) {
        int rc = rx_dbf_write_rec(dbf);
        if (rc != R_OK) {
            return rc;
        }
        rx_dbf_write_header(dbf);
    }
    rx_unlock(dbf, 0);
    close(dbf->fd);
    int fd = open(dbf->path, open_flags_for(newmode));
    if (fd < 0) {
        /* keep the old handle state consistent: reopen failed, table closed */
        dbf->fd = -1;
        return errno == ENOENT ? R_ERR_IO_NOTFOUND : R_ERR_IO_OPEN;
    }
    dbf->fd = fd;
    dbf->mode = newmode;
    dbf->readonly = (newmode & RD_READONLY) != 0;
    dbf->exclusive = (newmode & RD_EXCLUSIVE) != 0;
    /* re-read the header: another process may have appended records */
    uint8_t hdr[32];
    if (pread(fd, hdr, sizeof(hdr), 0) == (ssize_t)sizeof(hdr)) {
        dbf->nrec = rd32(hdr + 4);
        if (dbf->recno > dbf->nrec + 1) {
            dbf->recno = dbf->nrec + 1;
            dbf->eof = 1;
        }
    }
    return R_OK;
}

int rx_dbf_load_rec(rx_dbf *dbf)
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

int rx_dbf_write_rec(rx_dbf *dbf)
{
    if (dbf->recno < 1 || dbf->recno > dbf->nrec) {
        return R_ERR_DBF_RECNO;
    }
    off_t off = (off_t)dbf->header_size + (off_t)(dbf->recno - 1) * dbf->rec_size;
    ssize_t put = pwrite(dbf->fd, dbf->recbuf, dbf->rec_size, off);
    if (put != (ssize_t)dbf->rec_size) {
        return R_ERR_IO_WRITE;
    }
    dbf->dirty = 0;
    return R_OK;
}

int rx_dbf_read_raw(const rx_dbf *dbf, uint32_t recno, uint8_t *buf)
{
    if (!dbf || recno < 1 || recno > dbf->nrec) {
        return R_ERR_DBF_RECNO;
    }
    off_t off = (off_t)dbf->header_size + (off_t)(recno - 1) * dbf->rec_size;
    ssize_t got = pread(dbf->fd, buf, dbf->rec_size, off);
    if (got != (ssize_t)dbf->rec_size) {
        return got < 0 ? R_ERR_IO_READ : R_ERR_DBF_TRUNCATED;
    }
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
    dbf->dirty = 0;
    return rx_dbf_load_rec(dbf);
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
    /* logical position: past-end counts as nrec+1 (spec \u00a77.6 BOF/EOF rules) */
    int64_t base = dbf->eof ? (int64_t)dbf->nrec + 1 : (int64_t)dbf->recno;
    int64_t target = base + delta;
    if (target < 1) {
        dbf->recno = 1;
        dbf->bof = 1;
        dbf->eof = 0;
        dbf->dirty = 0;
        return rx_dbf_load_rec(dbf);
    }
    if (target > (int64_t)dbf->nrec) {
        dbf->recno = dbf->nrec + 1;
        dbf->eof = 1;
        dbf->bof = 0;
        dbf->deleted = 0;
        dbf->dirty = 0;
        return R_OK;
    }
    dbf->recno = (uint32_t)target;
    dbf->bof = 0;
    dbf->eof = 0;
    dbf->dirty = 0;
    return rx_dbf_load_rec(dbf);
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
    const rx_fielddef *f = &dbf->fields[fldidx];
    if (dbf->eof || dbf->nrec == 0) {
        /* FoxPro: reading fields past EOF yields blank values, not an error */
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
        case RFT_M:
            out->t = RX_T_M;
            out->u.memo.blk = 0;
            out->u.memo.loaded = calloc(1, sizeof(r_str));
            if (out->u.memo.loaded) {
                out->u.memo.loaded->utf8 = rx_strdup("");
                out->u.memo.loaded->n = 0;
            }
            break;
        default:
            break;
        }
        return R_OK;
    }
    int rc = rx_dbf_load_rec(dbf);
    if (rc != R_OK) {
        return rc;
    }
    const uint8_t *raw = dbf->recbuf + dbf->foff[fldidx];
    if (f->type == RFT_M) {
        char tmp[11];
        memcpy(tmp, raw, 10);
        tmp[10] = '\0';
        uint32_t blk = (uint32_t)strtoul(tmp, NULL, 10);
        out->t = RX_T_M;
        out->u.memo.blk = blk;
        out->u.memo.loaded = NULL;
        if (blk == 0) {
            r_str *e = calloc(1, sizeof(r_str));
            if (!e) {
                RX_FATAL("out of memory");
            }
            e->utf8 = rx_strdup("");
            e->n = 0;
            out->u.memo.loaded = e;
            return R_OK;
        }
        rc = rx_memo_open(dbf, 0);
        if (rc != R_OK) {
            return rc;
        }
        r_str *txt = NULL;
        if (dbf->memo_kind == RX_MEMO_FPT) {
            rc = rx_memo_fpt_read(dbf, blk, &txt);
        } else {
            rc = rx_memo_dbt_read(dbf, blk, &txt);
        }
        if (rc != R_OK) {
            return rc;
        }
        if (!txt) {
            txt = calloc(1, sizeof(r_str));
            if (!txt) {
                RX_FATAL("out of memory");
            }
            txt->utf8 = rx_strdup("");
            txt->n = 0;
        }
        out->u.memo.loaded = txt;
        return R_OK;
    }
    if (f->type == RFT_G || f->type == RFT_P) {
        char tmp[11];
        memcpy(tmp, raw, 10);
        tmp[10] = '\0';
        uint32_t blk = (uint32_t)strtoul(tmp, NULL, 10);
        out->t = RX_T_G;
        out->u.g.n = 0;
        out->u.g.bytes = NULL;
        if (blk == 0) {
            return R_OK;
        }
        rc = rx_memo_open(dbf, 0);
        if (rc != R_OK) {
            return rc;
        }
        if (dbf->memo_kind == RX_MEMO_FPT) {
            uint8_t *bytes = NULL;
            size_t n = 0;
            rc = rx_memo_fpt_read_raw(dbf, blk, &bytes, &n);
            if (rc == R_OK) {
                out->u.g.bytes = bytes;
                out->u.g.n = n;
            }
        }
        return rc;
    }
    return rx_field_decode(dbf, fldidx, raw, out);
}

int rx_dbf_put(rx_dbf *dbf, size_t fldidx, const rval *in)
{
    if (!dbf || !in) {
        return R_ERR_RANGE;
    }
    if (fldidx >= dbf->nfld) {
        return R_ERR_FIELD;
    }
    if (dbf->readonly) {
        return R_ERR_IO_PERM;
    }
    if (dbf->eof || dbf->nrec == 0 || dbf->recno < 1 || dbf->recno > dbf->nrec) {
        return R_ERR_DBF_RECNO;
    }
    if (!dbf->dirty) {
        int rc = rx_dbf_load_rec(dbf);
        if (rc != R_OK) {
            return rc;
        }
    }
    const rx_fielddef *f = &dbf->fields[fldidx];
    uint8_t *raw = dbf->recbuf + dbf->foff[fldidx];
    if (f->type == RFT_M) {
        /* memo write goes through the sidecar; store the block number */
        const char *txt = "";
        size_t tn = 0;
        if (in->t == RX_T_C) {
            txt = in->u.c.utf8 ? in->u.c.utf8 : "";
            tn = in->u.c.n;
        } else if (in->t == RX_T_M && in->u.memo.loaded) {
            txt = in->u.memo.loaded->utf8 ? in->u.memo.loaded->utf8 : "";
            tn = in->u.memo.loaded->n;
        } else if (in->t == RX_T_U) {
            txt = "";
            tn = 0;
        } else {
            return R_ERR_TYPE; /* memos take character values only */
        }
        char tmp[11];
        memcpy(tmp, raw, 10);
        tmp[10] = '\0';
        uint32_t old_blk = (uint32_t)strtoul(tmp, NULL, 10);
        int rc = rx_memo_open(dbf, 1);
        if (rc != R_OK) {
            return rc;
        }
        uint32_t new_blk = old_blk;
        if (dbf->memo_kind == RX_MEMO_FPT) {
            rc = rx_memo_fpt_write(dbf, txt, tn, old_blk, &new_blk);
        } else {
            rc = rx_memo_dbt_write(dbf, txt, tn, old_blk, &new_blk);
        }
        if (rc != R_OK) {
            return rc;
        }
        char blkstr[11];
        snprintf(blkstr, sizeof(blkstr), "%10u", new_blk);
        memcpy(raw, blkstr, 10);
        dbf->dirty = 1;
        return R_OK;
    }
    if (f->type == RFT_G || f->type == RFT_P) {
        return R_ERR_DBF_UNSUPPORTED; /* read-only in v1.0 (spec \u00a78.2) */
    }
    int rc = rx_field_encode(dbf, fldidx, in, raw);
    if (rc == R_OK) {
        dbf->dirty = 1;
    }
    return rc;
}

int rx_flush(rx_dbf *dbf)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->readonly) {
        return R_OK; /* nothing can be dirty */
    }
    if (dbf->dirty) {
        int rc = rx_journal_rec(dbf, dbf->recno);
        if (rc != R_OK) {
            return rc;
        }
        rc = rx_dbf_write_rec(dbf);
        if (rc != R_OK) {
            return rc;
        }
        if (!g_test_mode) {
            touch_date(dbf);
        }
        return rx_dbf_write_header(dbf);
    }
    return R_OK;
}

/* ---- M2 write operations ---- */

int rx_dbf_append(rx_dbf *dbf, uint32_t *recno)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->readonly) {
        return R_ERR_IO_PERM;
    }
    off_t rec_off = (off_t)dbf->header_size + (off_t)dbf->nrec * dbf->rec_size;
    /* ensure the EOF marker sits where the new record will go */
    struct stat st;
    off_t size = 0;
    if (fstat(dbf->fd, &st) == 0) {
        size = st.st_size;
    }
    if (size < rec_off) {
        return R_ERR_DBF_TRUNCATED;
    }
    if (size == rec_off) {
        uint8_t eofm = 0x1A;
        if (pwrite(dbf->fd, &eofm, 1, rec_off) != 1) {
            return R_ERR_IO_WRITE;
        }
    }
    uint8_t *blank = calloc(1, dbf->rec_size);
    if (!blank) {
        RX_FATAL("out of memory");
    }
    blank[0] = 0x20;
    for (size_t i = 1; i < dbf->rec_size; i++) {
        blank[i] = ' ';
    }
    if (pwrite(dbf->fd, blank, dbf->rec_size, rec_off) != (ssize_t)dbf->rec_size) {
        free(blank);
        return R_ERR_IO_WRITE;
    }
    uint8_t eofm = 0x1A;
    if (pwrite(dbf->fd, &eofm, 1, rec_off + dbf->rec_size) != 1) {
        free(blank);
        return R_ERR_IO_WRITE;
    }
    if (dbf->in_txn) {
        int rc = rx_journal_header(dbf);
        if (rc != R_OK) {
            free(blank);
            return rc;
        }
        rc = rx_journal_append(dbf, dbf->nrec + 1);
        if (rc != R_OK) {
            free(blank);
            return rc;
        }
    }
    dbf->nrec++;
    memcpy(dbf->recbuf, blank, dbf->rec_size);
    free(blank);
    dbf->recno = dbf->nrec;
    dbf->bof = 0;
    dbf->eof = 0;
    dbf->deleted = 0;
    dbf->dirty = 0;
    if (!g_test_mode) {
        touch_date(dbf);
    }
    int rc = rx_dbf_write_header(dbf);
    if (rc == R_OK && recno) {
        *recno = dbf->nrec;
    }
    return rc;
}

int rx_dbf_put_raw(rx_dbf *dbf, const uint8_t *raw)
{
    if (!dbf || !raw) {
        return R_ERR_RANGE;
    }
    if (dbf->readonly) {
        return R_ERR_IO_PERM;
    }
    if (dbf->recno < 1 || dbf->recno > dbf->nrec) {
        return R_ERR_DBF_RECNO;
    }
    memcpy(dbf->recbuf, raw, dbf->rec_size);
    dbf->deleted = dbf->recbuf[0] == 0x2A;
    dbf->dirty = 1;
    return R_OK;
}

int rx_dbf_set_deleted(rx_dbf *dbf, int del)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->readonly) {
        return R_ERR_IO_PERM;
    }
    if (dbf->eof || dbf->recno < 1 || dbf->recno > dbf->nrec) {
        return R_ERR_DBF_RECNO;
    }
    if (!dbf->dirty) {
        int rc = rx_dbf_load_rec(dbf);
        if (rc != R_OK) {
            return rc;
        }
    }
    int rc = rx_journal_rec(dbf, dbf->recno);
    if (rc != R_OK) {
        return rc;
    }
    dbf->recbuf[0] = del ? 0x2A : 0x20;
    dbf->deleted = del ? 1 : 0;
    rc = rx_dbf_write_rec(dbf);
    if (rc != R_OK) {
        return rc;
    }
    if (!g_test_mode) {
        touch_date(dbf);
    }
    return rx_dbf_write_header(dbf);
}

int rx_dbf_pack(rx_dbf *dbf)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->readonly) {
        return R_ERR_IO_PERM;
    }
    if (!dbf->exclusive) {
        return R_ERR_FILE_IN_USE; /* FoxPro: PACK requires exclusive use */
    }
    if (dbf->in_txn) {
        return R_ERR_DBF_UNSUPPORTED; /* documented: no PACK inside a transaction */
    }
    if (dbf->dirty) {
        int rc = rx_dbf_write_rec(dbf);
        if (rc != R_OK) {
            return rc;
        }
    }
    uint32_t nrec = dbf->nrec;
    uint8_t *all = NULL;
    if (nrec) {
        all = malloc((size_t)nrec * dbf->rec_size);
        if (!all) {
            RX_FATAL("out of memory");
        }
        for (uint32_t r = 1; r <= nrec; r++) {
            int rc = rx_dbf_read_raw(dbf, r, all + (size_t)(r - 1) * dbf->rec_size);
            if (rc != R_OK) {
                free(all);
                return rc;
            }
        }
    }
    /* memo compaction: snapshot live memos, reset the sidecar, rewrite */
    typedef struct {
        size_t recidx;
        size_t fldidx;
        r_str *txt;
    } memo_snap;
    memo_snap *snaps = NULL;
    size_t nsnap = 0;
    if (dbf->has_memo) {
        int rc = rx_memo_open(dbf, 1);
        if (rc != R_OK) {
            free(all);
            return rc;
        }
        snaps = calloc(nrec ? nrec : 1, sizeof(memo_snap));
        if (!snaps) {
            free(all);
            RX_FATAL("out of memory");
        }
        for (uint32_t r = 1; r <= nrec; r++) {
            const uint8_t *rec = all + (size_t)(r - 1) * dbf->rec_size;
            if (rec[0] == 0x2A) {
                continue;
            }
            for (size_t fi = 0; fi < dbf->nfld; fi++) {
                const rx_fielddef *f = &dbf->fields[fi];
                if (f->type != RFT_M && f->type != RFT_G && f->type != RFT_P) {
                    continue;
                }
                char tmp[11];
                memcpy(tmp, rec + dbf->foff[fi], 10);
                tmp[10] = '\0';
                uint32_t blk = (uint32_t)strtoul(tmp, NULL, 10);
                if (blk == 0) {
                    continue;
                }
                r_str *txt = NULL;
                if (dbf->memo_kind == RX_MEMO_FPT) {
                    rc = rx_memo_fpt_read(dbf, blk, &txt);
                } else {
                    rc = rx_memo_dbt_read(dbf, blk, &txt);
                }
                if (rc != R_OK) {
                    for (size_t k = 0; k < nsnap; k++) {
                        free(snaps[k].txt->utf8);
                        free(snaps[k].txt);
                    }
                    free(snaps);
                    free(all);
                    return rc;
                }
                if (txt) {
                    snaps[nsnap].recidx = r - 1;
                    snaps[nsnap].fldidx = fi;
                    snaps[nsnap].txt = txt;
                    nsnap++;
                }
            }
        }
        /* reset the memo file to its header block */
        if (rx_ftruncate(dbf->memo_fd, 512) != 0) {
            for (size_t k = 0; k < nsnap; k++) {
                free(snaps[k].txt->utf8);
                free(snaps[k].txt);
            }
            free(snaps);
            free(all);
            return R_ERR_IO_WRITE;
        }
        if (dbf->memo_kind == RX_MEMO_FPT) {
            uint32_t bs = dbf->memo_bs ? dbf->memo_bs : 64;
            uint8_t h[512];
            memset(h, 0, sizeof(h));
            uint32_t next = (512 + bs - 1) / bs;
            h[0] = (uint8_t)(next >> 24);
            h[1] = (uint8_t)(next >> 16);
            h[2] = (uint8_t)(next >> 8);
            h[3] = (uint8_t)next;
            h[6] = (uint8_t)(bs >> 8);
            h[7] = (uint8_t)bs;
            if (pwrite(dbf->memo_fd, h, sizeof(h), 0) != (ssize_t)sizeof(h)) {
                for (size_t k = 0; k < nsnap; k++) {
                    free(snaps[k].txt->utf8);
                    free(snaps[k].txt);
                }
                free(snaps);
                free(all);
                return R_ERR_IO_WRITE;
            }
            dbf->memo_next = next;
        } else {
            uint8_t h[512];
            memset(h, 0, sizeof(h));
            wr32(h, 1);
            if (pwrite(dbf->memo_fd, h, sizeof(h), 0) != (ssize_t)sizeof(h)) {
                for (size_t k = 0; k < nsnap; k++) {
                    free(snaps[k].txt->utf8);
                    free(snaps[k].txt);
                }
                free(snaps);
                free(all);
                return R_ERR_IO_WRITE;
            }
            dbf->memo_next = 1;
        }
        /* rewrite live memos into the fresh sidecar and patch the records */
        for (size_t k = 0; k < nsnap; k++) {
            uint8_t *rec = all + snaps[k].recidx * dbf->rec_size;
            uint32_t newblk = 0;
            int rc;
            if (dbf->memo_kind == RX_MEMO_FPT) {
                rc = rx_memo_fpt_write(dbf, snaps[k].txt->utf8 ? snaps[k].txt->utf8 : "",
                                       snaps[k].txt->n, 0, &newblk);
            } else {
                rc = rx_memo_dbt_write(dbf, snaps[k].txt->utf8 ? snaps[k].txt->utf8 : "",
                                       snaps[k].txt->n, 0, &newblk);
            }
            if (rc != R_OK) {
                for (size_t j = k; j < nsnap; j++) {
                    free(snaps[j].txt->utf8);
                    free(snaps[j].txt);
                }
                free(snaps);
                free(all);
                return rc;
            }
            char blkstr[11];
            snprintf(blkstr, sizeof(blkstr), "%10u", newblk);
            memcpy(rec + dbf->foff[snaps[k].fldidx], blkstr, 10);
            free(snaps[k].txt->utf8);
            free(snaps[k].txt);
        }
        free(snaps);
    }
    /* rewrite the DBF body without deleted records */
    uint32_t kept = 0;
    for (uint32_t r = 1; r <= nrec; r++) {
        const uint8_t *rec = all + (size_t)(r - 1) * dbf->rec_size;
        if (rec[0] == 0x2A) {
            continue;
        }
        if (kept != r - 1) {
            memmove(all + (size_t)kept * dbf->rec_size, rec, dbf->rec_size);
        }
        kept++;
    }
    if (rx_ftruncate(dbf->fd, dbf->header_size) != 0) {
        free(all);
        return R_ERR_IO_WRITE;
    }
    if (kept) {
        if (pwrite(dbf->fd, all, (size_t)kept * dbf->rec_size, dbf->header_size) !=
            (ssize_t)((size_t)kept * dbf->rec_size)) {
            free(all);
            return R_ERR_IO_WRITE;
        }
    }
    uint8_t eofm = 0x1A;
    if (pwrite(dbf->fd, &eofm, 1, (off_t)dbf->header_size + (off_t)kept * dbf->rec_size) != 1) {
        free(all);
        return R_ERR_IO_WRITE;
    }
    free(all);
    dbf->nrec = kept;
    dbf->dirty = 0;
    if (kept == 0) {
        dbf->recno = 1;
        dbf->bof = 1;
        dbf->eof = 1;
    } else {
        dbf->recno = 1;
        dbf->bof = 0;
        dbf->eof = 0;
        int rc = rx_dbf_load_rec(dbf);
        if (rc != R_OK) {
            return rc;
        }
    }
    if (!g_test_mode) {
        touch_date(dbf);
    }
    return rx_dbf_write_header(dbf);
}

int rx_dbf_zap(rx_dbf *dbf)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->readonly) {
        return R_ERR_IO_PERM;
    }
    if (!dbf->exclusive) {
        return R_ERR_FILE_IN_USE;
    }
    if (dbf->in_txn) {
        return R_ERR_DBF_UNSUPPORTED;
    }
    if (dbf->has_memo) {
        int rc = rx_memo_open(dbf, 1);
        if (rc != R_OK) {
            return rc;
        }
        if (rx_ftruncate(dbf->memo_fd, 512) != 0) {
            return R_ERR_IO_WRITE;
        }
        uint8_t h[512];
        memset(h, 0, sizeof(h));
        if (dbf->memo_kind == RX_MEMO_FPT) {
            uint32_t bs = dbf->memo_bs ? dbf->memo_bs : 64;
            uint32_t next = (512 + bs - 1) / bs;
            h[0] = (uint8_t)(next >> 24);
            h[1] = (uint8_t)(next >> 16);
            h[2] = (uint8_t)(next >> 8);
            h[3] = (uint8_t)next;
            h[6] = (uint8_t)(bs >> 8);
            h[7] = (uint8_t)bs;
            dbf->memo_next = next;
        } else {
            wr32(h, 1);
            dbf->memo_next = 1;
        }
        if (pwrite(dbf->memo_fd, h, sizeof(h), 0) != (ssize_t)sizeof(h)) {
            return R_ERR_IO_WRITE;
        }
    }
    if (rx_ftruncate(dbf->fd, dbf->header_size) != 0) {
        return R_ERR_IO_WRITE;
    }
    uint8_t eofm = 0x1A;
    if (pwrite(dbf->fd, &eofm, 1, dbf->header_size) != 1) {
        return R_ERR_IO_WRITE;
    }
    dbf->nrec = 0;
    dbf->recno = 1;
    dbf->bof = 1;
    dbf->eof = 1;
    dbf->deleted = 0;
    dbf->dirty = 0;
    if (!g_test_mode) {
        touch_date(dbf);
    }
    return rx_dbf_write_header(dbf);
}

void rx_dbf_set_found(rx_dbf *dbf, int found)
{
    if (dbf) {
        dbf->found = found;
    }
}

int rx_dbf_found(const rx_dbf *dbf)
{
    return dbf ? dbf->found : 0;
}

int rx_dbf_lupdate(const rx_dbf *dbf, int *y, int *m, int *d)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    *y = 1900 + dbf->hdr_yy;
    *m = dbf->hdr_mm;
    *d = dbf->hdr_dd;
    return R_OK;
}

/* ---- memo sidecar open (lazy) ---- */

int rx_memo_open(rx_dbf *dbf, int for_write)
{
    if (!dbf->has_memo) {
        return R_ERR_DBF_MEMO;
    }
    if (dbf->memo_fd >= 0) {
        if (for_write && dbf->memo_ro && !dbf->readonly) {
            /* upgrade the sidecar handle to read-write for the first write */
            close(dbf->memo_fd);
            dbf->memo_fd = -1;
            dbf->memo_ro = 0;
        } else {
            return R_OK;
        }
    }
    int flags = (for_write && !dbf->readonly) ? O_RDWR : O_RDONLY;
    int fd = open(dbf->memo_path, flags);
    if (fd < 0 && errno == ENOENT) {
        /* tolerate lowercase sidecar names (memo.fpt vs memo.FPT) */
        char lower[264];
        snprintf(lower, sizeof(lower), "%s", dbf->memo_path);
        for (char *q = lower; *q; q++) {
            if (*q >= 'A' && *q <= 'Z') {
                *q = (char)(*q - 'A' + 'a');
            }
        }
        fd = open(lower, flags);
        if (fd >= 0) {
            snprintf(dbf->memo_path, sizeof(dbf->memo_path), "%s", lower);
        }
    }
    if (fd < 0 && errno == ENOENT && for_write && !dbf->readonly) {
        fd = open(dbf->memo_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            uint8_t h[512];
            memset(h, 0, sizeof(h));
            if (dbf->memo_kind == RX_MEMO_FPT) {
                uint32_t bs = 64;
                uint32_t next = (512 + bs - 1) / bs;
                h[0] = (uint8_t)(next >> 24);
                h[1] = (uint8_t)(next >> 16);
                h[2] = (uint8_t)(next >> 8);
                h[3] = (uint8_t)next;
                h[6] = (uint8_t)(bs >> 8);
                h[7] = (uint8_t)bs;
                dbf->memo_next = next;
                dbf->memo_bs = bs;
            } else {
                wr32(h, 1);
                dbf->memo_next = 1;
            }
            if (write(fd, h, sizeof(h)) != (ssize_t)sizeof(h)) {
                close(fd);
                return R_ERR_IO_WRITE;
            }
        }
    }
    if (fd < 0) {
        return errno == ENOENT ? R_ERR_DBF_MEMO : R_ERR_IO_OPEN;
    }
    dbf->memo_fd = fd;
    dbf->memo_ro = (flags == O_RDONLY);
    /* read the memo header */
    uint8_t h[512];
    ssize_t got = pread(fd, h, sizeof(h), 0);
    if (got < 8) {
        if (for_write && !dbf->readonly && got >= 0) {
            /* freshly created or empty file: initialize header */
            memset(h, 0, sizeof(h));
            if (dbf->memo_kind == RX_MEMO_FPT) {
                uint32_t bs = 64;
                uint32_t next = (512 + bs - 1) / bs;
                h[0] = (uint8_t)(next >> 24);
                h[1] = (uint8_t)(next >> 16);
                h[2] = (uint8_t)(next >> 8);
                h[3] = (uint8_t)next;
                h[6] = (uint8_t)(bs >> 8);
                h[7] = (uint8_t)bs;
                dbf->memo_next = next;
                dbf->memo_bs = bs;
            } else {
                wr32(h, 1);
                dbf->memo_next = 1;
            }
            if (pwrite(fd, h, sizeof(h), 0) != (ssize_t)sizeof(h)) {
                close(fd);
                dbf->memo_fd = -1;
                return R_ERR_IO_WRITE;
            }
            return R_OK;
        }
        close(fd);
        return R_ERR_DBF_MEMO;
    }
    if (dbf->memo_kind == RX_MEMO_FPT) {
        uint32_t next = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) |
                        h[3];
        uint32_t bs = ((uint32_t)h[6] << 8) | h[7];
        dbf->memo_bs = bs ? bs : 64;
        dbf->memo_next = next ? next : (512 + dbf->memo_bs - 1) / dbf->memo_bs;
    } else {
        uint32_t next = rd32(h);
        dbf->memo_next = next ? next : 1;
    }
    return R_OK;
}

/* ---- header introspection ---- */

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

/* ---- locks / transactions / indexes live in rlock.c / journal.c / M6 ---- */

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

int rx_dbf_is_exclusive(const rx_dbf *dbf)
{
    return dbf ? dbf->exclusive : 0;
}

int rx_dbf_is_readonly(const rx_dbf *dbf)
{
    return dbf ? dbf->readonly : 1;
}

int rx_dbf_version_mark(const rx_dbf *dbf, uint8_t *version, uint8_t *mark)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (version) {
        *version = dbf->version;
    }
    if (mark) {
        *mark = dbf->lang_driver;
    }
    return R_OK;
}

const uint8_t *rx_dbf_recbuf(const rx_dbf *dbf)
{
    return dbf ? dbf->recbuf : NULL;
}

int rx_dbf_recsize(const rx_dbf *dbf, uint16_t *out)
{
    if (!dbf) {
        return R_ERR_RANGE;
    }
    *out = dbf->rec_size;
    return R_OK;
}
