/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file cdx.c
 * @brief Index engine (spec §8.4, M6): structural CDX compound indexes and
 *        standalone IDX files, plus a read-only NTX/NDX importer.
 *
 * File layout (docs/formats.md §4): 500-byte blocks, little-endian. Block 0
 * is the tag-directory root; tag headers chain across directory blocks and
 * store tag name, key expression, FOR filter, flags (descending/unique) and
 * page counts. Each tag owns a B+tree of 500-byte nodes (type root/internal/
 * leaf, key count, left/right sibling pointers); leaf entries are
 * (key, recno), character keys have trailing spaces stripped, equal keys
 * batch in sibling order. A file rewrite is deterministic for a given entry
 * set, so the CDX round-trip gate can require byte-identical output.
 *
 * The live index is the in-memory sorted entry array per tag; seeks run
 * there, and the on-disk tree is (re)generated on flush. Structural CDX
 * writes happen only under exclusive open (FoxPro rule, spec §8.4).
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cdx_internal.h"
#include "data.h"

#include "util/util.h"

#define _POSIX_C_SOURCE 200809L

#include "cdx_internal.h"
#include "data.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "util/util.h"

/* ============================ key encoding ============================ */

static void put_u64(strbuf* b, uint64_t u) {
    for (int i = 7; i >= 0; i--) {
        sb_putc(b, (char)(u >> (8 * i)));
    }
}

int rx_key_encode(const rval* v, strbuf* out) {
    switch (v->t) {
    case RX_T_U:
        sb_putc(out, 'U');
        break;
    case RX_T_C: {
        sb_putc(out, 'C');
        const char* s = v->u.c.utf8 ? v->u.c.utf8 : "";
        size_t n = v->u.c.utf8 ? v->u.c.n : 0;
        while (n && s[n - 1] == ' ') {
            n--; /* character keys have trailing spaces stripped */
        }
        for (size_t i = 0; i < n; i++) {
            sb_putc(out, (char)toupper((unsigned char)s[i])); /* MACHINE collation */
        }
        break;
    }
    case RX_T_N:
    case RX_T_I:
    case RX_T_Y:
    case RX_T_B: {
        double d;
        if (v->t == RX_T_N || v->t == RX_T_B) {
            d = v->t == RX_T_N ? v->u.n.d : v->u.b;
        } else if (v->t == RX_T_I) {
            d = v->u.i;
        } else {
            d = v->u.y / 10000.0;
        }
        sb_putc(out, 'N');
        uint64_t u;
        memcpy(&u, &d, 8);
        u ^= 0x8000000000000000ull; /* sign-flip: doubles sort as unsigned */
        put_u64(out, u);
        break;
    }
    case RX_T_D:
    case RX_T_T: {
        sb_putc(out, 'D');
        int yy = v->t == RX_T_D ? v->u.d.y : v->u.tm.d.y;
        int mm = v->t == RX_T_D ? v->u.d.m : v->u.tm.d.m;
        int dd = v->t == RX_T_D ? v->u.d.d : v->u.tm.d.d;
        char tmp[8];
        if (yy == 0) {
            memset(tmp, ' ', 8); /* empty date sorts first */
        } else {
            if (yy < 0)
                yy = 0;
            if (yy > 9999)
                yy = 9999;
            if (mm < 1)
                mm = 1;
            if (mm > 12)
                mm = 12;
            if (dd < 1)
                dd = 1;
            if (dd > 31)
                dd = 31;
            tmp[0] = (char)('0' + yy / 1000);
            tmp[1] = (char)('0' + (yy / 100) % 10);
            tmp[2] = (char)('0' + (yy / 10) % 10);
            tmp[3] = (char)('0' + yy % 10);
            tmp[4] = (char)('0' + mm / 10);
            tmp[5] = (char)('0' + mm % 10);
            tmp[6] = (char)('0' + dd / 10);
            tmp[7] = (char)('0' + dd % 10);
        }
        sb_put(out, tmp, 8);
        break;
    }
    case RX_T_L:
        sb_putc(out, 'L');
        sb_putc(out, (char)(v->u.l == L_T ? 1 : v->u.l == L_F ? 0 : 2));
        break;
    case RX_T_M:
    case RX_T_G: {
        /* memo/general keys index as their text/bytes uppercased like C */
        sb_putc(out, 'C');
        const char* s = NULL;
        size_t n = 0;
        if (v->t == RX_T_M && v->u.memo.loaded) {
            s = v->u.memo.loaded->utf8;
            n = v->u.memo.loaded->n;
        } else if (v->t == RX_T_G && v->u.g.bytes) {
            s = (const char*)v->u.g.bytes;
            n = v->u.g.n;
        }
        if (s) {
            while (n && s[n - 1] == ' ') {
                n--;
            }
            for (size_t i = 0; i < n; i++) {
                sb_putc(out, (char)toupper((unsigned char)s[i]));
            }
        }
        break;
    }
    default:
        sb_putc(out, 'U');
        break;
    }
    return R_OK;
}

static int ent_cmp(const rx_ent* a, const rx_ent* b, int descending) {
    size_t m = a->klen < b->klen ? a->klen : b->klen;
    int c = memcmp(a->key, b->key, m);
    if (c == 0) {
        c = a->klen < b->klen ? -1 : a->klen > b->klen ? 1 : 0;
    }
    if (c == 0) {
        c = a->recno < b->recno ? -1 : a->recno > b->recno ? 1 : 0;
    }
    return descending ? -c : c;
}

/* ============================ in-memory tag ops ============================ */

static rx_tag* tag_get(rx_dbf* dbf, int fi, int ti) {
    if (!dbf || fi < 0 || ti < 0 || (size_t)fi >= dbf->nidxs) {
        return NULL;
    }
    rx_idx* ix = dbf->idxs[fi];
    if (!ix || (size_t)ti >= ix->ntags) {
        return NULL;
    }
    return &ix->tags[ti];
}

static void ent_free(rx_ent* e) {
    free(e->key);
    e->key = NULL;
}

