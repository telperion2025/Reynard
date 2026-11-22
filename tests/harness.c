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
    CHECK(pairs_seen == 64, "palette has 64 slots");

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
        char outp[1200], errp[1200];
        snprintf(outp, sizeof(outp), "%s/out.txt", wd);
        snprintf(errp, sizeof(errp), "%s/err.txt", wd);
        pid_t pid = fork();
        if (pid == 0) {
            if (chdir(wd) != 0) {
                _exit(99);
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
    fprintf(stderr, "usage: %s selftest | conformance <dir> <reynard> <fixtures> <workroot>\n",
            argv[0]);
    return 2;
}
