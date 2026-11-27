/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file rlock.c
 * @brief fcntl(F_SETLK) byte-range advisory locking (spec \u00a78.5): the file
 *        lock lives at byte offset 0, record locks at 100000000 + recno
 *        (above typical file sizes so plain F_SETLK users never collide).
 *        Exclusive opens skip locking; shared opens lock per operation and
 *        obey SET REPROCESS for retry pacing. Locks are released on
 *        rx_unlock and implicitly at close (spec \u00a78.5).
 */
/* POSIX system headers come before project headers here: <fcntl.h> and
   <unistd.h> define R_OK; reynard/status.h owns it. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "data/data.h"
#include "util/util.h"

#define RX_LOCK_BASE 100000000L

static int track_lock(rx_dbf* dbf, uint32_t recno) {
    for (size_t i = 0; i < dbf->nlocks; i++) {
        if (dbf->locks[i] == recno) {
            return R_OK;
        }
    }
    if (dbf->nlocks == dbf->caplocks) {
        size_t nc = dbf->caplocks ? dbf->caplocks * 2 : 8;
        uint32_t* nl = realloc(dbf->locks, nc * sizeof(uint32_t));
        if (!nl) {
            RX_FATAL("out of memory");
        }
        dbf->locks = nl;
        dbf->caplocks = nc;
    }
    dbf->locks[dbf->nlocks++] = recno;
    return R_OK;
}

static void untrack_lock(rx_dbf* dbf, uint32_t recno) {
    for (size_t i = 0; i < dbf->nlocks; i++) {
        if (dbf->locks[i] == recno) {
            dbf->locks[i] = dbf->locks[dbf->nlocks - 1];
            dbf->nlocks--;
            return;
        }
    }
}

static int lock_range(rx_dbf* dbf, off_t off, int type) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = (short)type;
    fl.l_whence = SEEK_SET;
    fl.l_start = off;
    fl.l_len = 1;
    int retries = rx_data_reprocess();
    if (retries < 0) {
        retries = 600; /* AUTOMATIC: the default retry policy (spec \u00a77.4) */
    }
    for (;;) {
        if (fcntl(dbf->fd, F_SETLK, &fl) == 0) {
            return R_OK;
        }
        if (errno != EACCES && errno != EAGAIN) {
            return R_ERR_LOCK_PERM;
        }
        if (retries-- <= 0) {
            return R_ERR_LOCK_CONFLICT;
        }
        struct timespec ts = {0, 10 * 1000 * 1000}; /* 10 ms */
        nanosleep(&ts, NULL);
    }
}

static void unlock_range(rx_dbf* dbf, off_t off) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = off;
    fl.l_len = 1;
    fcntl(dbf->fd, F_SETLK, &fl); /* best effort */
}

int rx_rlock(rx_dbf* dbf, uint32_t recno) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->exclusive) {
        return track_lock(dbf, recno); /* exclusive open implies all locks */
    }
    if (dbf->readonly) {
        return R_ERR_LOCK_CONFLICT;
    }
    int rc = lock_range(dbf, (off_t)RX_LOCK_BASE + recno, F_WRLCK);
    if (rc != R_OK) {
        return rc;
    }
    return track_lock(dbf, recno);
}

int rx_flock(rx_dbf* dbf) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->exclusive) {
        dbf->flock_held = 1;
        return R_OK;
    }
    if (dbf->readonly) {
        return R_ERR_LOCK_CONFLICT;
    }
    int rc = lock_range(dbf, 0, F_WRLCK);
    if (rc == R_OK) {
        dbf->flock_held = 1;
    }
    return rc;
}

int rx_funlock(rx_dbf* dbf) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (dbf->flock_held) {
        unlock_range(dbf, 0);
        dbf->flock_held = 0;
    }
    return R_OK;
}

void rx_unlock(rx_dbf* dbf, uint32_t recno) {
    if (!dbf) {
        return;
    }
    if (recno == 0) {
        for (size_t i = 0; i < dbf->nlocks; i++) {
            unlock_range(dbf, (off_t)RX_LOCK_BASE + dbf->locks[i]);
        }
        dbf->nlocks = 0;
        rx_funlock(dbf);
        return;
    }
    unlock_range(dbf, (off_t)RX_LOCK_BASE + recno);
    untrack_lock(dbf, recno);
}

int rx_rlock_query(const rx_dbf* dbf, uint32_t recno) {
    if (!dbf) {
        return 0;
    }
    if (dbf->exclusive) {
        return 0; /* we hold everything */
    }
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = (off_t)RX_LOCK_BASE + recno;
    fl.l_len = 1;
    if (fcntl(dbf->fd, F_GETLK, &fl) != 0) {
        return 0;
    }
    return fl.l_type == F_UNLCK ? 0 : 1; /* held by another process */
}
