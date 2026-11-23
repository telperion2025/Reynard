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
#include <stdio.h>
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
    fprintf(stderr, "usage: %s selftest | conformance <dir> <reynard> <fixtures> <workroot>"
                    " | rxp <dir> <reynard> <fixtures> <workroot>"
                    " | roundtrip <fixtures> <workroot> | lockrace <workroot>"
                    " | golden <dir> <reynard> <fixtures> <workroot> [update]"
                    " | browseperf <workroot> <reynard>\n",
            argv[0]);
    return 2;
}