void rx_cdx_tag_clear_ents(rx_tag* t) {
    for (size_t i = 0; i < t->n; i++) {
        ent_free(&t->ents[i]);
    }
    free(t->ents);
    t->ents = NULL;
    t->n = t->cap = 0;
    t->dirty = 1;
}

static int g_sort_desc; /* qsort context (the engine is single-threaded) */

static int ent_qsort_cmp(const void* a, const void* b) {
    return ent_cmp((const rx_ent*)a, (const rx_ent*)b, g_sort_desc);
}

/* sort ents if needed; for UNIQUE tags drop exact-duplicate keys (keeping
   the lowest recno, i.e. the first record that produced the key) */
void rx_cdx_tag_ensure_sorted(rx_tag* t) {
    if (t->sorted || t->n == 0) {
        t->sorted = 1;
        return;
    }
    g_sort_desc = (t->flags & RX_TAG_DESC) != 0;
    qsort(t->ents, t->n, sizeof(rx_ent), ent_qsort_cmp);
    if (t->flags & RX_TAG_UNIQUE) {
        size_t w = 0;
        for (size_t i = 0; i < t->n; i++) {
            if (w > 0) {
                rx_ent* pr = &t->ents[w - 1];
                rx_ent* c = &t->ents[i];
                size_t m = pr->klen < c->klen ? pr->klen : c->klen;
                int c2 = memcmp(pr->key, c->key, m);
                if (c2 == 0) {
                    c2 = pr->klen < c->klen ? -1 : pr->klen > c->klen ? 1 : 0;
                }
                if (c2 == 0) {
                    ent_free(&t->ents[i]); /* duplicate key: drop */
                    continue;
                }
            }
            t->ents[w++] = t->ents[i];
        }
        t->n = w;
    }
    t->sorted = 1;
}

int rx_cdx_tag_insert_sorted(rx_tag* t, const uint8_t* key, size_t klen, uint32_t recno) {
    /* O(1) append; the array is (re)sorted lazily on first read (seek /
       positional access / flush), so bulk builds of 100k+ keys stay linear */
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 64;
        rx_ent* ne = realloc(t->ents, nc * sizeof(rx_ent));
        if (!ne) {
            RX_FATAL("out of memory");
        }
        t->ents = ne;
        t->cap = nc;
    }
    rx_ent* en = &t->ents[t->n++];
    en->key = malloc(klen ? klen : 1);
    if (!en->key) {
        RX_FATAL("out of memory");
    }
    memcpy(en->key, key, klen);
    en->klen = klen;
    en->recno = recno;
    t->sorted = 0;
    t->dirty = 1;
    return R_OK;
}

/* ============================ low-level block I/O ============================ */

int rx_cdx_blk_read(int fd, uint32_t blk, uint8_t out[RX_CDX_BLOCK]) {
    off_t off = (off_t)blk * RX_CDX_BLOCK;
    size_t got = 0;
    while (got < RX_CDX_BLOCK) {
        ssize_t r = pread(fd, out + got, RX_CDX_BLOCK - got, off + (off_t)got);
        if (r <= 0) {
            return R_ERR_IO_READ;
        }
        got += (size_t)r;
    }
    return R_OK;
}

int rx_cdx_blk_write(int fd, uint32_t blk, const uint8_t buf[RX_CDX_BLOCK]) {
    off_t off = (off_t)blk * RX_CDX_BLOCK;
    size_t put = 0;
    while (put < RX_CDX_BLOCK) {
        ssize_t w = pwrite(fd, buf + put, RX_CDX_BLOCK - put, off + (off_t)put);
        if (w <= 0) {
            return R_ERR_IO_WRITE;
        }
        put += (size_t)w;
    }
    return R_OK;
}

uint16_t rx_cdx_get_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t rx_cdx_get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t rx_cdx_get_u64(const uint8_t* p) {
    uint64_t u = 0;
    for (int i = 7; i >= 0; i--) {
        u = (u << 8) | p[i];
    }
    return u;
}

