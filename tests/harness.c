/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file harness.c
 * @brief Purpose-built test harness (spec §12).
 *   reynard-harness selftest
 *       term_script backend self-test: replay events, blit a known grid,
 *       dump cells, compare; plus mouse protocol decoder checks (M0 gate).
 *   reynard-harness conformance <dir> <reynard-bin> <fixtures-dir> <work-root>
 *       runs each tests/conformance case (.prg) in -t mode inside a fresh sandbox
 *       workdir with fixtures copied in, then diffs stdout against
 *       <case>.expected and stderr against <case>.expected.err (error
 *       cases require exit code 1).
 */
#define _POSIX_C_SOURCE 200809L

/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "reynard/dbf.h"
#include "term/mouse.h"
#include "term/term_if.h"
#include "util/util.h"
#include "util/scrfmt.h"

static int failures = 0;

static void rm_rf(const char *path);

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                        \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ---------------- selftest ---------------- */

static int pairs_seen = 0;

static void count_pair(int idx, uint8_t fg, uint8_t bg)
{
    (void)idx;
    (void)fg;
    (void)bg;
    pairs_seen++;
}

static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fputs(content, f);
    fclose(f);
    return 0;
}

static char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    strbuf sb;
    sb_init(&sb);
    char buf[4096];
    size_t k;
    while ((k = fread(buf, 1, sizeof(buf), f)) > 0) {
        sb_put(&sb, buf, k);
    }
    fclose(f);
    if (!sb.buf) {
        sb_putc(&sb, '\0');
        sb.len = 0;
    }
    *n = sb.len;
    return sb.buf;
}

static int selftest_term(void)
{
    char dir[] = "/tmp/rx-selftest-XXXXXX";
    if (rx_mkdtemp(dir) != 0) {
        perror("mkdtemp");
        return 1;
    }
    char evpath[300], dumppath[300];
    snprintf(evpath, sizeof(evpath), "%s/t.events", dir);
    snprintf(dumppath, sizeof(dumppath), "%s/t.dump", dir);
    write_file(evpath,
               "# comment line\n"
               "key a\n"
               "key enter\n"
               "key f5\n"
               "key ctrl-a\n"
               "text hi\n"
               "mouse press 3 4 1\n"
               "mouse release 3 4 1\n"
               "mouse wheelup 0 0 0\n"
               "resize 40 10\n"
               "eof\n");
    setenv("RX_EVENTS", evpath, 1);
    setenv("RX_DUMP", dumppath, 1);

    const term_backend *tb = rx_term_script();
    CHECK(tb != NULL, "script backend exists");
    CHECK(tb->init() == R_OK, "script init");

    revent ev;
    CHECK(tb->poll(&ev, 0) == R_OK && ev.kind == EV_KEY && ev.u.k.cp == 'a', "key a");
    CHECK(tb->poll(&ev, 0) == R_OK && ev.kind == EV_KEY && ev.u.k.sym == RXK_ENTER, "enter");
    CHECK(tb->poll(&ev, 0) == R_OK && ev.kind == EV_KEY && ev.u.k.sym == RXK_F5, "f5");
    CHECK(tb->poll(&ev, 0) == R_OK && ev.kind == EV_KEY && ev.u.k.sym == RXK_CTL('A'), "ctrl-a");
    CHECK(tb->poll(&ev, 0) == R_OK && ev.kind == EV_KEY && ev.u.k.cp == 'h', "text h");
    CHECK(tb->poll(&ev, 0) == R_OK && ev.kind == EV_KEY && ev.u.k.cp == 'i', "text i");
    CHECK(tb->poll(&ev, 0) == R_OK && ev.kind == EV_MOUSE && ev.u.m.kind == RM_PRESS &&
              ev.u.m.x == 3 && ev.u.m.y == 4 && ev.u.m.button == 1,
          "mouse press");
    CHECK(tb->poll(&ev, 0) == R_OK && ev.kind == EV_MOUSE && ev.u.m.kind == RM_RELEASE, "release");
    CHECK(tb->poll(&ev, 0) == R_OK && ev.kind == EV_MOUSE && ev.u.m.kind == RM_WHEEL_UP, "wheel");
    CHECK(tb->poll(&ev, 0) == R_OK && ev.kind == EV_RESIZE && ev.u.m.x == 40 && ev.u.m.y == 10,
          "resize");
    CHECK(tb->poll(&ev, 0) == R_OK && ev.kind == EV_EOF, "eof");

    /* blit a known 4x2 grid */
    rcell cells[8];
    memset(cells, 0, sizeof(cells));
    cells[0].ch = 'R';
    cells[1].ch = 'e';
    cells[2].ch = 'y';
    cells[3].ch = '!';
    cells[4].ch = 'f';
    cells[5].ch = 'o';
    cells[6].ch = 'x';
    cells[7].ch = 0; /* renders as space */
    CHECK(tb->blit(cells, 4, 2, NULL, 0) == R_OK, "blit");
    CHECK(rx_term_script_blit_count() == 1, "blit counted");

    CHECK(tb->palette(count_pair) == R_OK, "palette callback");
    CHECK(pairs_seen == 256, "palette has 256 slots (16 fg x 16 bg)");

    tb->shutdown();
    size_t n;
    char *dump = read_file(dumppath, &n);
    CHECK(dump != NULL, "dump written");
    if (dump) {
        CHECK(strcmp(dump, "Rey!\nfox\n") == 0, "dump content matches grid");
        free(dump);
    }
    unsetenv("RX_EVENTS");
    unsetenv("RX_DUMP");
    rm_rf(dir);
    return failures;
}

static int selftest_mouse(void)
{
    /* SGR 1006 press: ESC [ < 0 ; 11 ; 6 M  -> button 1 at (10,5) */
    const uint8_t sgr_press[] = { 0x1B, '[', '<', '0', ';', '1', '1', ';', '6', 'M' };
    rmouse m;
    size_t used = 0;
    CHECK(mouse_decode(sgr_press, sizeof(sgr_press), &m, &used) == 1, "sgr press decodes");
    CHECK(used == sizeof(sgr_press) && m.kind == RM_PRESS && m.button == 1 && m.x == 10 && m.y == 5,
          "sgr press fields");
    /* SGR release: ... m */
    const uint8_t sgr_rel[] = { 0x1B, '[', '<', '0', ';', '1', '1', ';', '6', 'm' };
    CHECK(mouse_decode(sgr_rel, sizeof(sgr_rel), &m, &used) == 1 && m.kind == RM_RELEASE,
          "sgr release");
    /* SGR wheel up: button 64 */
    const uint8_t sgr_wheel[] = { 0x1B, '[', '<', '6', '4', ';', '1', ';', '1', 'M' };
    CHECK(mouse_decode(sgr_wheel, sizeof(sgr_wheel), &m, &used) == 1 && m.kind == RM_WHEEL_UP,
          "sgr wheel up");
    /* legacy 1000: ESC [ M C! 4  -> b=67-32=35? use b=32(' ')+0 -> button1, x=33+32... */
    const uint8_t legacy[] = { 0x1B, '[', 'M', (uint8_t)(' ' + 0), (uint8_t)(' ' + 10),
                               (uint8_t)(' ' + 5) };
    CHECK(mouse_decode(legacy, sizeof(legacy), &m, &used) == 1 && used == 6 && m.kind == RM_PRESS &&
              m.button == 1 && m.x == 9 && m.y == 4,
          "legacy 1000 decodes");
    /* SGR drag: motion (32) + button1 (0) -> b=32 */
    const uint8_t sgr_drag[] = { 0x1B, '[', '<', '3', '2', ';', '1', '1', ';', '6', 'M' };
    CHECK(mouse_decode(sgr_drag, sizeof(sgr_drag), &m, &used) == 1 && m.kind == RM_DRAG &&
              m.button == 1 && m.x == 10 && m.y == 5,
          "sgr drag decodes");
    /* SGR hover: motion with no button -> b=35 */
    const uint8_t sgr_move[] = { 0x1B, '[', '<', '3', '5', ';', '1', ';', '1', 'M' };
    CHECK(mouse_decode(sgr_move, sizeof(sgr_move), &m, &used) == 1 && m.kind == RM_MOVE &&
              m.button == 0,
          "sgr hover decodes");
    /* legacy 1002/1003 drag: b = 32|0 -> byte 64 ('@') */
    const uint8_t legacy_drag[] = { 0x1B, '[', 'M', (uint8_t)('@'), (uint8_t)(' ' + 10),
                                    (uint8_t)(' ' + 5) };
    CHECK(mouse_decode(legacy_drag, sizeof(legacy_drag), &m, &used) == 1 && used == 6 &&
              m.kind == RM_DRAG && m.button == 1 && m.x == 9 && m.y == 4,
          "legacy drag decodes");
    /* legacy X10 release: b = 3 -> byte 35 ('#') */
    const uint8_t legacy_rel[] = { 0x1B, '[', 'M', (uint8_t)('#'), (uint8_t)(' ' + 10),
                                   (uint8_t)(' ' + 5) };
    CHECK(mouse_decode(legacy_rel, sizeof(legacy_rel), &m, &used) == 1 && m.kind == RM_RELEASE,
          "legacy release decodes");
    /* incomplete: needs more bytes */
    const uint8_t part[] = { 0x1B, '[', '<', '0', ';' };
    CHECK(mouse_decode(part, sizeof(part), &m, &used) == 0, "incomplete returns 0");
    /* garbage */
    const uint8_t bad[] = { 'x', 'y', 'z' };
    CHECK(mouse_decode(bad, sizeof(bad), &m, &used) == -1, "garbage rejected");
    return failures;
}

/* ---------------- conformance ---------------- */

static int cmp_files(const char *a, const char *b, char *diffbuf, size_t diffsz)
{
    size_t na, nb;
    char *ca = read_file(a, &na);
    char *cb = read_file(b, &nb);
    if (!ca || !cb) {
        free(ca);
        free(cb);
        snprintf(diffbuf, diffsz, "cannot read files");
        return -1;
    }
    int eq = na == nb && memcmp(ca, cb, na) == 0;
    if (!eq) {
        snprintf(diffbuf, diffsz, "first difference at byte %zu (sizes %zu vs %zu)",
                 na < nb ? na : nb, na, nb);
    }
    free(ca);
    free(cb);
    return eq ? 0 : 1;
}

static void copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "r");
    if (!in) {
        return;
    }
    FILE *out = fopen(dst, "w");
    if (!out) {
        fclose(in);
        return;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
}

static void rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                char sub[1400];
                snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
                rm_rf(sub);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static int filter_prg(const struct dirent *d)
{
    size_t n = strlen(d->d_name);
    return n > 4 && strcmp(d->d_name + n - 4, ".prg") == 0;
}

static int conformance(const char *dir, const char *bin, const char *fixtures, const char *workroot)
{
    /* the child chdir()s into its sandbox, so the binary path must be absolute */
    char absbin[1200];
    if (bin[0] == '/') {
        snprintf(absbin, sizeof(absbin), "%s", bin);
    } else {
        char cwd[600];
        if (!getcwd(cwd, sizeof(cwd))) {
            perror("getcwd");
            return 1;
        }
        snprintf(absbin, sizeof(absbin), "%s/%s", cwd, bin);
    }
    bin = absbin;
    char absroot[1000];
    if (workroot[0] == '/') {
        snprintf(absroot, sizeof(absroot), "%s", workroot);
    } else {
        char cwd[600];
        if (!getcwd(cwd, sizeof(cwd))) {
            perror("getcwd");
            return 1;
        }
        snprintf(absroot, sizeof(absroot), "%s/%s", cwd, workroot);
    }
    mkdir(absroot, 0755); /* may exist */
    struct dirent **names = NULL;
    int n = scandir(dir, &names, filter_prg, NULL);
    if (n < 0) {
        perror("scandir");
        return 1;
    }
    /* sort by name */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(names[i]->d_name, names[j]->d_name) > 0) {
                struct dirent *t = names[i];
                names[i] = names[j];
                names[j] = t;
            }
        }
    }
    int ran = 0;
    for (int i = 0; i < n; i++) {
        char base[1000], prg[1100], expected[1100], expectederr[1100];
        snprintf(base, sizeof(base), "%s/%s", dir, names[i]->d_name);
        base[strlen(base) - 4] = '\0';
        snprintf(prg, sizeof(prg), "%s.prg", base);
        snprintf(expected, sizeof(expected), "%s.expected", base);
        snprintf(expectederr, sizeof(expectederr), "%s.expected.err", base);
        char stdinf[1100];
        snprintf(stdinf, sizeof(stdinf), "%s.stdin", base);

        char wd[1100];
        snprintf(wd, sizeof(wd), "%s/case-XXXXXX", absroot);
        if (rx_mkdtemp(wd) != 0) {
            perror("mkdtemp");
            failures++;
            continue;
        }
        /* copy fixtures */
        struct dirent **fx = NULL;
        int nf = scandir(fixtures, &fx, NULL, alphasort);
        if (nf > 0) {
            for (int k = 0; k < nf; k++) {
                char s[1400], d[1400];
                snprintf(s, sizeof(s), "%s/%s", fixtures, fx[k]->d_name);
                snprintf(d, sizeof(d), "%s/%s", wd, fx[k]->d_name);
                struct stat st;
                if (fx[k]->d_name[0] != '.' && stat(s, &st) == 0 && S_ISREG(st.st_mode)) {
                    copy_file(s, d);
                }
                free(fx[k]);
            }
            free(fx);
        }

        /* copy the case into the sandbox under a stable name so error
           diagnostics ("of case.prg") are machine-independent */
        char caseprg[1200];
        snprintf(caseprg, sizeof(caseprg), "%s/case.prg", wd);
        copy_file(prg, caseprg);
        /* per-case form siblings ship as case.scr / case.frm / case.rlb */
        static const char *exts3[3] = { "scr", "frm", "rlb" };
        for (int ei = 0; ei < 3; ei++) {
            char srcf[1200], dstf[1200];
            snprintf(srcf, sizeof(srcf), "%s.%s", base, exts3[ei]);
            if (access(srcf, F_OK) == 0) {
                snprintf(dstf, sizeof(dstf), "%s/case.%s", wd, exts3[ei]);
                copy_file(srcf, dstf);
            }
        }
        int has_stdin = access(stdinf, F_OK) == 0;
        if (has_stdin) {
            char stdind[1200];
            snprintf(stdind, sizeof(stdind), "%s/stdin.txt", wd);
            copy_file(stdinf, stdind);
        }
        char outp[1200], errp[1200];
        snprintf(outp, sizeof(outp), "%s/out.txt", wd);
        snprintf(errp, sizeof(errp), "%s/err.txt", wd);
        pid_t pid = fork();
        if (pid == 0) {
            if (chdir(wd) != 0) {
                _exit(99);
            }
            setenv("HOME", wd, 1); /* isolate the .RXP fallback cache */
            if (has_stdin) {
                FILE *fi = freopen("stdin.txt", "r", stdin);
                (void)fi;
            }
            FILE *fe = freopen(errp, "w", stderr);
            (void)fe;
            execl(bin, "reynard", "-t", "-o", "out.txt", "case.prg", (char *)NULL);
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        int exitcode = WIFEXITED(status) ? WEXITSTATUS(status) : 128;

        ran++;
        int err_case = access(expectederr, F_OK) == 0;
        char diff[300] = "";
        int bad = 0;
        if (err_case) {
            if (exitcode == 0) {
                snprintf(diff, sizeof(diff), "expected failure but exit code 0");
                bad = 1;
            }
            int r = cmp_files(errp, expectederr, diff, sizeof(diff));
            if (r != 0) {
                bad = 1;
            }
        } else {
            if (exitcode != 0) {
                size_t ne;
                char *e = read_file(errp, &ne);
                snprintf(diff, sizeof(diff), "exit code %d; stderr: %s", exitcode,
                         e ? e : "(none)");
                free(e);
                bad = 1;
            }
            if (!bad) {
                int r = cmp_files(outp, expected, diff, sizeof(diff));
                if (r != 0) {
                    bad = 1;
                }
            }
        }
        if (bad) {
            printf("FAIL %s: %s\n", names[i]->d_name, diff);
            failures++;
        } else {
            printf("PASS %s\n", names[i]->d_name);
        }
        rm_rf(wd);
        free(names[i]);
    }
    free(names);
    printf("conformance: %d cases, %d failures\n", ran, failures);
    return failures ? 1 : 0;
}

/* ---------------- roundtrip (M2 gate: byte-identical read/write) ---------------- */

static int filter_dbf(const struct dirent *d)
{
    size_t n = strlen(d->d_name);
    return n > 4 && strcmp(d->d_name + n - 4, ".dbf") == 0;
}

/* read the raw 32-byte header of a dbf file */
static int read_hdr32(const char *path, uint8_t hdr[32])
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    size_t n = fread(hdr, 1, 32, f);
    fclose(f);
    return n == 32 ? 0 : -1;
}