static void put_u16b(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32b(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64b(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* ============================ CDX read ============================ */

#define RX_TAG_HDR_SIZE 236
#define RX_DIR_HDR_SIZE 24
#define RX_DIR_ENTRIES ((RX_CDX_BLOCK - RX_DIR_HDR_SIZE) / RX_TAG_HDR_SIZE)

static int cdx_read_dir(int fd, rx_idx* ix) {
    uint8_t buf[RX_CDX_BLOCK];
    uint32_t blk = 0;
    for (;;) {
        if (rx_cdx_blk_read(fd, blk, buf) != R_OK) {
            return R_ERR_IO_READ;
        }
        if (rx_cdx_get_u32(buf) != RX_CDX_SIG || rx_cdx_get_u16(buf + 4) != RX_CDX_VERSION) {
            return R_ERR_CDX_VARIANT;
        }
        uint16_t n = rx_cdx_get_u16(buf + 6);
        uint32_t next = rx_cdx_get_u32(buf + 8);
        for (uint16_t i = 0; i < n && i < RX_DIR_ENTRIES; i++) {
            const uint8_t* h = buf + RX_DIR_HDR_SIZE + (size_t)i * RX_TAG_HDR_SIZE;
            if (ix->ntags == ix->captags) {
                size_t nc = ix->captags ? ix->captags * 2 : 8;
                rx_tag* nt = realloc(ix->tags, nc * sizeof(rx_tag));
                if (!nt) {
                    RX_FATAL("out of memory");
                }
                ix->tags = nt;
                ix->captags = nc;
            }
            rx_tag* t = &ix->tags[ix->ntags++];
            memset(t, 0, sizeof(*t));
            memcpy(t->name, h, 11);
            t->name[11] = '\0';
            memcpy(t->key_expr, h + 12, 128);
            t->key_expr[128] = '\0';
            memcpy(t->for_expr, h + 140, 64);
            t->for_expr[64] = '\0';
            t->flags = rx_cdx_get_u16(h + 204);
            /* h+206: root page, h+210: page count, h+214: key count (u64 at 222?) */
            (void)rx_cdx_get_u32(h + 206);
            t->n = 0;
            t->cap = 0;
            t->ents = NULL;
        }
        if (next == 0) {
            break;
        }
        blk = next;
        if (blk > 1000000) {
            return R_ERR_CDX_NODE;
        }
    }
    return R_OK;
}

/* read one tag's B+tree into t->ents (leaf order) */
static int cdx_read_tree(int fd, rx_tag* t, uint32_t root, uint32_t nkeys_expected) {
    uint8_t buf[RX_CDX_BLOCK];
    uint32_t cur = root;
    int rc = R_OK;
    /* iterative descent to leftmost leaf, then follow sibling chain */
    for (;;) {
        if (cur == 0 || cur > 10000000) {
            rc = R_ERR_CDX_NODE;
            break;
        }
        if (rx_cdx_blk_read(fd, cur, buf) != R_OK) {
            rc = R_ERR_IO_READ;
            break;
        }
        uint16_t type = rx_cdx_get_u16(buf);
        uint16_t nk = rx_cdx_get_u16(buf + 2);
        if (type == RX_NODE_LEAF) {
            break;
        }
        if (type != RX_NODE_ROOT && type != RX_NODE_INTERNAL) {
            rc = R_ERR_CDX_NODE;
            break;
        }
        /* descend to the leftmost child (header field at offset 12) */
        if (nk == 0) {
            rc = R_ERR_CDX_NODE;
            break;
        }
        cur = rx_cdx_get_u32(buf + 12);
    }
    if (rc == R_OK) {
        uint32_t page = cur;
        for (;;) {
            if (page == 0 || page > 10000000) {
                rc = R_ERR_CDX_NODE;
                break;
            }
            if (rx_cdx_blk_read(fd, page, buf) != R_OK) {
                rc = R_ERR_IO_READ;
                break;
            }
            uint16_t type = rx_cdx_get_u16(buf);
            uint16_t nk = rx_cdx_get_u16(buf + 2);
            if (type != RX_NODE_LEAF) {
                rc = R_ERR_CDX_NODE;
                break;
            }
            size_t off = 16;
            for (uint16_t i = 0; i < nk; i++) {
                if (off + 6 > RX_CDX_BLOCK) {
                    rc = R_ERR_CDX_NODE;
                    break;
                }
                uint16_t klen = rx_cdx_get_u16(buf + off);
                off += 2;
                if (off + klen + 4 > RX_CDX_BLOCK) {
                    rc = R_ERR_CDX_NODE;
                    break;
                }
                if (t->n == t->cap) {
                    size_t nc = t->cap ? t->cap * 2 : 64;
                    rx_ent* ne = realloc(t->ents, nc * sizeof(rx_ent));
                    if (!ne) {
                        RX_FATAL("out of memory");
                    }
                    t->ents = ne;
                    t->cap = nc;
                }
                rx_ent* en = &t->ents[t->n++];
                en->key = malloc(klen ? klen : 1);
                if (!en->key) {
                    RX_FATAL("out of memory");
                }
                memcpy(en->key, buf + off, klen);
                en->klen = klen;
                off += klen;
                en->recno = rx_cdx_get_u32(buf + off);
                off += 4;
            }
            if (rc != R_OK) {
                break;
            }
            uint32_t right = rx_cdx_get_u32(buf + 8);
            if (right == 0) {
                break;
            }
            page = right;
        }
    }
    (void)nkeys_expected;
    if (rc == R_OK) {
        t->dirty = 0;
    }
    return rc;
}

static int cdx_read_tag_trees(int fd, rx_idx* ix) {
    /* re-read dir to get root pages, then trees */
    uint8_t buf[RX_CDX_BLOCK];
    uint32_t blk = 0;
    size_t ti = 0;
    for (;;) {
        if (rx_cdx_blk_read(fd, blk, buf) != R_OK) {
            return R_ERR_IO_READ;
        }
        uint16_t n = rx_cdx_get_u16(buf + 6);
        uint32_t next = rx_cdx_get_u32(buf + 8);
        for (uint16_t i = 0; i < n && i < RX_DIR_ENTRIES && ti < ix->ntags; i++) {
            const uint8_t* h = buf + RX_DIR_HDR_SIZE + (size_t)i * RX_TAG_HDR_SIZE;
            uint32_t root = rx_cdx_get_u32(h + 206);
            uint64_t nk = rx_cdx_get_u64(h + 222);
            int rc = cdx_read_tree(fd, &ix->tags[ti], root, (uint32_t)nk);
            if (rc != R_OK) {
                return rc;
            }
            ti++;
        }
        if (next == 0) {
            break;
        }
        blk = next;
    }
    return R_OK;
}

/* ============================ CDX write (deterministic bulk build) ============================ */

void rx_cdx_wr_u16(uint8_t* p, uint16_t v) {
    put_u16b(p, v);
}
void rx_cdx_wr_u32(uint8_t* p, uint32_t v) {
    put_u32b(p, v);
}
void rx_cdx_wr_u64(uint8_t* p, uint64_t v) {
    put_u64b(p, v);
}

/* append a leaf entry; returns 0 on success, 1 when full */
static int leaf_put(uint8_t* buf, size_t* off, const rx_ent* e) {
    if (*off + 2 + e->klen + 4 > RX_CDX_BLOCK) {
        return 1;
    }
    rx_cdx_wr_u16(buf + *off, (uint16_t)e->klen);
    *off += 2;
    memcpy(buf + *off, e->key, e->klen);
    *off += e->klen;
    rx_cdx_wr_u32(buf + *off, e->recno);
    *off += 4;
    return 0;
}

static int internal_put(uint8_t* buf, size_t* off, const uint8_t* key, size_t klen,
                        uint32_t child) {
    if (*off + 2 + klen + 8 > RX_CDX_BLOCK) {
        return 1;
    }
    rx_cdx_wr_u16(buf + *off, (uint16_t)klen);
    *off += 2;
    memcpy(buf + *off, key, klen);
    *off += klen;
    rx_cdx_wr_u32(buf + *off, 0); /* recno unused in separators */
    *off += 4;
    rx_cdx_wr_u32(buf + *off, child);
    *off += 4;
    return 0;
}

/* Write tag t's tree starting at block *next; returns root page in *root_out.
 * Pages are allocated sequentially; the file is sized by the caller. */
int rx_cdx_write_tree(int fd, rx_tag* t, uint32_t* next, uint32_t* root_out) {
    /* deterministic bulk build. Node layout: u16 type @0, u16 key count @2,
       u32 left sibling @4, u32 right sibling @8, u32 leftmost child @12,
       entries from offset 16. Leaf entry: u16 klen, key, u32 recno.
       Internal entry: u16 klen, key (separator = first key of the child),
       u32 0, u32 child page. A single-leaf tree keeps LEAF type at the root
       page (the tag directory stores the root page). */
    size_t n = t->n;
    if (n == 0) {
        *root_out = 0;
        return R_OK;
    }
    typedef struct {
        uint32_t page;
        const rx_ent* first; /* first key of this node's subtree */
        size_t count;
    } nodeinfo;
    nodeinfo* level = NULL;
    size_t nlevel = 0, caplevel = 0;
    size_t i = 0;
    while (i < n) {
        if (nlevel == caplevel) {
            caplevel = caplevel ? caplevel * 2 : 64;
            nodeinfo* nl = realloc(level, caplevel * sizeof(nodeinfo));
            if (!nl) {
                RX_FATAL("out of memory");
            }
            level = nl;
        }
        uint32_t page = (*next)++;
        uint8_t buf[RX_CDX_BLOCK];
        memset(buf, 0, sizeof(buf));
        size_t off = 16, cnt = 0;
        const rx_ent* first = &t->ents[i];
        while (i < n) {
            size_t need = 2 + t->ents[i].klen + 4;
            if (cnt > 0 && off + need > RX_CDX_BLOCK) {
                break;
            }
            leaf_put(buf, &off, &t->ents[i]);
            cnt++;
            i++;
        }
        rx_cdx_wr_u16(buf, RX_NODE_LEAF);
        rx_cdx_wr_u16(buf + 2, (uint16_t)cnt);
        level[nlevel].page = page;
        level[nlevel].first = first;
        level[nlevel].count = cnt;
        nlevel++;
        if (rx_cdx_blk_write(fd, page, buf) != R_OK) {
            free(level);
            return R_ERR_IO_WRITE;
        }
    }
    for (size_t li = 0; li < nlevel; li++) {
        uint8_t buf[RX_CDX_BLOCK];
        rx_cdx_blk_read(fd, level[li].page, buf);
        rx_cdx_wr_u32(buf + 4, li > 0 ? level[li - 1].page : 0);
        rx_cdx_wr_u32(buf + 8, li + 1 < nlevel ? level[li + 1].page : 0);
        rx_cdx_wr_u32(buf + 12, 0);
        rx_cdx_blk_write(fd, level[li].page, buf);
    }
    if (nlevel == 1) {
        *root_out = level[0].page;
        free(level);
        return R_OK;
    }
    int leaf_level = 1;
    while (nlevel > 1) {
        nodeinfo* up = NULL;
        size_t nup = 0, capup = 0;
        size_t ci = 0;
        while (ci < nlevel) {
            if (nup == capup) {
                capup = capup ? capup * 2 : 64;
                nodeinfo* nu = realloc(up, capup * sizeof(nodeinfo));
                if (!nu) {
                    RX_FATAL("out of memory");
                }
                up = nu;
            }
            uint32_t page = (*next)++;
            uint8_t buf[RX_CDX_BLOCK];
            memset(buf, 0, sizeof(buf));
            size_t off = 16, cnt = 0;
            rx_cdx_wr_u32(buf + 12, level[ci].page); /* leftmost child */
            const rx_ent* first = level[ci].first;
            ci++;
            while (ci < nlevel) {
                const rx_ent* sep = level[ci].first;
                size_t need = 2 + sep->klen + 8;
                if (cnt > 0 && off + need > RX_CDX_BLOCK) {
                    break;
                }
                internal_put(buf, &off, sep->key, sep->klen, level[ci].page);
                cnt++;
                ci++;
            }
            rx_cdx_wr_u16(buf, RX_NODE_INTERNAL);
            rx_cdx_wr_u16(buf + 2, (uint16_t)cnt);
            up[nup].page = page;
            up[nup].first = first;
            up[nup].count = cnt + 1;
            nup++;
            if (rx_cdx_blk_write(fd, page, buf) != R_OK) {
                free(up);
                free(level);
                return R_ERR_IO_WRITE;
            }
        }
        for (size_t k = 0; k < nup; k++) {
            uint8_t buf[RX_CDX_BLOCK];
            rx_cdx_blk_read(fd, up[k].page, buf);
            rx_cdx_wr_u32(buf + 4, k > 0 ? up[k - 1].page : 0);
            rx_cdx_wr_u32(buf + 8, k + 1 < nup ? up[k + 1].page : 0);
            rx_cdx_blk_write(fd, up[k].page, buf);
        }
        free(level);
        level = up;
        nlevel = nup;
        leaf_level = 0;
    }
    if (!leaf_level) {
        uint8_t buf[RX_CDX_BLOCK];
        rx_cdx_blk_read(fd, level[0].page, buf);
        rx_cdx_wr_u16(buf, RX_NODE_ROOT);
        rx_cdx_blk_write(fd, level[0].page, buf);
    }
    *root_out = level[0].page;
    free(level);
    return R_OK;
}

static int cdx_write_file(rx_idx* ix) {
    int fd = open(ix->path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return R_ERR_IO_OPEN;
    }
    /* size the file: 1 dir block per RX_DIR_ENTRIES tags + tree pages */
    uint32_t next = 1;
    uint32_t dir_blocks = (uint32_t)((ix->ntags + RX_DIR_ENTRIES - 1) / RX_DIR_ENTRIES);
    if (dir_blocks < 1) {
        dir_blocks = 1;
    }
    next = dir_blocks;
    /* first pass: count pages per tag by building into a scratch fd? We
       build directly: compute page counts by simulating allocation */
    uint32_t* roots = calloc(ix->ntags ? ix->ntags : 1, sizeof(uint32_t));
    if (!roots) {
        RX_FATAL("out of memory");
    }
    /* allocate tree pages per tag */
    for (size_t i = 0; i < ix->ntags; i++) {
        rx_cdx_tag_ensure_sorted(&ix->tags[i]);
        int rc = rx_cdx_write_tree(fd, &ix->tags[i], &next, &roots[i]);
        if (rc != R_OK) {
            free(roots);
            close(fd);
            return rc;
        }
    }
    /* truncate to next blocks */
    if (ftruncate(fd, (off_t)next * RX_CDX_BLOCK) != 0) {
        free(roots);
        close(fd);
        return R_ERR_IO_WRITE;
    }
    /* write dir blocks */
    for (uint32_t d = 0; d < dir_blocks; d++) {
        uint8_t buf[RX_CDX_BLOCK];
        memset(buf, 0, sizeof(buf));
        rx_cdx_wr_u32(buf, RX_CDX_SIG);
        rx_cdx_wr_u16(buf + 4, RX_CDX_VERSION);
        size_t base = (size_t)d * RX_DIR_ENTRIES;
        size_t cnt = ix->ntags > base ? ix->ntags - base : 0;
        if (cnt > RX_DIR_ENTRIES) {
            cnt = RX_DIR_ENTRIES;
        }
        rx_cdx_wr_u16(buf + 6, (uint16_t)cnt);
        rx_cdx_wr_u32(buf + 8, d + 1 < dir_blocks ? d + 1 : 0);
        rx_cdx_wr_u32(buf + 12, next);
        rx_cdx_wr_u32(buf + 16, 0);
        for (size_t i = 0; i < cnt; i++) {
            rx_tag* t = &ix->tags[base + i];
            uint8_t* h = buf + RX_DIR_HDR_SIZE + i * RX_TAG_HDR_SIZE;
            memset(h, 0, RX_TAG_HDR_SIZE);
            memcpy(h, t->name, strlen(t->name) > 11 ? 11 : strlen(t->name));
            memcpy(h + 12, t->key_expr, strlen(t->key_expr) > 128 ? 128 : strlen(t->key_expr));
            memcpy(h + 140, t->for_expr, strlen(t->for_expr) > 64 ? 64 : strlen(t->for_expr));
            rx_cdx_wr_u16(h + 204, (uint16_t)t->flags);
            rx_cdx_wr_u32(h + 206, roots[base + i]);
            rx_cdx_wr_u32(h + 210, 0); /* page count: informational */
            rx_cdx_wr_u64(h + 222, (uint64_t)t->n);
        }
        int rc = rx_cdx_blk_write(fd, d, buf);
        if (rc != R_OK) {
            free(roots);
            close(fd);
            return rc;
        }
    }
    free(roots);
    close(fd);
    for (size_t i = 0; i < ix->ntags; i++) {
        ix->tags[i].dirty = 0;
    }
    return R_OK;
}

/* ============================ public API ============================ */

int rx_index_file_count(const rx_dbf* dbf) {
    return dbf ? (int)dbf->nidxs : 0;
}

int rx_index_file_path(const rx_dbf* dbf, int fi, const char** out) {
    if (!dbf || fi < 0 || (size_t)fi >= dbf->nidxs || !dbf->idxs[fi]) {
        return R_ERR_RANGE;
    }
    *out = dbf->idxs[fi]->path;
    return R_OK;
}

int rx_index_file_is_cdx(const rx_dbf* dbf, int fi) {
    if (!dbf || fi < 0 || (size_t)fi >= dbf->nidxs || !dbf->idxs[fi]) {
        return 0;
    }
    return dbf->idxs[fi]->is_cdx;
}

int rx_tag_count(const rx_dbf* dbf, int fi) {
    if (!dbf || fi < 0 || (size_t)fi >= dbf->nidxs || !dbf->idxs[fi]) {
        return 0;
    }
    return (int)dbf->idxs[fi]->ntags;
}

int rx_tag_info(const rx_dbf* dbf, int fi, int ti, const char** name, const char** key,
                int* flags) {
    rx_tag* t = tag_get((rx_dbf*)dbf, fi, ti);
    if (!t) {
        return R_ERR_CDX_TAG;
    }
    if (name) {
        *name = t->name;
    }
    if (key) {
        *key = t->key_expr;
    }
    if (flags) {
        *flags = t->flags;
    }
    return R_OK;
}

int rx_tag_for_expr(const rx_dbf* dbf, int fi, int ti, const char** for_expr) {
    rx_tag* t = tag_get((rx_dbf*)dbf, fi, ti);
    if (!t) {
        return R_ERR_CDX_TAG;
    }
    if (for_expr) {
        *for_expr = t->for_expr;
    }
    return R_OK;
}

int rx_index_order_set(rx_dbf* dbf, int fi, int ti) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (fi == -1 && ti == -1) {
        dbf->order_file = -1;
        dbf->order_tag = -1;
        return R_OK;
    }
    if (!tag_get(dbf, fi, ti)) {
        return R_ERR_CDX_TAG;
    }
    dbf->order_file = fi;
    dbf->order_tag = ti;
    return R_OK;
}

int rx_index_order_get(const rx_dbf* dbf, int* fi, int* ti) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    if (fi) {
        *fi = dbf->order_file;
    }
    if (ti) {
        *ti = dbf->order_tag;
    }
    return R_OK;
}

int rx_cdx_idx_add(rx_dbf* dbf, rx_idx* ix) {
    if (dbf->nidxs == dbf->capidxs) {
        size_t nc = dbf->capidxs ? dbf->capidxs * 2 : 4;
        rx_idx** ni = realloc(dbf->idxs, nc * sizeof(rx_idx*));
        if (!ni) {
            RX_FATAL("out of memory");
        }
        dbf->idxs = ni;
        dbf->capidxs = nc;
    }
    dbf->idxs[dbf->nidxs++] = ix;
    return R_OK;
}

int rx_cdx_idx_add_front(rx_dbf* dbf, rx_idx* ix) {
    if (dbf->nidxs == dbf->capidxs) {
        size_t nc = dbf->capidxs ? dbf->capidxs * 2 : 4;
        rx_idx** ni = realloc(dbf->idxs, nc * sizeof(rx_idx*));
        if (!ni) {
            RX_FATAL("out of memory");
        }
        dbf->idxs = ni;
        dbf->capidxs = nc;
    }
    memmove(&dbf->idxs[1], &dbf->idxs[0], dbf->nidxs * sizeof(rx_idx*));
    dbf->idxs[0] = ix;
    dbf->nidxs++;
    return R_OK;
}

rx_idx* rx_cdx_idx_new(const char* path, int is_cdx, int is_ntx) {
    rx_idx* ix = calloc(1, sizeof(rx_idx));
    if (!ix) {
        RX_FATAL("out of memory");
    }
    snprintf(ix->path, sizeof(ix->path), "%s", path);
    ix->fd = -1;
    ix->is_cdx = is_cdx;
    ix->is_ntx = is_ntx;
    ix->writable = is_ntx ? 0 : 1;
    return ix;
}

int rx_cdx_tag_push(rx_tag* t, const uint8_t* key, size_t klen, uint32_t recno) {
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 64;
        rx_ent* ne = realloc(t->ents, nc * sizeof(rx_ent));
        if (!ne) {
            RX_FATAL("out of memory");
        }
        t->ents = ne;
        t->cap = nc;
    }
    rx_ent* en = &t->ents[t->n++];
    en->key = malloc(klen ? klen : 1);
    if (!en->key) {
        RX_FATAL("out of memory");
    }
    memcpy(en->key, key, klen);
    en->klen = klen;
    en->recno = recno;
    t->dirty = 1;
    return R_OK;
}