static int roundtrip(const char *fixtures, const char *workroot)
{
    rx_data_session(1, 0x01); /* frozen header date: byte-identical output */
    char absroot[1000];
    if (workroot[0] == '/') {
        snprintf(absroot, sizeof(absroot), "%s", workroot);
    } else {
        char cwd[600];
        if (!getcwd(cwd, sizeof(cwd))) {
            perror("getcwd");
            return 1;
        }
        snprintf(absroot, sizeof(absroot), "%s/%s", cwd, workroot);
    }
    mkdir(absroot, 0755);
    struct dirent **names = NULL;
    int n = scandir(fixtures, &names, filter_dbf, alphasort);
    if (n < 0) {
        perror("scandir");
        return 1;
    }
    int ran = 0;
    for (int i = 0; i < n; i++) {
        char src[1400], dst[1400];
        snprintf(src, sizeof(src), "%s/%s", fixtures, names[i]->d_name);
        snprintf(dst, sizeof(dst), "%s/rt-%s", absroot, names[i]->d_name);
        unlink(dst);
        ran++;
        uint8_t hdr[32];
        if (read_hdr32(src, hdr) != 0) {
            printf("FAIL %s: cannot read header\n", names[i]->d_name);
            failures++;
            free(names[i]);
            continue;
        }
        uint8_t version = hdr[0], mark = hdr[29];
        int yy = 1900 + hdr[1], mm = hdr[2], dd = hdr[3];
        rx_dbf *in = NULL;
        int rc = rx_dbf_open(src, RD_READONLY, &in);
        if (rc != R_OK) {
            printf("FAIL %s: open %d\n", names[i]->d_name, rc);
            failures++;
            free(names[i]);
            continue;
        }
        size_t nf;
        rx_dbf_field_count(in, &nf);
        rx_fielddef *defs = calloc(nf, sizeof(rx_fielddef));
        for (size_t k = 0; k < nf; k++) {
            rx_dbf_field_def(in, k, &defs[k]);
        }
        rx_dbf *out = NULL;
        rc = rx_dbf_create_full(dst, defs, nf, version, mark, yy, mm, dd, &out);
        free(defs);
        if (rc != R_OK) {
            printf("FAIL %s: create %d\n", names[i]->d_name, rc);
            failures++;
            rx_dbf_close(in);
            free(names[i]);
            continue;
        }
        uint32_t nrec;
        rx_dbf_recount(in, &nrec);
        uint16_t rsz;
        rx_dbf_recsize(in, &rsz);
        uint8_t *raw = malloc(rsz);
        for (uint32_t r = 1; r <= nrec; r++) {
            rc = rx_dbf_read_raw(in, r, raw);
            if (rc != R_OK) {
                break;
            }
            uint32_t nr;
            rc = rx_dbf_append(out, &nr);
            if (rc != R_OK) {
                break;
            }
            rc = rx_dbf_put_raw(out, raw);
            if (rc != R_OK) {
                break;
            }
            rc = rx_flush(out);
            if (rc != R_OK) {
                break;
            }
        }
        free(raw);
        rx_dbf_close(in);
        rx_dbf_close(out);
        if (rc != R_OK) {
            printf("FAIL %s: copy rc=%d\n", names[i]->d_name, rc);
            failures++;
            free(names[i]);
            continue;
        }
        /* byte-identical check */
        size_t na, nb;
        char *ca = read_file(src, &na);
        char *cb = read_file(dst, &nb);
        int eq = ca && cb && na == nb && memcmp(ca, cb, na) == 0;
        free(ca);
        free(cb);
        /* determinism: write the same content again, must be identical */
        char dst2[1400];
        snprintf(dst2, sizeof(dst2), "%s/rt2-%s", absroot, names[i]->d_name);
        unlink(dst2);
        rx_dbf *in2 = NULL;
        rx_dbf_open(src, RD_READONLY, &in2);
        size_t nf2;
        rx_dbf_field_count(in2, &nf2);
        rx_fielddef *defs2 = calloc(nf2, sizeof(rx_fielddef));
        for (size_t k = 0; k < nf2; k++) {
            rx_dbf_field_def(in2, k, &defs2[k]);
        }
        rx_dbf *out2 = NULL;
        rx_dbf_create_full(dst2, defs2, nf2, version, mark, yy, mm, dd, &out2);
        free(defs2);
        uint32_t nrec2;
        rx_dbf_recount(in2, &nrec2);
        uint8_t *raw2 = malloc(rsz);
        for (uint32_t r = 1; r <= nrec2; r++) {
            rx_dbf_read_raw(in2, r, raw2);
            uint32_t nr;
            rx_dbf_append(out2, &nr);
            rx_dbf_put_raw(out2, raw2);
            rx_flush(out2);
        }
        free(raw2);
        rx_dbf_close(in2);
        rx_dbf_close(out2);
        size_t nc;
        char *cc = read_file(dst, &na);
        char *cd = read_file(dst2, &nc);
        int det = cc && cd && na == nc && memcmp(cc, cd, na) == 0;
        free(cc);
        free(cd);
        if (eq && det) {
            printf("PASS %s (byte-identical, deterministic)\n", names[i]->d_name);
        } else {
            printf("FAIL %s: byte-identical=%d deterministic=%d\n", names[i]->d_name, eq, det);
            failures++;
        }
        free(names[i]);
    }
    free(names);

    /* memo round trip through the public API: create FPT dbf, write memos,
       reopen, compare text; write twice and require byte-identical files */
    char wd[1100];
    snprintf(wd, sizeof(wd), "%s/rt-memo-XXXXXX", absroot);
    if (rx_mkdtemp(wd) == 0) {
        ran++;
        char p1[1200], p2[1200];
        snprintf(p1, sizeof(p1), "%s/m1.dbf", wd);
        snprintf(p2, sizeof(p2), "%s/m2.dbf", wd);
        rx_fielddef mf[2] = { { "ID", RFT_N, 4, 0, 0 }, { "NOTES", RFT_M, 10, 0, 0 } };
        const char *texts[3] = { "alpha memo", "a much longer memo payload that spans "
                                         "multiple 64-byte FPT blocks for sure........"
                                         "..............................................",
                                 "" };
        int ok = 1;
        for (int pass = 0; pass < 2 && ok; pass++) {
            const char *pp = pass == 0 ? p1 : p2;
            rx_dbf *d = NULL;
            int rc = rx_dbf_create(pp, mf, 2, &d);
            if (rc != R_OK) {
                ok = 0;
                break;
            }
            for (int i = 0; i < 3; i++) {
                uint32_t nr;
                rx_dbf_append(d, &nr);
                rval v;
                rx_rval_undef(&v);
                char num[8];
                snprintf(num, sizeof(num), "%d", i + 1);
                rval_set_str(&v, rx_strdup(num), strlen(num));
                rx_dbf_put(d, 0, &v);
                rx_rval_free(&v);
                rx_rval_undef(&v);
                rval_set_str(&v, rx_strdup(texts[i]), strlen(texts[i]));
                rx_dbf_put(d, 1, &v);
                rx_rval_free(&v);
                rx_flush(d);
            }
            rx_dbf_close(d);
        }
        if (ok) {
            /* reopen p1 and verify memo text */
            rx_dbf *d = NULL;
            if (rx_dbf_open(p1, RD_READONLY, &d) == R_OK) {
                for (int i = 0; i < 3; i++) {
                    rx_dbf_goto(d, (uint32_t)(i + 1));
                    rval v;
                    rx_rval_undef(&v);
                    if (rx_dbf_get(d, 1, &v) == R_OK) {
                        const char *s = "";
                        size_t sn = 0;
                        if (v.t == RX_T_M && v.u.memo.loaded) {
                            s = v.u.memo.loaded->utf8 ? v.u.memo.loaded->utf8 : "";
                            sn = v.u.memo.loaded->n;
                        }
                        if (sn != strlen(texts[i]) || memcmp(s, texts[i], sn) != 0) {
                            ok = 0;
                        }
                        rx_rval_free(&v);
                    } else {
                        ok = 0;
                    }
                }
                rx_dbf_close(d);
            } else {
                ok = 0;
            }
        }
        if (ok) {
            /* byte-identical determinism of dbf + fpt */
            char f1[1200], f2[1200];
            snprintf(f1, sizeof(f1), "%s/m1.FPT", wd);
            snprintf(f2, sizeof(f2), "%s/m2.FPT", wd);
            size_t n1, n2, n3, n4;
            char *b1 = read_file(p1, &n1);
            char *b2 = read_file(p2, &n2);
            char *g1 = read_file(f1, &n3);
            char *g2 = read_file(f2, &n4);
            /* fpt may be created with uppercase ext; find it */
            if (!g1) {
                snprintf(f1, sizeof(f1), "%s/m1.fpt", wd);
                g1 = read_file(f1, &n3);
            }
            if (!g2) {
                snprintf(f2, sizeof(f2), "%s/m2.fpt", wd);
                g2 = read_file(f2, &n4);
            }
            if (!b1 || !b2 || n1 != n2 || memcmp(b1, b2, n1) != 0 || !g1 || !g2 || n3 != n4 ||
                memcmp(g1, g2, n3) != 0) {
                ok = 0;
            }
            free(b1);
            free(b2);
            free(g1);
            free(g2);
        }
        if (ok) {
            printf("PASS memo-fpt (text round-trip, byte-identical)\n");
        } else {
            printf("FAIL memo-fpt\n");
            failures++;
        }
        rm_rf(wd);
    }

    /* delete/pack round trip: copy people.dbf, delete rec 2, pack, verify */
    {
        char src[1400], dst[1400];
        snprintf(src, sizeof(src), "%s/people.dbf", fixtures);
        snprintf(dst, sizeof(dst), "%s/rt-pack.dbf", absroot);
        struct stat st;
        if (stat(src, &st) == 0) {
            ran++;
            copy_file(src, dst);
            rx_dbf *d = NULL;
            int ok = 1;
            if (rx_dbf_open(dst, RD_EXCLUSIVE, &d) == R_OK) {
                if (rx_dbf_goto(d, 2) != R_OK || rx_dbf_set_deleted(d, 1) != R_OK ||
                    rx_dbf_pack(d) != R_OK) {
                    ok = 0;
                }
                uint32_t n;
                rx_dbf_recount(d, &n);
                if (n != 3) { /* people.dbf has Dave deleted already: 5 - Bob - Dave */
                    ok = 0;
                }
                rx_dbf_close(d);
            } else {
                ok = 0;
            }
            if (ok && rx_dbf_open(dst, RD_READONLY, &d) == R_OK) {
                rval v;
                rx_rval_undef(&v);
                rx_dbf_goto(d, 2);
                if (rx_dbf_get(d, 0, &v) == R_OK) {
                    const char *s = v.t == RX_T_C ? v.u.c.utf8 : "";
                    if (strncmp(s, "Bob", 3) == 0) {
                        ok = 0; /* Bob must be gone */
                    }
                    rx_rval_free(&v);
                }
                rx_dbf_goto(d, 2);
                rx_rval_undef(&v);
                if (rx_dbf_get(d, 0, &v) == R_OK) {
                    const char *s = v.t == RX_T_C ? v.u.c.utf8 : "";
                    if (strncmp(s, "Carol", 5) != 0) {
                        ok = 0;
                    }
                    rx_rval_free(&v);
                }
                rx_dbf_close(d);
            }
            if (ok) {
                printf("PASS pack (delete+pack+reopen)\n");
            } else {
                printf("FAIL pack\n");
                failures++;
            }
            unlink(dst);
        }
    }

    printf("roundtrip: %d cases, %d failures\n", ran, failures);
    return failures ? 1 : 0;
}

/* ---------------- lockrace (M2 gate: forked-process lock race) ---------------- */

#define RACE_ITERS 200

static int lockrace(const char *workroot)
{
    rx_data_session(1, 0x01);
    rx_data_set_reprocess(-1); /* AUTOMATIC: exercise the retry policy (spec §7.4) */
    char absroot[1000];
    if (workroot[0] == '/') {
        snprintf(absroot, sizeof(absroot), "%s", workroot);
    } else {
        char cwd[600];
        if (!getcwd(cwd, sizeof(cwd))) {
            perror("getcwd");
            return 1;
        }
        snprintf(absroot, sizeof(absroot), "%s/%s", cwd, workroot);
    }
    mkdir(absroot, 0755);
    char path[1200];
    snprintf(path, sizeof(path), "%s/race.dbf", absroot);
    unlink(path);
    rx_fielddef f[1] = { { "N1", RFT_N, 10, 0, 0 } };
    rx_dbf *d = NULL;
    int rc = rx_dbf_create(path, f, 1, &d);
    CHECK(rc == R_OK, "race.dbf created");
    uint32_t nr;
    CHECK(rx_dbf_append(d, &nr) == R_OK, "race append");
    rval v;
    rx_rval_undef(&v);
    v.t = RX_T_N;
    v.u.n.d = 0;
    v.u.n.dec = 0;
    CHECK(rx_dbf_put(d, 0, &v) == R_OK, "race put 0");
    CHECK(rx_flush(d) == R_OK, "race flush");
    rx_dbf_close(d);

    /* phase 1: mutual exclusion — parent holds rec 1, child must conflict */
    rx_dbf *pd = NULL;
    CHECK(rx_dbf_open(path, RD_SHARED, &pd) == R_OK, "parent open shared");
    CHECK(rx_rlock(pd, 1) == R_OK, "parent rlock 1");
    CHECK(rx_rlock_query(pd, 1) == 0, "own lock not reported as foreign");
    pid_t pid = fork();
    if (pid == 0) {
        rx_dbf *cd = NULL;
        if (rx_dbf_open(path, RD_SHARED, &cd) != R_OK) {
            _exit(10);
        }
        if (rx_rlock_query(cd, 1) != 1) {
            _exit(11); /* foreign lock not visible */
        }
        if (rx_rlock(cd, 1) != R_ERR_LOCK_CONFLICT) {
            _exit(12); /* must conflict */
        }
        if (rx_rlock(cd, 2) != R_OK) {
            _exit(13); /* other record free */
        }
        rx_unlock(cd, 2);
        rx_dbf_close(cd);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child conflict detection");
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        printf("  child exit status=%d\n", status);
    }
    rx_unlock(pd, 0);
    rx_dbf_close(pd);

    /* phase 2: counter race — two children increment rec 1 under RLOCK */
    for (int c = 0; c < 2; c++) {
        pid = fork();
        if (pid == 0) {
            rx_dbf *cd = NULL;
            if (rx_dbf_open(path, RD_SHARED, &cd) != R_OK) {
                _exit(20);
            }
            for (int i = 0; i < RACE_ITERS; i++) {
                if (rx_rlock(cd, 1) != R_OK) {
                    _exit(21);
                }
                if (rx_dbf_goto(cd, 1) != R_OK) {
                    _exit(22);
                }
                rval v;
                rx_rval_undef(&v);
                if (rx_dbf_get(cd, 0, &v) != R_OK) {
                    _exit(23);
                }
                double cur = 0;
                if (v.t == RX_T_N) {
                    cur = v.u.n.d;
                }
                rx_rval_free(&v);
                v.t = RX_T_N;
                v.u.n.d = cur + 1;
                v.u.n.dec = 0;
                if (rx_dbf_put(cd, 0, &v) != R_OK) {
                    _exit(24);
                }
                if (rx_flush(cd) != R_OK) {
                    _exit(25);
                }
                rx_unlock(cd, 1);
            }
            rx_dbf_close(cd);
            _exit(0);
        }
    }
    int bad = 0;
    for (int c = 0; c < 2; c++) {
        waitpid(-1, &status, 0);
        if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
            bad = 1;
            printf("  race child status=%d\n", status);
        }
    }
    CHECK(!bad, "race children clean");
    rx_dbf *vd = NULL;
    CHECK(rx_dbf_open(path, RD_READONLY, &vd) == R_OK, "verify open");
    rx_dbf_goto(vd, 1);
    rval vv;
    rx_rval_undef(&vv);
    CHECK(rx_dbf_get(vd, 0, &vv) == R_OK, "verify get");
    double final = vv.u.n.d;
    rx_rval_free(&vv);
    rx_dbf_close(vd);
    CHECK(final == 2.0 * RACE_ITERS, "no lost updates under RLOCK");
    if (final != 2.0 * RACE_ITERS) {
        printf("  counter final=%f expected=%d\n", final, 2 * RACE_ITERS);
    }
    unlink(path);
    if (failures == 0) {
        printf("lockrace: PASS\n");
        return 0;
    }
    printf("lockrace: %d failures\n", failures);
    return 1;
}