static int tag_find(rx_idx* ix, const char* name) {
    for (size_t i = 0; i < ix->ntags; i++) {
        if (rx_strcasecmp(ix->tags[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int tag_add(rx_idx* ix, const char* name, const char* expr, const char* for_expr,
                   int flags) {
    int ex = tag_find(ix, name);
    if (ex >= 0) {
        rx_tag* t = &ix->tags[ex];
        rx_cdx_tag_clear_ents(t);
        snprintf(t->key_expr, sizeof(t->key_expr), "%s", expr ? expr : "");
        snprintf(t->for_expr, sizeof(t->for_expr), "%s", for_expr ? for_expr : "");
        t->flags = flags;
        return ex;
    }
    if (ix->ntags == ix->captags) {
        size_t nc = ix->captags ? ix->captags * 2 : 8;
        rx_tag* nt = realloc(ix->tags, nc * sizeof(rx_tag));
        if (!nt) {
            RX_FATAL("out of memory");
        }
        ix->tags = nt;
        ix->captags = nc;
    }
    rx_tag* t = &ix->tags[ix->ntags];
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "%s", name);
    for (char* c = t->name; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    snprintf(t->key_expr, sizeof(t->key_expr), "%s", expr ? expr : "");
    snprintf(t->for_expr, sizeof(t->for_expr), "%s", for_expr ? for_expr : "");
    t->flags = flags;
    t->dirty = 1;
    return (int)ix->ntags++;
}

int rx_index_create(rx_dbf* dbf, const char* tag, const char* cdx_path, const char* expr,
                    const char* for_expr, int flags, int additive) {
    (void)additive;
    if (!dbf || !tag || !expr) {
        return R_ERR_RANGE;
    }
    rx_idx* ix = NULL;
    int fi = -1;
    if (cdx_path) {
        for (size_t i = 0; i < dbf->nidxs; i++) {
            if (strcmp(dbf->idxs[i]->path, cdx_path) == 0) {
                ix = dbf->idxs[i];
                fi = (int)i;
                break;
            }
        }
        if (!ix) {
            int rc = rx_index_attach(dbf, cdx_path, &ix);
            if (rc != R_OK) {
                return rc;
            }
            fi = (int)dbf->nidxs - 1;
        }
    } else {
        /* structural CDX (same stem as the DBF): use the attached one, or
           create it in memory now (written on flush under exclusive open) */
        fi = -1;
        for (size_t i = 0; i < dbf->nidxs; i++) {
            if (dbf->idxs[i]->structural) {
                ix = dbf->idxs[i];
                fi = (int)i;
                break;
            }
        }
        if (fi < 0) {
            char spath[264];
            snprintf(spath, sizeof(spath), "%s", dbf->path);
            size_t l = strlen(spath);
            if (l >= 4 && rx_strcasecmp(spath + l - 4, ".dbf") == 0) {
                snprintf(spath + l - 4, sizeof(spath) - l + 4, ".cdx");
            } else {
                strncat(spath, ".cdx", sizeof(spath) - strlen(spath) - 1);
            }
            ix = rx_cdx_idx_new(spath, 1, 0);
            ix->structural = 1;
            ix->writable = dbf->exclusive && !dbf->readonly;
            rx_cdx_idx_add_front(dbf, ix);
            fi = 0;
        }
    }
    int ti = tag_add(ix, tag, expr, for_expr, flags);
    if (ti < 0) {
        return R_ERR_CDX_TAG;
    }
    if (dbf->order_file == -1) {
        dbf->order_file = fi;
        dbf->order_tag = ti;
    }
    return R_OK;
}

int rx_index_clear(rx_dbf* dbf, int fi, int ti) {
    rx_tag* t = tag_get(dbf, fi, ti);
    if (!t) {
        return R_ERR_CDX_TAG;
    }
    rx_cdx_tag_clear_ents(t);
    return R_OK;
}

int rx_index_insert(rx_dbf* dbf, int fi, int ti, const rval* key, uint32_t recno) {
    rx_tag* t = tag_get(dbf, fi, ti);
    if (!t || !key) {
        return R_ERR_CDX_TAG;
    }
    strbuf kb;
    sb_init(&kb);
    rx_key_encode(key, &kb);
    if (!kb.buf) {
        sb_putc(&kb, '\0');
        kb.len = 0;
    }
    int rc = rx_cdx_tag_insert_sorted(t, (const uint8_t*)kb.buf, kb.len, recno);
    sb_free(&kb);
    return rc;
}

int rx_index_delete_rec(rx_dbf* dbf, uint32_t recno) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    for (size_t f = 0; f < dbf->nidxs; f++) {
        rx_idx* ix = dbf->idxs[f];
        for (size_t ti = 0; ti < ix->ntags; ti++) {
            rx_tag* t = &ix->tags[ti];
            for (size_t i = 0; i < t->n; i++) {
                if (t->ents[i].recno == recno) {
                    ent_free(&t->ents[i]);
                    memmove(&t->ents[i], &t->ents[i + 1], (t->n - i - 1) * sizeof(rx_ent));
                    t->n--;
                    t->dirty = 1;
                    i--; /* recheck same slot */
                }
            }
        }
    }
    return R_OK;
}

int rx_index_seek_tag(rx_dbf* dbf, int fi, int ti, const rval* key, int exact, uint32_t* recno_out,
                      int* found) {
    rx_tag* t = tag_get(dbf, fi, ti);
    if (!t || !key) {
        return R_ERR_CDX_TAG;
    }
    rx_cdx_tag_ensure_sorted(t);
    strbuf kb;
    sb_init(&kb);
    rx_key_encode(key, &kb);
    if (!kb.buf) {
        sb_putc(&kb, '\0');
        kb.len = 0;
    }
    rx_ent probe;
    probe.key = (uint8_t*)kb.buf;
    probe.klen = kb.len;
    probe.recno = 0;
    int desc = (t->flags & RX_TAG_DESC) != 0;
    /* lower bound on key (recno=0 => first entry with key >= probe) */
    size_t lo = 0, hi = t->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        rx_ent* e = &t->ents[mid];
        size_t m = e->klen < probe.klen ? e->klen : probe.klen;
        int c = memcmp(e->key, probe.key, m);
        if (c == 0) {
            c = e->klen < probe.klen ? -1 : e->klen > probe.klen ? 1 : 0;
        }
        if (c == 0) {
            c = e->recno < probe.recno ? -1 : 1; /* probe.recno=0: key equal => e >= probe */
        }
        if (desc) {
            c = -c;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *found = 0;
    if (lo < t->n) {
        rx_ent* e = &t->ents[lo];
        if (exact) {
            size_t m = e->klen < probe.klen ? e->klen : probe.klen;
            int c = memcmp(e->key, probe.key, m);
            if (c == 0 && e->klen == probe.klen) {
                *found = 1;
            }
        } else {
            *found = 1;
        }
        if (*found && recno_out) {
            *recno_out = e->recno;
        }
    }
    sb_free(&kb); /* after the memcmp: probe.key aliases kb.buf */
    return R_OK;
}

int rx_index_seek(rx_dbf* dbf, const rval* key, int exact, uint32_t* recno_out, int* found) {
    if (!dbf || dbf->order_file < 0) {
        return R_ERR_CDX_TAG;
    }
    return rx_index_seek_tag(dbf, dbf->order_file, dbf->order_tag, key, exact, recno_out, found);
}

int rx_index_key_count(const rx_dbf* dbf, int fi, int ti, uint32_t* out) {
    rx_tag* t = tag_get((rx_dbf*)dbf, fi, ti);
    if (!t) {
        return R_ERR_CDX_TAG;
    }
    *out = (uint32_t)t->n;
    return R_OK;
}

int rx_index_recno_at(const rx_dbf* dbf, uint32_t pos, uint32_t* recno_out) {
    if (!dbf || dbf->order_file < 0) {
        return R_ERR_CDX_TAG;
    }
    rx_tag* t = tag_get((rx_dbf*)dbf, dbf->order_file, dbf->order_tag);
    if (!t) {
        return R_ERR_CDX_TAG;
    }
    rx_cdx_tag_ensure_sorted(t);
    if (pos >= t->n) {
        return R_ERR_RANGE;
    }
    *recno_out = t->ents[pos].recno;
    return R_OK;
}

int rx_index_pos_of(const rx_dbf* dbf, uint32_t recno, int64_t* pos_out) {
    if (!dbf || dbf->order_file < 0) {
        *pos_out = dbf ? (int64_t)recno - 1 : -1; /* natural order */
        return dbf ? R_OK : R_ERR_RANGE;
    }
    rx_tag* t = tag_get((rx_dbf*)dbf, dbf->order_file, dbf->order_tag);
    if (!t) {
        return R_ERR_CDX_TAG;
    }
    rx_cdx_tag_ensure_sorted(t);
    for (size_t i = 0; i < t->n; i++) {
        if (t->ents[i].recno == recno) {
            *pos_out = (int64_t)i;
            return R_OK;
        }
    }
    *pos_out = -1;
    return R_OK;
}

int rx_index_flush(rx_dbf* dbf) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    int rc = R_OK;
    for (size_t f = 0; f < dbf->nidxs; f++) {
        rx_idx* ix = dbf->idxs[f];
        int dirty = 0;
        for (size_t ti = 0; ti < ix->ntags; ti++) {
            if (ix->tags[ti].dirty) {
                dirty = 1;
            }
        }
        if (!dirty || !ix->writable || ix->is_ntx) {
            continue;
        }
        /* structural CDX writes only under exclusive open */
        if (ix->structural && !dbf->exclusive) {
            continue;
        }
        int w = ix->is_cdx ? cdx_write_file(ix) : rx_idx_write_file(ix);
        if (w != R_OK) {
            rc = w;
        }
    }
    return rc;
}

int rx_index_close_all(rx_dbf* dbf) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    rx_index_flush(dbf);
    for (size_t f = 0; f < dbf->nidxs; f++) {
        rx_idx* ix = dbf->idxs[f];
        if (ix->fd >= 0) {
            close(ix->fd);
        }
        for (size_t ti = 0; ti < ix->ntags; ti++) {
            rx_cdx_tag_clear_ents(&ix->tags[ti]);
            ix->tags[ti].dirty = 0;
        }
        free(ix->tags);
        free(ix);
    }
    free(dbf->idxs);
    dbf->idxs = NULL;
    dbf->nidxs = dbf->capidxs = 0;
    dbf->order_file = -1;
    dbf->order_tag = -1;
    return R_OK;
}

int rx_index_close_nonsstructural(rx_dbf* dbf) {
    if (!dbf) {
        return R_ERR_RANGE;
    }
    rx_index_flush(dbf);
    size_t w = 0;
    for (size_t f = 0; f < dbf->nidxs; f++) {
        rx_idx* ix = dbf->idxs[f];
        if (ix->structural) {
            dbf->idxs[w++] = ix;
            continue;
        }
        if (dbf->order_file == (int)f) {
            dbf->order_file = -1;
            dbf->order_tag = -1;
        }
        if (ix->fd >= 0) {
            close(ix->fd);
        }
        for (size_t ti = 0; ti < ix->ntags; ti++) {
            rx_cdx_tag_clear_ents(&ix->tags[ti]);
        }
        free(ix->tags);
        free(ix);
    }
    dbf->nidxs = w;
    return R_OK;
}

/* ============================ attach / open ============================ */

static int has_ext(const char* path, const char* ext) {
    size_t l = strlen(path), e = strlen(ext);
    return l >= e && rx_strcasecmp(path + l - e, ext) == 0;
}

int rx_index_attach(rx_dbf* dbf, const char* path, rx_idx** out) {
    if (!dbf || !path) {
        return R_ERR_RANGE;
    }
    /* already attached? */
    for (size_t i = 0; i < dbf->nidxs; i++) {
        if (strcmp(dbf->idxs[i]->path, path) == 0) {
            if (out) {
                *out = dbf->idxs[i];
            }
            return R_OK;
        }
    }
    rx_idx* ix = calloc(1, sizeof(rx_idx));
    if (!ix) {
        RX_FATAL("out of memory");
    }
    snprintf(ix->path, sizeof(ix->path), "%s", path);
    ix->fd = -1;
    if (has_ext(path, ".ntx") || has_ext(path, ".ndx")) {
        ix->is_ntx = 1;
        ix->is_cdx = 0;
        ix->writable = 0;
    } else if (has_ext(path, ".idx")) {
        ix->is_cdx = 0;
        ix->writable = 1;
    } else {
        ix->is_cdx = 1;
        ix->writable = 1;
    }
    int fd = open(path, O_RDONLY);
    int rc = R_OK;
    if (fd >= 0) {
        uint8_t buf[RX_CDX_BLOCK];
        if (rx_cdx_blk_read(fd, 0, buf) == R_OK) {
            uint32_t sig = rx_cdx_get_u32(buf);
            if (sig == RX_CDX_SIG && ix->is_cdx) {
                rc = cdx_read_dir(fd, ix);
                if (rc == R_OK) {
                    rc = cdx_read_tag_trees(fd, ix);
                }
            } else if (sig == RX_IDX_SIG && !ix->is_cdx && !ix->is_ntx) {
                /* standalone IDX: single implicit tag from the header */
                if (ix->ntags == 0) {
                    ix->captags = 1;
                    ix->tags = calloc(1, sizeof(rx_tag));
                    if (!ix->tags) {
                        RX_FATAL("out of memory");
                    }
                    rx_tag* t = &ix->tags[ix->ntags++];
                    memset(t, 0, sizeof(*t));
                    /* implicit tag carries the file stem (ORDER()/TAG() report it) */
                    const char* base = strrchr(path, '/');
                    base = base ? base + 1 : path;
                    char stem[12];
                    size_t sl = 0;
                    while (base[sl] && base[sl] != '.' && sl < 11) {
                        stem[sl] = (char)toupper((unsigned char)base[sl]);
                        sl++;
                    }
                    stem[sl] = '\0';
                    snprintf(t->name, sizeof(t->name), "%s", stem);
                    memcpy(t->key_expr, buf + 8, 128);
                    t->key_expr[128] = '\0';
                    memcpy(t->for_expr, buf + 136, 64);
                    t->for_expr[64] = '\0';
                    t->flags = rx_cdx_get_u16(buf + 200);
                    uint32_t root = rx_cdx_get_u32(buf + 204);
                    uint64_t nk = rx_cdx_get_u64(buf + 208);
                    rc = cdx_read_tree(fd, t, root, (uint32_t)nk);
                }
            } else if (ix->is_ntx) {
                rc = rx_ntx_read_file(dbf, path, ix); /* read-only import */
            } else {
                rc = R_ERR_CDX_VARIANT;
            }
        } else {
            rc = R_ERR_IO_READ;
        }
        close(fd);
    } else {
        /* not present yet: allowed for create paths; structural CDX may not exist */
        rc = R_OK;
    }
    if (rc != R_OK) {
        for (size_t i = 0; i < ix->ntags; i++) {
            rx_cdx_tag_clear_ents(&ix->tags[i]);
        }
        free(ix->tags);
        free(ix);
        return rc;
    }
    ix->structural = 0;
    rx_cdx_idx_add(dbf, ix);
    if (out) {
        *out = ix;
    }
    return R_OK;
}

/* internal: attach the structural CDX (same stem) if it exists */
int rx_index_attach_structural(rx_dbf* dbf) {
    char path[264];
    snprintf(path, sizeof(path), "%s", dbf->path);
    size_t l = strlen(path);
    if (l >= 4 && rx_strcasecmp(path + l - 4, ".dbf") == 0) {
        snprintf(path + l - 4, sizeof(path) - l + 4, ".cdx");
    } else {
        strncat(path, ".cdx", sizeof(path) - strlen(path) - 1);
    }
    if (access(path, F_OK) != 0) {
        return R_OK; /* no structural index: fine */
    }
    rx_idx* ix = NULL;
    int rc = rx_index_attach(dbf, path, &ix);
    if (rc == R_OK && ix) {
        ix->structural = 1;
        ix->writable = dbf->exclusive && !dbf->readonly;
        /* move to slot 0 */
        size_t pos = dbf->nidxs - 1;
        rx_idx* tmp = dbf->idxs[pos];
        memmove(&dbf->idxs[1], &dbf->idxs[0], pos * sizeof(rx_idx*));
        dbf->idxs[0] = tmp;
        /* FoxPro rule: the first tag of the structural CDX is the controlling
           order when the table is opened */
        if (dbf->order_file == -1 && ix->ntags > 0) {
            dbf->order_file = 0;
            dbf->order_tag = 0;
        }
    }
    return rc;
}