/* ---------------- golden screens (M3 gate: §12 event replay + cell dumps) ---------------- */

static int filter_events(const struct dirent *d)
{
    size_t n = strlen(d->d_name);
    return n > 7 && strcmp(d->d_name + n - 7, ".events") == 0;
}

static int cmp_lines(const char *a, const char *b, char *diffbuf, size_t diffsz)
{
    size_t na, nb;
    char *ca = read_file(a, &na);
    char *cb = read_file(b, &nb);
    if (!ca || !cb) {
        free(ca);
        free(cb);
        snprintf(diffbuf, diffsz, "cannot read dump/golden");
        return -1;
    }
    size_t la = 1;
    char *pa = ca, *pb = cb;
    char *enda = ca + na, *endb = cb + nb;
    while (pa < enda && pb < endb) {
        char *nla = memchr(pa, '\n', (size_t)(enda - pa));
        char *nlb = memchr(pb, '\n', (size_t)(endb - pb));
        size_t sa = nla ? (size_t)(nla - pa) : (size_t)(enda - pa);
        size_t sb = nlb ? (size_t)(nlb - pb) : (size_t)(endb - pb);
        if (sa != sb || memcmp(pa, pb, sa) != 0) {
            snprintf(diffbuf, diffsz, "line %zu differs: dump=\"%.*s\" golden=\"%.*s\"", la,
                     (int)(sa > 120 ? 120 : sa), pa, (int)(sb > 120 ? 120 : sb), pb);
            free(ca);
            free(cb);
            return 1;
        }
        la++;
        pa = nla ? nla + 1 : enda;
        pb = nlb ? nlb + 1 : endb;
    }
    if (pa < enda || pb < endb) {
        snprintf(diffbuf, diffsz, "line count differs at line %zu (dump has %s lines)", la,
                 pa < enda ? "more" : "fewer");
        free(ca);
        free(cb);
        return 1;
    }
    free(ca);
    free(cb);
    return 0;
}

static void abs_path(const char *p, char *out, size_t outsz)
{
    if (p[0] == '/') {
        snprintf(out, outsz, "%s", p);
        return;
    }
    char cwd[600];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("getcwd");
        snprintf(out, outsz, "%s", p);
        return;
    }
    snprintf(out, outsz, "%s/%s", cwd, p);
}

static int golden(const char *dir, const char *bin, const char *fixtures, const char *workroot,
                  int update)
{
    char absbin[1200], absroot[1000];
    abs_path(bin, absbin, sizeof(absbin));
    abs_path(workroot, absroot, sizeof(absroot));
    mkdir(absroot, 0755);
    struct dirent **names = NULL;
    int n = scandir(dir, &names, filter_events, NULL);
    if (n < 0) {
        perror("scandir");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(names[i]->d_name, names[j]->d_name) > 0) {
                struct dirent *t = names[i];
                names[i] = names[j];
                names[j] = t;
            }
        }
    }
    int ran = 0;
    for (int i = 0; i < n; i++) {
        char base[1100];
        snprintf(base, sizeof(base), "%s/%s", dir, names[i]->d_name);
        base[strlen(base) - 7] = '\0';
        char evp[1200], goldp[1200], sizep[1200], exitp[1200], absev[1400];
        snprintf(evp, sizeof(evp), "%s.events", base);
        snprintf(goldp, sizeof(goldp), "%s.golden", base);
        snprintf(sizep, sizeof(sizep), "%s.size", base);
        snprintf(exitp, sizeof(exitp), "%s.exit", base);
        abs_path(evp, absev, sizeof(absev)); /* the child chdirs into its sandbox */

        char wd[1100];
        snprintf(wd, sizeof(wd), "%s/golden-XXXXXX", absroot);
        if (rx_mkdtemp(wd) != 0) {
            perror("mkdtemp");
            failures++;
            continue;
        }
        struct dirent **fx = NULL;
        int nf = scandir(fixtures, &fx, NULL, alphasort);
        if (nf > 0) {
            for (int k = 0; k < nf; k++) {
                char s[1400], d[1400];
                snprintf(s, sizeof(s), "%s/%s", fixtures, fx[k]->d_name);
                snprintf(d, sizeof(d), "%s/%s", wd, fx[k]->d_name);
                struct stat st;
                if (fx[k]->d_name[0] != '.' && stat(s, &st) == 0 && S_ISREG(st.st_mode)) {
                    copy_file(s, d);
                }
                free(fx[k]);
            }
            free(fx);
        }
        char home[1200], dump[1200], outp[1200], errp[1200];
        snprintf(home, sizeof(home), "%s/home", wd);
        mkdir(home, 0755);
        snprintf(dump, sizeof(dump), "%s/dump.txt", wd);
        snprintf(outp, sizeof(outp), "%s/out.txt", wd);
        snprintf(errp, sizeof(errp), "%s/err.txt", wd);

        int cols = 0, rows = 0;
        size_t sn;
        char *sz = read_file(sizep, &sn);
        if (sz) {
            sscanf(sz, "%d %d", &cols, &rows);
            free(sz);
        }
        int want_exit = 0;
        char *ex = read_file(exitp, &sn);
        if (ex) {
            want_exit = (int)strtol(ex, NULL, 10);
            free(ex);
        }

        pid_t pid = fork();
        if (pid == 0) {
            if (chdir(wd) != 0) {
                _exit(99);
            }
            FILE *fo = freopen(outp, "w", stdout);
            FILE *fe = freopen(errp, "w", stderr);
            (void)fo;
            (void)fe;
            setenv("RX_EVENTS", absev, 1);
            setenv("RX_DUMP", dump, 1);
            setenv("HOME", home, 1);
            char xdg[1400];
            snprintf(xdg, sizeof(xdg), "%s/.cache", home);
            setenv("XDG_CACHE_HOME", xdg, 1);
            if (cols > 0 && rows > 0) {
                char cb[16], rb[16];
                snprintf(cb, sizeof(cb), "%d", cols);
                snprintf(rb, sizeof(rb), "%d", rows);
                setenv("RX_COLS", cb, 1);
                setenv("RX_ROWS", rb, 1);
            }
            execl(absbin, "reynard", "-t", (char *)NULL);
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        int exitcode = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        ran++;
        char diff[400] = "";
        int bad = 0;
        if (exitcode != want_exit) {
            size_t ne;
            char *e = read_file(errp, &ne);
            snprintf(diff, sizeof(diff), "exit code %d (want %d); stderr: %s", exitcode, want_exit,
                     e ? e : "(none)");
            free(e);
            bad = 1;
        } else if (access(goldp, F_OK) != 0) {
            snprintf(diff, sizeof(diff), "missing .golden file");
            bad = 1;
        } else if (!bad) {
            int r = cmp_lines(dump, goldp, diff, sizeof(diff));
            if (r != 0) {
                bad = 1;
            }
        }
        if (bad && update && access(dump, F_OK) == 0) {
            copy_file(dump, goldp);
            printf("UPDATE %s\n", names[i]->d_name);
            bad = 0;
        } else if (bad) {
            printf("FAIL %s: %s\n", names[i]->d_name, diff);
            failures++;
        } else {
            printf("PASS %s\n", names[i]->d_name);
        }
        rm_rf(wd);
        free(names[i]);
    }
    free(names);
    printf("golden: %d scenarios, %d failures\n", ran, failures);
    return failures ? 1 : 0;
}

/* M4 gate (spec §11.3): browse relayout must stay under 16ms/keystroke on a
   100k-record DBF. Generates large.dbf deterministically, replays a scripted
   BROWSE + 300 PgDn session through the script backend with RX_PERF timing,
   and asserts the average per-event handle+render time. */
static int browseperf(const char *workroot, const char *bin)
{
    char absbin[1200], absroot[1000];
    abs_path(bin, absbin, sizeof(absbin));
    abs_path(workroot, absroot, sizeof(absroot));
    mkdir(absroot, 0755);
    char wd[1100];
    snprintf(wd, sizeof(wd), "%s/perf-XXXXXX", absroot);
    if (rx_mkdtemp(wd) != 0) {
        perror("mkdtemp");
        return 1;
    }
    /* Write a valid 100k-record DBF (v0x03, no memo, CP mark 0) with a few
       block writes instead of 100k append+flush round-trips. */
    char dbfpath[1200];
    snprintf(dbfpath, sizeof(dbfpath), "%s/large.dbf", wd);
    {
        static const char *fnames[] = { "ID", "NAME", "SCORE", "BORN", "MARRIED" };
        static const char ftypes[] = { 'N', 'C', 'N', 'D', 'L' };
        static const uint8_t flens[] = { 8, 20, 8, 8, 1 };
        const size_t nf = 5;
        const uint16_t hdr_size = (uint16_t)(32 + 32 * nf + 1);
        const uint16_t rec_size = (uint16_t)(1 + 8 + 20 + 8 + 8 + 1);
        const uint32_t nrec = 100000;
        uint8_t *buf = malloc((size_t)hdr_size + (size_t)nrec * rec_size + 1);
        if (!buf) {
            fprintf(stderr, "browseperf: out of memory\n");
            return 1;
        }
        memset(buf, 0, hdr_size);
        buf[0] = 0x03;
        buf[1] = 100; /* 2000-01-01 like test mode */
        buf[2] = 1;
        buf[3] = 1;
        buf[4] = (uint8_t)(nrec & 0xFF);
        buf[5] = (uint8_t)((nrec >> 8) & 0xFF);
        buf[6] = (uint8_t)((nrec >> 16) & 0xFF);
        buf[7] = (uint8_t)((nrec >> 24) & 0xFF);
        buf[8] = (uint8_t)(hdr_size & 0xFF);
        buf[9] = (uint8_t)(hdr_size >> 8);
        buf[10] = (uint8_t)(rec_size & 0xFF);
        buf[11] = (uint8_t)(rec_size >> 8);
        uint8_t *fd = buf + 32;
        for (size_t i = 0; i < nf; i++) {
            memset(fd, 0, 32);
            memcpy(fd, fnames[i], strlen(fnames[i]));
            fd[11] = (uint8_t)ftypes[i];
            fd[16] = flens[i];
            fd[17] = (uint8_t)(i == 2 ? 2 : 0); /* SCORE dec */
            fd += 32;
        }
        buf[32 + 32 * nf] = 0x0D;
        uint8_t *rec = buf + hdr_size;
        for (uint32_t i = 1; i <= nrec; i++) {
            memset(rec, ' ', rec_size);
            rec[0] = ' ';
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%8u", i);
            memcpy(rec + 1, tmp, 8);
            char nm[32];
            snprintf(nm, sizeof(nm), "name-%u", i);
            memcpy(rec + 9, nm, strlen(nm) < 20 ? strlen(nm) : 20);
            snprintf(tmp, sizeof(tmp), "%8.2f", (double)(i % 1000) / 10.0);
            memcpy(rec + 29, tmp, 8);
            snprintf(tmp, sizeof(tmp), "%04d%02d%02d", 1990 + (int)(i % 30), 1 + (int)(i % 12),
                     1 + (int)(i % 28));
            memcpy(rec + 37, tmp, 8);
            rec[45] = (i % 2) ? 'T' : 'F';
            rec += rec_size;
        }
        *rec = 0x1A;
        FILE *df = fopen(dbfpath, "wb");
        if (!df) {
            perror("fopen large.dbf");
            free(buf);
            return 1;
        }
        size_t total = (size_t)hdr_size + (size_t)nrec * rec_size + 1;
        if (fwrite(buf, 1, total, df) != total) {
            fprintf(stderr, "browseperf: short write\n");
            fclose(df);
            free(buf);
            return 1;
        }
        fclose(df);
        free(buf);
    }

    char evp[1200], dump[1200], perf[1200], home[1200], outp[1200], errp[1200];
    snprintf(evp, sizeof(evp), "%s/ev.events", wd);
    snprintf(dump, sizeof(dump), "%s/dump.txt", wd);
    snprintf(perf, sizeof(perf), "%s/perf.txt", wd);
    snprintf(home, sizeof(home), "%s/home", wd);
    snprintf(outp, sizeof(outp), "%s/out.txt", wd);
    snprintf(errp, sizeof(errp), "%s/err.txt", wd);
    FILE *f = fopen(evp, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fprintf(f, "text USE large\nkey enter\ntext BROWSE\nkey enter\n");
    for (int i = 0; i < 300; i++) {
        fprintf(f, "key pgdn\n");
    }
    fprintf(f, "key down\nkey up\nkey left\nkey right\nkey tab\nkey pgup\neof\n");
    fclose(f);
    mkdir(home, 0755);

    char absev[1400];
    abs_path(evp, absev, sizeof(absev));
    pid_t pid = fork();
    if (pid == 0) {
        if (chdir(wd) != 0) {
            _exit(99);
        }
        FILE *fo = freopen(outp, "w", stdout);
        FILE *fe = freopen(errp, "w", stderr);
        (void)fo;
        (void)fe;
        setenv("RX_EVENTS", absev, 1);
        setenv("RX_DUMP", dump, 1);
        setenv("RX_PERF", perf, 1);
        setenv("HOME", home, 1);
        char xdg[1400];
        snprintf(xdg, sizeof(xdg), "%s/.cache", home);
        setenv("XDG_CACHE_HOME", xdg, 1);
        execl(absbin, "reynard", "-t", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int exitcode = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    size_t pn = 0;
    char *ptxt = read_file(perf, &pn);
    unsigned long n = 0;
    double avg = 0, max = 0;
    if (ptxt) {
        sscanf(ptxt, "n=%lu avg=%lf max=%lf", &n, &avg, &max);
        free(ptxt);
    }
    int ok = exitcode == 0 && n >= 300 && avg <= 16.0;
    printf("browseperf: %s (exit=%d, %lu events on 100k-record dbf, avg=%.3fms max=%.3fms, gate 16ms)\n",
           ok ? "PASS" : "FAIL", exitcode, n, avg, max);
    if (!ok && exitcode != 0) {
        size_t en;
        char *e = read_file(errp, &en);
        if (e) {
            fprintf(stderr, "stderr: %s\n", e);
            free(e);
        }
    }
    rm_rf(wd);
    return ok ? 0 : 1;
}

/* ---------------- rxp (M5 gate: byte-reproducible .RXP cache) ----------------
 * For each <case>.prg: run it in two fresh sandboxes, require identical
 * stdout/stderr/exit code AND byte-identical case.rxp; then re-run in the
 * first sandbox (cache hit) and require identical output again. */
static int run_in_sandbox(const char *bin, const char *wd, const char *fixtures,
                          const char *prg, const char *caseprg, char *outp, char *errp,
                          size_t sz)
{
    struct dirent **fx = NULL;
    int nf = scandir(fixtures, &fx, NULL, alphasort);
    if (nf > 0) {
        for (int k = 0; k < nf; k++) {
            char s[1400], d[1400];
            snprintf(s, sizeof(s), "%s/%s", fixtures, fx[k]->d_name);
            snprintf(d, sizeof(d), "%s/%s", wd, fx[k]->d_name);
            struct stat st;
            if (fx[k]->d_name[0] != '.' && stat(s, &st) == 0 && S_ISREG(st.st_mode)) {
                copy_file(s, d);
            }
            free(fx[k]);
        }
        free(fx);
    }
    copy_file(prg, caseprg);
    snprintf(outp, sz, "%s/out.txt", wd);
    snprintf(errp, sz, "%s/err.txt", wd);
    pid_t pid = fork();
    if (pid == 0) {
        if (chdir(wd) != 0) {
            _exit(99);
        }
        setenv("HOME", wd, 1);
        FILE *fe = freopen(errp, "w", stderr);
        (void)fe;
        execl(bin, "reynard", "-t", "-o", "out.txt", "case.prg", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static int rxp_mode(const char *dir, const char *bin, const char *fixtures, const char *workroot)
{
    char absbin[1200];
    if (bin[0] == '/') {
        snprintf(absbin, sizeof(absbin), "%s", bin);
    } else {
        char cwd[600];
        if (!getcwd(cwd, sizeof(cwd))) {
            perror("getcwd");
            return 1;
        }
        snprintf(absbin, sizeof(absbin), "%s/%s", cwd, bin);
    }
    char absroot[1000];
    if (workroot[0] == '/') {
        snprintf(absroot, sizeof(absroot), "%s", workroot);
    } else {
        char cwd[600];
        if (!getcwd(cwd, sizeof(cwd))) {
            perror("getcwd");
            return 1;
        }
        snprintf(absroot, sizeof(absroot), "%s/%s", cwd, workroot);
    }
    mkdir(absroot, 0755);
    struct dirent **names = NULL;
    int n = scandir(dir, &names, filter_prg, NULL);
    if (n < 0) {
        perror("scandir");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(names[i]->d_name, names[j]->d_name) > 0) {
                struct dirent *t = names[i];
                names[i] = names[j];
                names[j] = t;
            }
        }
    }
    int ran = 0, bad = 0;
    for (int i = 0; i < n; i++) {
        char base[1000], prg[1100];
        snprintf(base, sizeof(base), "%s/%s", dir, names[i]->d_name);
        base[strlen(base) - 4] = '\0';
        snprintf(prg, sizeof(prg), "%s.prg", base);
        char wdA[1100], wdB[1100];
        snprintf(wdA, sizeof(wdA), "%s/rxpA-XXXXXX", absroot);
        snprintf(wdB, sizeof(wdB), "%s/rxpB-XXXXXX", absroot);
        if (rx_mkdtemp(wdA) != 0 || rx_mkdtemp(wdB) != 0) {
            perror("mkdtemp");
            bad++;
            free(names[i]);
            continue;
        }
        char ca[1200], cb[1200], oa[1200], ob[1200], ea[1200], eb[1200];
        snprintf(ca, sizeof(ca), "%s/case.prg", wdA);
        snprintf(cb, sizeof(cb), "%s/case.prg", wdB);
        int rcA = run_in_sandbox(absbin, wdA, fixtures, prg, ca, oa, ea, sizeof(oa));
        int rcB = run_in_sandbox(absbin, wdB, fixtures, prg, cb, ob, eb, sizeof(ob));
        /* cache-hit re-run in sandbox A */
        int rcA2 = run_in_sandbox(absbin, wdA, fixtures, prg, ca, oa, ea, sizeof(oa));
        char rxpA[1200], rxpB[1200];
        snprintf(rxpA, sizeof(rxpA), "%s/case.rxp", wdA);
        snprintf(rxpB, sizeof(rxpB), "%s/case.rxp", wdB);
        char diff[600] = "";
        int fail = 0;
        if (rcA != rcB || rcA != rcA2) {
            snprintf(diff, sizeof(diff), "exit codes %d/%d/%d", rcA, rcB, rcA2);
            fail = 1;
        } else {
            char d2[300] = "";
            if (cmp_files(oa, ob, d2, sizeof(d2)) != 0) {
                snprintf(diff, sizeof(diff), "stdout differs run1 vs run2: %.*s", 250, d2);
                fail = 1;
            }
        }
        if (!fail && access(rxpA, F_OK) != 0) {
            snprintf(diff, sizeof(diff), "no case.rxp written");
            fail = 1;
        }
        if (!fail) {
            char d2[300] = "";
            if (cmp_files(rxpA, rxpB, d2, sizeof(d2)) != 0) {
                snprintf(diff, sizeof(diff), ".rxp not byte-reproducible: %.*s", 250, d2);
                fail = 1;
            }
        }
        ran++;
        if (fail) {
            printf("FAIL %s: %s\n", names[i]->d_name, diff);
            bad++;
        } else {
            printf("PASS %s\n", names[i]->d_name);
        }
        rm_rf(wdA);
        rm_rf(wdB);
        free(names[i]);
    }
    free(names);
    printf("rxp: %d cases, %d failures\n", ran, bad);
    return bad ? 1 : 0;
}


/* ---------------- cdx (M6 gates: byte-level interop validator, deterministic
   round-trip, CDX build perf < 2s for 100k keys, spec §11.3) ----------------
   The validator below is an independent linear parser of the 500-byte block
   format (docs/formats.md §4): it shares no code with src/data/cdx.c. */

#define HBLOCK 500
#define HSIG_CDX 0x58444352u /* "RCDX" */
#define HSIG_IDX 0x58444449u /* "RIDX" */
#define HTAG_HDR 236
#define HDIR_HDR 24

static uint16_t h_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t h_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t h_u64(const uint8_t *p)
{
    uint64_t u = 0;
    for (int i = 7; i >= 0; i--) {
        u = (u << 8) | p[i];
    }
    return u;
}

static int h_blk(int fd, uint32_t blk, uint8_t out[HBLOCK])
{
    off_t off = (off_t)blk * HBLOCK;
    size_t got = 0;
    while (got < HBLOCK) {
        ssize_t r = pread(fd, out + got, HBLOCK - got, off + (off_t)got);
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

/* walk one B+tree: leftmost descent + sibling chain; verify node types,
   page bounds, entry bounds, recno range, key ordering (asc/desc) and the
   total key count against the tag header */
static int h_tree_walk(int fd, uint32_t nblocks, uint32_t root, int desc, uint64_t expect_keys,
                       uint32_t nrec, char *err, size_t errsz)
{
    uint8_t buf[HBLOCK];
    if (root == 0 || root >= nblocks) {
        snprintf(err, errsz, "root page %u out of range", root);
        return -1;
    }
    uint32_t cur = root;
    for (;;) {
        if (h_blk(fd, cur, buf) != 0) {
            snprintf(err, errsz, "read node %u", cur);
            return -1;
        }
        uint16_t type = h_u16(buf);
        uint16_t nk = h_u16(buf + 2);
        if (type == 3) {
            break; /* leaf */
        }
        if (type != 1 && type != 2) {
            snprintf(err, errsz, "bad node type %u at %u", type, cur);
            return -1;
        }
        if (nk == 0) {
            snprintf(err, errsz, "empty internal node %u", cur);
            return -1;
        }
        cur = h_u32(buf + 12); /* leftmost child (header field) */
        if (cur == 0 || cur >= nblocks) {
            snprintf(err, errsz, "child page %u out of range", cur);
            return -1;
        }
    }
    uint64_t count = 0;
    uint32_t page = cur;
    int guard = 0;
    uint8_t prev[HBLOCK];
    size_t prevlen = 0;
    uint32_t prevrec = 0;
    int have_prev = 0;
    while (page) {
        if (page >= nblocks || ++guard > (int)nblocks + 1) {
            snprintf(err, errsz, "leaf sibling loop at %u", page);
            return -1;
        }
        if (h_blk(fd, page, buf) != 0) {
            snprintf(err, errsz, "read leaf %u", page);
            return -1;
        }
        uint16_t type = h_u16(buf);
        uint16_t nk = h_u16(buf + 2);
        if (type != 3) {
            snprintf(err, errsz, "non-leaf %u in leaf chain", page);
            return -1;
        }
        uint32_t right = h_u32(buf + 8);
        uint32_t left = h_u32(buf + 4);
        if (left != 0 && left >= nblocks) {
            snprintf(err, errsz, "left sibling %u out of range", left);
            return -1;
        }
        size_t off = 16;
        for (uint16_t i = 0; i < nk; i++) {
            if (off + 2 > HBLOCK) {
                snprintf(err, errsz, "entry overrun at leaf %u", page);
                return -1;
            }
            uint16_t klen = h_u16(buf + off);
            off += 2;
            if (off + (size_t)klen + 4 > HBLOCK) {
                snprintf(err, errsz, "entry key overrun at leaf %u", page);
                return -1;
            }
            const uint8_t *key = buf + off;
            off += klen;
            uint32_t recno = h_u32(buf + off);
            off += 4;
            if (recno < 1 || recno > nrec) {
                snprintf(err, errsz, "recno %u out of range 1..%u", recno, nrec);
                return -1;
            }
            if (have_prev) {
                size_t m = prevlen < klen ? prevlen : klen;
                int c = memcmp(prev, key, m);
                if (c == 0) {
                    c = prevlen < klen ? -1 : prevlen > klen ? 1 : 0;
                }
                if (c == 0) {
                    c = prevrec < recno ? -1 : prevrec > recno ? 1 : 0;
                }
                if (desc) {
                    c = -c;
                }
                if (c > 0) {
                    snprintf(err, errsz, "keys out of order at leaf %u entry %u", page, i);
                    return -1;
                }
            }
            if (klen > HBLOCK) {
                snprintf(err, errsz, "key too long");
                return -1;
            }
            memcpy(prev, key, klen);
            prevlen = klen;
            prevrec = recno;
            have_prev = 1;
            count++;
        }
        page = right;
    }
    if (count != expect_keys) {
        snprintf(err, errsz, "key count %llu != header %llu", (unsigned long long)count,
                 (unsigned long long)expect_keys);
        return -1;
    }
    return 0;
}

/* validate one index file (CDX or IDX) end to end; returns tags/keys */
static int cdxcheck_file(const char *path, uint32_t nrec, int *ntags_out, uint64_t *keys_out,
                         char *err, size_t errsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errsz, "open %s", path);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        snprintf(err, errsz, "fstat %s", path);
        close(fd);
        return -1;
    }
    if (st.st_size % HBLOCK != 0) {
        snprintf(err, errsz, "%s: size %lld not a multiple of %d", path, (long long)st.st_size,
                 HBLOCK);
        close(fd);
        return -1;
    }
    uint32_t nblocks = (uint32_t)(st.st_size / HBLOCK);
    uint8_t buf[HBLOCK];
    if (nblocks < 1 || h_blk(fd, 0, buf) != 0) {
        snprintf(err, errsz, "%s: cannot read block 0", path);
        close(fd);
        return -1;
    }
    uint32_t sig = h_u32(buf);
    int ntags = 0;
    uint64_t keys = 0;
    if (sig == HSIG_IDX) {
        if (h_u16(buf + 4) != 1) {
            snprintf(err, errsz, "%s: bad IDX version", path);
            close(fd);
            return -1;
        }
        int desc = (h_u16(buf + 200) & 1) != 0;
        uint32_t root = h_u32(buf + 204);
        uint64_t nk = h_u64(buf + 208);
        if (h_tree_walk(fd, nblocks, root, desc, nk, nrec, err, errsz) != 0) {
            close(fd);
            return -1;
        }
        ntags = 1;
        keys = nk;
    } else if (sig == HSIG_CDX) {
        if (h_u16(buf + 4) != 1) {
            snprintf(err, errsz, "%s: bad CDX version", path);
            close(fd);
            return -1;
        }
        uint32_t blk = 0;
        int guard = 0;
        for (;;) {
            if (blk >= nblocks || ++guard > (int)nblocks + 1) {
                snprintf(err, errsz, "%s: dir chain out of range/loop", path);
                close(fd);
                return -1;
            }
            if (h_blk(fd, blk, buf) != 0) {
                snprintf(err, errsz, "%s: read dir %u", path, blk);
                close(fd);
                return -1;
            }
            if (h_u32(buf) != HSIG_CDX || h_u16(buf + 4) != 1) {
                snprintf(err, errsz, "%s: bad dir sig at %u", path, blk);
                close(fd);
                return -1;
            }
            uint16_t n = h_u16(buf + 6);
            uint32_t next = h_u32(buf + 8);
            if (n > (HBLOCK - HDIR_HDR) / HTAG_HDR) {
                snprintf(err, errsz, "%s: dir count %u too large", path, n);
                close(fd);
                return -1;
            }
            for (uint16_t i = 0; i < n; i++) {
                const uint8_t *h = buf + HDIR_HDR + (size_t)i * HTAG_HDR;
                int desc = (h_u16(h + 204) & 1) != 0;
                uint32_t root = h_u32(h + 206);
                uint64_t nk = h_u64(h + 222);
                if (h_tree_walk(fd, nblocks, root, desc, nk, nrec, err, errsz) != 0) {
                    close(fd);
                    return -1;
                }
                ntags++;
                keys += nk;
            }
            if (next == 0) {
                break;
            }
            blk = next;
        }
    } else {
        snprintf(err, errsz, "%s: bad signature 0x%08x", path, sig);
        close(fd);
        return -1;
    }
    close(fd);
    if (ntags_out) {
        *ntags_out = ntags;
    }
    if (keys_out) {
        *keys_out = keys;
    }
    return 0;
}

/* build a table of n records (NAME C20, SCORE N(8,2)) + fill the given tags;
   *ms_out receives the index build (insert+flush) wall time */
static int build_table(const char *dir, const char *stem, uint32_t n, const char **tags,
                       const char **exprs, const int *tagflags, size_t ntags, double *ms_out)
{
    char path[700];
    snprintf(path, sizeof(path), "%.660s/%.31s.dbf", dir, stem);
    rx_fielddef flds[2];
    memset(flds, 0, sizeof(flds));
    snprintf(flds[0].name, sizeof(flds[0].name), "NAME");
    flds[0].type = RFT_C;
    flds[0].len = 20;
    snprintf(flds[1].name, sizeof(flds[1].name), "SCORE");
    flds[1].type = RFT_N;
    flds[1].len = 8;
    flds[1].dec = 2;
    rx_dbf *dbf = NULL;
    int rc = rx_dbf_create(path, flds, 2, &dbf);
    if (rc != R_OK) {
        return rc;
    }
    for (uint32_t i = 1; i <= n; i++) {
        uint32_t h = i * 2654435761u;
        uint32_t recno;
        rc = rx_dbf_append(dbf, &recno);
        if (rc != R_OK) {
            break;
        }
        char nm[24];
        snprintf(nm, sizeof(nm), "N%07u", h % 10000000u);
        rval v;
        rx_rval_undef(&v);
        rval_set_str(&v, rx_strdup(nm), strlen(nm));
        rc = rx_dbf_put(dbf, 0, &v);
        rx_rval_free(&v);
        if (rc != R_OK) {
            break;
        }
        double score = (double)(h % 100000u) / 100.0;
        rx_rval_undef(&v);
        v.t = RX_T_N;
        v.u.n.d = score;
        v.u.n.dec = 2;
        rc = rx_dbf_put(dbf, 1, &v);
        rx_rval_free(&v);
        if (rc != R_OK) {
            break;
        }
        rc = rx_flush(dbf);
        if (rc != R_OK) {
            break;
        }
    }
    if (rc != R_OK) {
        rx_dbf_close(dbf);
        return rc;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t t = 0; t < ntags; t++) {
        rc = rx_index_create(dbf, tags[t], NULL, exprs[t], NULL, tagflags[t], 0);
        if (rc != R_OK) {
            rx_dbf_close(dbf);
            return rc;
        }
    }
    for (uint32_t r = 1; r <= n; r++) {
        rc = rx_dbf_goto(dbf, r);
        if (rc != R_OK) {
            break;
        }
        for (size_t t = 0; t < ntags; t++) {
            int fi = 0, ti = (int)t; /* structural CDX tags in creation order */
            size_t fld = strcmp(exprs[t], "NAME") == 0 ? 0 : 1;
            rval v;
            rx_rval_undef(&v);
            rc = rx_dbf_get(dbf, fld, &v);
            if (rc != R_OK) {
                break;
            }
            rc = rx_index_insert(dbf, fi, ti, &v, r);
            rx_rval_free(&v);
            if (rc != R_OK) {
                break;
            }
        }
        if (rc != R_OK) {
            break;
        }
    }
    if (rc == R_OK) {
        rc = rx_index_flush(dbf);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *ms_out = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    rx_dbf_close(dbf);
    return rc;
}

static int cdx_mode(const char *workroot)
{
    rx_data_session(1, 0x01); /* frozen header date: byte-identical output */
    mkdir(workroot, 0755); /* may exist */
    char dirA[700], dirB[700];
    snprintf(dirA, sizeof(dirA), "%.687s/cdxA-XXXXXX", workroot);
    snprintf(dirB, sizeof(dirB), "%.687s/cdxB-XXXXXX", workroot);
    if (rx_mkdtemp(dirA) != 0 || rx_mkdtemp(dirB) != 0) {
        perror("mkdtemp");
        return 1;
    }
    int ran = 0, bad = 0;
#define CDXCHK(cond, msg)                                                                      \
    do {                                                                                       \
        ran++;                                                                                 \
        if (!(cond)) {                                                                         \
            printf("FAIL %s\n", msg);                                                          \
            bad++;                                                                             \
        }                                                                                      \
    } while (0)

    /* 1. 100k-key structural CDX build, timed (spec §11.3 gate: < 2s for the
       index build itself: tag create + key insert + tree write) */
    const char *tags1[] = { "SCORE" };
    const char *exprs1[] = { "SCORE" };
    const int flags1[] = { 0 };
    double ms = 0;
    int rc = build_table(dirA, "big", 100000, tags1, exprs1, flags1, 1, &ms);
    CDXCHK(rc == R_OK, "build big.dbf + big.cdx (100k keys)");
    printf("cdx: 100k-key CDX build %.1f ms (gate < 2000 ms)\n", ms);
    CDXCHK(ms < 2000.0, "perf gate: 100k CDX build < 2s");
    char fA[800], dA[800];
    snprintf(fA, sizeof(fA), "%.791s/big.cdx", dirA);
    snprintf(dA, sizeof(dA), "%.791s/big.dbf", dirA);

    /* 2. determinism: identical 5k multi-tag build in a second dir ->
       byte-identical CDX and DBF */
    const char *tags3[] = { "NM", "SCORE", "SCD" };
    const char *exprs3[] = { "NAME", "SCORE", "SCORE" };
    const int flags3[] = { 0, 0, 1 /* RX_TAG_DESC */ };
    double ms3 = 0;
    rc = build_table(dirA, "multi", 5000, tags3, exprs3, flags3, 3, &ms3);
    CDXCHK(rc == R_OK, "build multi.dbf with 3 tags (dirA)");
    double ms3b = 0;
    rc = build_table(dirB, "multi", 5000, tags3, exprs3, flags3, 3, &ms3b);
    CDXCHK(rc == R_OK, "build multi.dbf with 3 tags (dirB)");
    char fM[800], fMB[800], dM[800], dMB[800];
    snprintf(fM, sizeof(fM), "%.790s/multi.cdx", dirA);
    snprintf(fMB, sizeof(fMB), "%.790s/multi.cdx", dirB);
    snprintf(dM, sizeof(dM), "%.790s/multi.dbf", dirA);
    snprintf(dMB, sizeof(dMB), "%.790s/multi.dbf", dirB);
    char diff[300] = "";
    CDXCHK(cmp_files(fM, fMB, diff, sizeof(diff)) == 0, "CDX byte-reproducible across runs");
    CDXCHK(cmp_files(dM, dMB, diff, sizeof(diff)) == 0, "DBF byte-reproducible across runs");

    /* 3. reopen + API verification: counts, monotonic order, seeks */
    rx_dbf *dbf = NULL;
    rc = rx_dbf_open(dA, RD_EXCLUSIVE, &dbf);
    CDXCHK(rc == R_OK, "reopen big.dbf");
    if (rc == R_OK) {
        int nf = rx_index_file_count(dbf);
        CDXCHK(nf == 1, "structural CDX auto-attached");
        CDXCHK(rx_tag_count(dbf, 0) == 1, "one tag in structural CDX");
        uint32_t kc = 0;
        rx_index_key_count(dbf, 0, 0, &kc);
        CDXCHK(kc == 100000, "KEYCOUNT == 100000");
        int fi, ti;
        rx_index_order_get(dbf, &fi, &ti);
        CDXCHK(fi == 0 && ti == 0, "first structural tag is the default order");
        /* monotonic scan of the controlling order */
        double prev = -1.0;
        int mono = 1;
        for (uint32_t p = 0; p < kc; p++) {
            uint32_t r = 0;
            if (rx_index_recno_at(dbf, p, &r) != R_OK) {
                mono = 0;
                break;
            }
            if (rx_dbf_goto(dbf, r) != R_OK) {
                mono = 0;
                break;
            }
            rval v;
            rx_rval_undef(&v);
            if (rx_dbf_get(dbf, 1, &v) != R_OK || v.t != RX_T_N || v.u.n.d < prev) {
                rx_rval_free(&v);
                mono = 0;
                break;
            }
            prev = v.u.n.d;
            rx_rval_free(&v);
        }
        CDXCHK(mono, "controlling order is monotonic over 100k keys");
        /* seeks: exact present + absent */
        rval probe;
        rx_rval_undef(&probe);
        probe.t = RX_T_N;
        probe.u.n.d = 500.00;
        probe.u.n.dec = 2;
        uint32_t recno = 0;
        int found = 0;
        rc = rx_index_seek(dbf, &probe, 0, &recno, &found); /* EXACT OFF: first >= probe */
        CDXCHK(rc == R_OK && found && recno >= 1 && recno <= 100000, "SEEK approximate hit");
        probe.u.n.d = -1.0;
        rc = rx_index_seek(dbf, &probe, 1, &recno, &found);
        CDXCHK(rc == R_OK && !found, "SEEK exact miss");
        rx_dbf_close(dbf);
    }

    /* 4. byte-level interop validator on the CDX */
    int ntags = 0;
    uint64_t keys = 0;
    char err[300] = "";
    rc = cdxcheck_file(fA, 100000, &ntags, &keys, err, sizeof(err));
    CDXCHK(rc == 0, "cdxcheck big.cdx structure");
    if (rc != 0) {
        printf("  cdxcheck: %s\n", err);
    }
    CDXCHK(ntags == 1 && keys == 100000, "cdxcheck tag/key counts");

    /* 5. byte-level validator on the multi-tag CDX (asc + desc tags) */
    rc = cdxcheck_file(fM, 5000, &ntags, &keys, err, sizeof(err));
    CDXCHK(rc == 0, "cdxcheck multi.cdx (asc + desc tags)");
    if (rc != 0) {
        printf("  cdxcheck: %s\n", err);
    }
    CDXCHK(ntags == 3 && keys == 15000, "cdxcheck multi tag/key counts");

    /* 6. standalone IDX round-trip through the public API */
    {
        char spath[700];
        snprintf(spath, sizeof(spath), "%.689s/small.dbf", dirA);
        rx_fielddef flds[1];
        memset(flds, 0, sizeof(flds));
        snprintf(flds[0].name, sizeof(flds[0].name), "K");
        flds[0].type = RFT_N;
        flds[0].len = 6;
        flds[0].dec = 0;
        rx_dbf *sd = NULL;
        rc = rx_dbf_create(spath, flds, 1, &sd);
        CDXCHK(rc == R_OK, "create small.dbf");
        if (rc == R_OK) {
            for (uint32_t i = 1; i <= 1000; i++) {
                uint32_t rn;
                rc = rx_dbf_append(sd, &rn);
                rval v;
                rx_rval_undef(&v);
                v.t = RX_T_N;
                v.u.n.d = (double)((i * 7919u) % 1000u);
                v.u.n.dec = 0;
                if (rc == R_OK) {
                    rc = rx_dbf_put(sd, 0, &v);
                }
                rx_rval_free(&v);
                if (rc == R_OK) {
                    rc = rx_flush(sd);
                }
                if (rc != R_OK) {
                    break;
                }
            }
            char ipath[700];
            snprintf(ipath, sizeof(ipath), "%.689s/small.idx", dirA);
            rc = rx_index_create(sd, "SMALL", ipath, "K", NULL, 0, 0);
            CDXCHK(rc == R_OK, "create standalone IDX tag");
            int fi = rx_index_file_count(sd) - 1, ti = 0; /* attached IDX file */
            for (uint32_t r = 1; r <= 1000 && rc == R_OK; r++) {
                rx_dbf_goto(sd, r);
                rval v;
                rx_rval_undef(&v);
                if (rx_dbf_get(sd, 0, &v) == R_OK) {
                    rc = rx_index_insert(sd, fi, ti, &v, r);
                    rx_rval_free(&v);
                }
            }
            CDXCHK(rc == R_OK, "fill small.idx");
            rx_index_flush(sd);
            rx_dbf_close(sd);
            rc = cdxcheck_file(ipath, 1000, &ntags, &keys, err, sizeof(err));
            CDXCHK(rc == 0, "cdxcheck small.idx structure");
            if (rc != 0) {
                printf("  cdxcheck: %s\n", err);
            }
            CDXCHK(ntags == 1 && keys == 1000, "cdxcheck IDX counts");
            /* reopen + attach + seek */
            rx_dbf *sd2 = NULL;
            rc = rx_dbf_open(spath, RD_EXCLUSIVE, &sd2);
            CDXCHK(rc == R_OK, "reopen small.dbf");
            if (rc == R_OK) {
                rx_idx *ix = NULL;
                rc = rx_index_attach(sd2, ipath, &ix);
                CDXCHK(rc == R_OK, "attach small.idx");
                if (rc == R_OK) {
                    int sfi = rx_index_file_count(sd2) - 1; /* the attached IDX */
                    uint32_t kc = 0;
                    rx_index_key_count(sd2, sfi, 0, &kc);
                    CDXCHK(kc == 1000, "IDX key count after reopen");
                    rx_index_order_set(sd2, sfi, 0);
                    rval probe;
                    rx_rval_undef(&probe);
                    probe.t = RX_T_N;
                    probe.u.n.d = 500;
                    probe.u.n.dec = 0;
                    uint32_t recno = 0;
                    int found = 0;
                    rc = rx_index_seek(sd2, &probe, 0, &recno, &found); /* first >= 500 */
                    CDXCHK(rc == R_OK && found, "IDX SEEK after reopen");
                }
                rx_dbf_close(sd2);
            }
        }
    }

    printf("cdx: %d checks, %d failures\n", ran, bad);
    rm_rf(dirA);
    rm_rf(dirB);
    return bad ? 1 : 0;
#undef CDXCHK
}

/* ---------------- scrcheck (M7 gate: .SCR round-trip + .RSO + codegen) ---------------- */

static int filter_scr(const struct dirent *d)
{
    size_t n = strlen(d->d_name);
    return n > 4 && strcmp(d->d_name + n - 4, ".scr") == 0;
}

static int scrcheck(const char *dir, const char *workroot)
{
    char absroot[1000];
    abs_path(workroot, absroot, sizeof(absroot));
    mkdir(absroot, 0755);
    struct dirent **names = NULL;
    int n = scandir(dir, &names, filter_scr, NULL);
    if (n < 0) {
        perror("scandir");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(names[i]->d_name, names[j]->d_name) > 0) {
                struct dirent *t = names[i];
                names[i] = names[j];
                names[j] = t;
            }
        }
    }
    int ran = 0;
    for (int i = 0; i < n; i++) {
        char base[1100], scrp[1200], goldp[1200];
        snprintf(base, sizeof(base), "%s/%s", dir, names[i]->d_name);
        base[strlen(base) - 4] = '\0';
        snprintf(scrp, sizeof(scrp), "%s.scr", base);
        snprintf(goldp, sizeof(goldp), "%s.prg.golden", base);
        char wd[1100];
        snprintf(wd, sizeof(wd), "%s/scr-XXXXXX", absroot);
        if (rx_mkdtemp(wd) != 0) {
            perror("mkdtemp");
            failures++;
            continue;
        }
        ran++;
        char rt[1200], rso[1200], rt2[1200], prg[1200];
        snprintf(rt, sizeof(rt), "%s/rt.scr", wd);
        snprintf(rso, sizeof(rso), "%s/x.rso", wd);
        snprintf(rt2, sizeof(rt2), "%s/rt2.scr", wd);
        snprintf(prg, sizeof(prg), "%s/x.prg", wd);
        char diff[300] = "";
        int bad = 0;
        scr_file sf;
        int rc = scrf_load(scrp, &sf);
        if (rc != R_OK) {
            snprintf(diff, sizeof(diff), "load failed rc=%d", rc);
            bad = 1;
        } else {
            /* 1. .SCR text round-trip must be byte-identical */
            if (scrf_save(rt, &sf) != R_OK || cmp_files(scrp, rt, diff, sizeof(diff)) != 0) {
                if (!diff[0]) {
                    snprintf(diff, sizeof(diff), "save failed");
                }
                bad = 1;
            }
            /* 2. .RSO compile + load must serialize identically */
            if (!bad && (scrf_compile(rso, &sf) != R_OK)) {
                snprintf(diff, sizeof(diff), "rso compile failed");
                bad = 1;
            }
            if (!bad) {
                scr_file sf2;
                if (scrf_load_rso(rso, &sf2) != R_OK) {
                    snprintf(diff, sizeof(diff), "rso load failed");
                    bad = 1;
                } else if (scrf_save(rt2, &sf2) != R_OK || cmp_files(rt, rt2, diff, sizeof(diff)) != 0) {
                    if (!diff[0]) {
                        snprintf(diff, sizeof(diff), "rso round-trip differs");
                    }
                    bad = 1;
                    scrf_free(&sf2);
                } else {
                    scrf_free(&sf2);
                }
            }
            /* 3. codegen snapshot */
            if (!bad) {
                strbuf sb;
                sb_init(&sb);
                if (scrf_generate(&sf, &sb) != R_OK) {
                    snprintf(diff, sizeof(diff), "generate failed");
                    bad = 1;
                } else {
                    FILE *f = fopen(prg, "w");
                    if (!f || fwrite(sb.buf ? sb.buf : "", 1, sb.len, f) != sb.len) {
                        snprintf(diff, sizeof(diff), "prg write failed");
                        bad = 1;
                    }
                    if (f) {
                        fclose(f);
                    }
                    sb_free(&sb);
                }
                if (!bad && access(goldp, F_OK) == 0 && cmp_files(prg, goldp, diff, sizeof(diff)) != 0) {
                    bad = 1;
                }
            }
            scrf_free(&sf);
        }
        if (bad) {
            printf("FAIL %s: %s\n", names[i]->d_name, diff);
            failures++;
        } else {
            printf("PASS %s\n", names[i]->d_name);
        }
        rm_rf(wd);
        free(names[i]);
    }
    free(names);
    printf("scrcheck: %d cases, %d failures\n", ran, failures);
    return failures ? 1 : 0;
}


/* ---------------- rptcheck (M8 gate: emitter determinism) ---------------- */

static int filter_frm(const struct dirent *d)
{
    size_t n = strlen(d->d_name);
    return (n > 4 && strcmp(d->d_name + n - 4, ".frm") == 0) ||
           (n > 4 && strcmp(d->d_name + n - 4, ".rlb") == 0);
}

static int rptcheck(const char *dir, const char *bin, const char *fixtures, const char *workroot)
{
    char absbin[1200], absroot[1000];
    abs_path(bin, absbin, sizeof(absbin));
    abs_path(workroot, absroot, sizeof(absroot));
    mkdir(absroot, 0755);
    struct dirent **names = NULL;
    int n = scandir(dir, &names, filter_frm, NULL);
    if (n < 0) {
        perror("scandir");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(names[i]->d_name, names[j]->d_name) > 0) {
                struct dirent *t = names[i];
                names[i] = names[j];
                names[j] = t;
            }
        }
    }
    int ran = 0;
    for (int i = 0; i < n; i++) {
        char base[1100];
        snprintf(base, sizeof(base), "%s/%s", dir, names[i]->d_name);
        size_t bl = strlen(base);
        int is_label = strcmp(base + bl - 4, ".rlb") == 0;
        base[bl - 4] = '\0';
        char wd[1100];
        snprintf(wd, sizeof(wd), "%s/rpt-XXXXXX", absroot);
        if (rx_mkdtemp(wd) != 0) {
            perror("mkdtemp");
            failures++;
            continue;
        }
        ran++;
        /* copy fixtures + the form as case.frm/case.rlb */
        struct dirent **fx = NULL;
        int nf = scandir(fixtures, &fx, NULL, alphasort);
        if (nf > 0) {
            for (int k = 0; k < nf; k++) {
                char sp[1400], dp[1400];
                snprintf(sp, sizeof(sp), "%s/%s", fixtures, fx[k]->d_name);
                snprintf(dp, sizeof(dp), "%s/%s", wd, fx[k]->d_name);
                struct stat st;
                if (fx[k]->d_name[0] != '.' && stat(sp, &st) == 0 && S_ISREG(st.st_mode)) {
                    copy_file(sp, dp);
                }
                free(fx[k]);
            }
            free(fx);
        }
        char formpath[1200], dstform[1200];
        snprintf(formpath, sizeof(formpath), "%s.%s", base, is_label ? "rlb" : "frm");
        snprintf(dstform, sizeof(dstform), "%s/case.%s", wd, is_label ? "rlb" : "frm");
        copy_file(formpath, dstform);
        /* driver program: run the form to every emitter */
        char prg[1200];
        snprintf(prg, sizeof(prg), "%s/case.prg", wd);
        FILE *pf = fopen(prg, "w");
        fprintf(pf, "USE people\n%s FORM case TO FILE out.txt\n", is_label ? "LABEL" : "REPORT");
        fprintf(pf, "%s FORM case TO FILE out.ps\n", is_label ? "LABEL" : "REPORT");
        fprintf(pf, "%s FORM case TO FILE out.pdf\n", is_label ? "LABEL" : "REPORT");
        fprintf(pf, "%s FORM case TO FILE out.html\n", is_label ? "LABEL" : "REPORT");
        fprintf(pf, "%s FORM case TO FILE out.csv\n", is_label ? "LABEL" : "REPORT");
        fclose(pf);
        char outp[1200], errp[1200];
        snprintf(outp, sizeof(outp), "%s/out.txt.log", wd);
        snprintf(errp, sizeof(errp), "%s/err.txt", wd);
        pid_t pid = fork();
        if (pid == 0) {
            if (chdir(wd) != 0) {
                _exit(99);
            }
            setenv("HOME", wd, 1);
            FILE *fe = freopen(errp, "w", stderr);
            (void)fe;
            execl(absbin, "reynard", "-t", "-o", "stdout.txt", "case.prg", (char *)NULL);
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        int bad = 0;
        char diff[300] = "";
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            size_t ne;
            char *e = read_file(errp, &ne);
            snprintf(diff, sizeof(diff), "run failed; stderr: %s", e ? e : "(none)");
            free(e);
            bad = 1;
        }
        /* compare: .txt.golden byte-exact; .ps/.pdf/.html/.csv.<sha256> hash-stable */
        static const char *exts[5] = { "txt", "ps", "pdf", "html", "csv" };
        for (int ei = 0; ei < 5 && !bad; ei++) {
            char gp[1200], op[1200];
            snprintf(gp, sizeof(gp), "%s.%s.golden", base, exts[ei]);
            snprintf(op, sizeof(op), "%s/out.%s", wd, exts[ei]);
            if (access(gp, F_OK) != 0) {
                continue; /* no golden for this emitter */
            }
            if (ei == 0) {
                if (cmp_files(op, gp, diff, sizeof(diff)) != 0) {
                    bad = 1;
                }
            } else {
                /* hash compare: golden holds the sha256 hex of the output */
                size_t gn;
                char *want = read_file(gp, &gn);
                size_t on;
                char *ob = read_file(op, &on);
                uint8_t h[32];
                char hex[65];
                rx_sha256(ob ? ob : "", on, h);
                for (int hi = 0; hi < 32; hi++) {
                    snprintf(hex + hi * 2, 3, "%02x", h[hi]);
                }
                /* trim want */
                size_t wl = gn;
                while (wl && (want[wl - 1] == '\n' || want[wl - 1] == '\r' || want[wl - 1] == ' ')) {
                    wl--;
                }
                if (!want || strncmp(want, hex, wl < 64 ? wl : 64) != 0 || wl != 64) {
                    snprintf(diff, sizeof(diff), "%s hash mismatch (got %.16s...)", exts[ei], hex);
                    bad = 1;
                }
                free(want);
                free(ob);
            }
        }
        if (bad) {
            printf("FAIL %s: %s\n", names[i]->d_name, diff);
            failures++;
        } else {
            printf("PASS %s\n", names[i]->d_name);
        }
        rm_rf(wd);
        free(names[i]);
    }
    free(names);
    printf("rptcheck: %d cases, %d failures\n", ran, failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    rx_log_init();
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        int before = failures;
        selftest_mouse();
        selftest_term();
        if (failures == before) {
            printf("selftest: PASS\n");
            return 0;
        }
        printf("selftest: %d failures\n", failures - before);
        return 1;
    }
    if (argc >= 6 && strcmp(argv[1], "conformance") == 0) {
        return conformance(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc >= 6 && strcmp(argv[1], "rxp") == 0) {
        return rxp_mode(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc >= 4 && strcmp(argv[1], "roundtrip") == 0) {
        return roundtrip(argv[2], argv[3]);
    }
    if (argc >= 3 && strcmp(argv[1], "lockrace") == 0) {
        return lockrace(argv[2]);
    }
    if (argc >= 6 && strcmp(argv[1], "golden") == 0) {
        int update = argc >= 7 && strcmp(argv[6], "update") == 0;
        return golden(argv[2], argv[3], argv[4], argv[5], update);
    }
    if (argc >= 4 && strcmp(argv[1], "browseperf") == 0) {
        return browseperf(argv[2], argv[3]);
    }
    if (argc >= 3 && strcmp(argv[1], "cdx") == 0) {
        return cdx_mode(argv[2]);
    }
    if (argc >= 4 && strcmp(argv[1], "scrcheck") == 0) {
        return scrcheck(argv[2], argv[3]);
    }
    if (argc >= 6 && strcmp(argv[1], "rptcheck") == 0) {
        return rptcheck(argv[2], argv[3], argv[4], argv[5]);
    }
    fprintf(stderr, "usage: %s selftest | conformance <dir> <reynard> <fixtures> <workroot>"
                    " | rxp <dir> <reynard> <fixtures> <workroot>"
                    " | roundtrip <fixtures> <workroot> | lockrace <workroot>"
                    " | golden <dir> <reynard> <fixtures> <workroot> [update]"
                    " | browseperf <workroot> <reynard> | cdx <workroot> | scrcheck <dir> <workroot>\n"
                    " | rptcheck <dir> <reynard> <fixtures> <workroot>\n",
            argv[0]);
    return 2;
}
