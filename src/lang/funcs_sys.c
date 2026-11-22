/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file funcs_sys.c
 * @brief Environment/system functions (spec Appendix A, funcs_sys.c).
 *        SYS(5), GETENV, CAPSLOCK etc. map to Unix semantics; MEMORY()
 *        and DISKSPACE() return documented synthetic values
 *        (docs/DEVIATIONS.md).
 */
/* POSIX system headers come before project headers here: <unistd.h>
   defines R_OK for access(2), and reynard/status.h must own R_OK (=0). */
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "lang/funcs.h"
#include "util/util.h"

static int f_os(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "OS() expects no arguments");
        return err->code;
    }
    (void)argv;
#if defined(__linux__)
    const char *v = "Linux";
#elif defined(__APPLE__)
    const char *v = "macOS";
#elif defined(__FreeBSD__)
    const char *v = "FreeBSD";
#elif defined(__OpenBSD__)
    const char *v = "OpenBSD";
#elif defined(__NetBSD__)
    const char *v = "NetBSD";
#else
    const char *v = "Unix";
#endif
    f_str(out, rx_strdup(v), strlen(v));
    (void)c;
    return R_OK;
}

static int f_version(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "VERSION() expects no arguments");
        return err->code;
    }
    (void)argv;
    const char *v = "Reynard 0.1.0 (FoxPro 2.6a compatible)";
    f_str(out, rx_strdup(v), strlen(v));
    (void)c;
    return R_OK;
}

static int f_getenv(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (f_argc(err, argc, 1, "GETENV") != 0) {
        return err->code;
    }
    const char *s;
    size_t n;
    if (!rv_to_str(&argv[0], &s, &n)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "GETENV() expects character");
        return err->code;
    }
    char nm[256];
    size_t k = n < 255 ? n : 255;
    memcpy(nm, s, k);
    nm[k] = '\0';
    const char *v = getenv(nm);
    if (!v) {
        v = "";
    }
    f_str(out, rx_strdup(v), strlen(v));
    (void)c;
    return R_OK;
}

static int f_memory(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "MEMORY() expects no arguments");
        return err->code;
    }
    (void)argv;
    /* Synthetic Unix value, documented deviation (spec Appendix A). */
    f_num(c, out, 640.0);
    return R_OK;
}

static int f_diskspace(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "DISKSPACE() expects 0 or 1 arguments");
        return err->code;
    }
    const char *path = ".";
    if (argc == 1) {
        const char *s;
        size_t n;
        if (rv_to_str(&argv[0], &s, &n)) {
            static _Thread_local char buf[260];
            size_t k = n < 259 ? n : 259;
            memcpy(buf, s, k);
            buf[k] = '\0';
            path = buf;
        }
    }
    struct statvfs sv;
    if (statvfs(path, &sv) != 0) {
        err->code = R_ERR_OS;
        snprintf(err->msg, sizeof(err->msg), "DISKSPACE() statvfs failed");
        return err->code;
    }
    f_num(c, out, (double)sv.f_bavail * (double)sv.f_frsize);
    return R_OK;
}

static int f_sys(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (f_argc(err, argc, 1, "SYS") != 0) {
        return err->code;
    }
    double k;
    if (!rv_to_num(&argv[0], &k)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "SYS() expects numeric");
        return err->code;
    }
    switch ((int)k) {
    case 2: /* seconds since midnight; frozen in -t */
        f_num(c, out, c->test_mode ? 0 : 0 + (double)0);
        if (!c->test_mode) {
            time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            out->u.n.d = tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec;
        }
        break;
    case 5: { /* default drive/directory -> Unix current/default dir */
        char buf[260];
        if (c->set.defdir[0]) {
            snprintf(buf, sizeof(buf), "%s", c->set.defdir);
        } else if (getcwd(buf, sizeof(buf))) {
        } else {
            snprintf(buf, sizeof(buf), "/");
        }
        f_str(out, rx_strdup(buf), strlen(buf));
        break;
    }
    case 12:
        f_num(c, out, 640.0);
        break;
    case 2015:
        f_str(out, rx_strdup("V"), 1);
        break;
    default:
        f_num(c, out, 0);
        break;
    }
    return R_OK;
}

static int f_logic_const(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err,
                         rx_logic v, const char *nm)
{
    if (argc > 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "%s() expects 0 or 1 arguments", nm);
        return err->code;
    }
    if (argc == 1) {
        rx_logic l;
        if (!rv_to_logic(&argv[0], &l)) {
            err->code = R_ERR_TYPE;
            snprintf(err->msg, sizeof(err->msg), "%s() expects logical", nm);
            return err->code;
        }
        v = l;
    }
    f_logic(out, v);
    (void)c;
    return R_OK;
}

static int f_capslock(rx_ctx *c, size_t a, const rval *v, rval *o, r_err *e)
{
    return f_logic_const(c, a, v, o, e, L_F, "CAPSLOCK");
}

static int f_numlock(rx_ctx *c, size_t a, const rval *v, rval *o, r_err *e)
{
    return f_logic_const(c, a, v, o, e, L_T, "NUMLOCK");
}

static int f_insmode(rx_ctx *c, size_t a, const rval *v, rval *o, r_err *e)
{
    return f_logic_const(c, a, v, o, e, L_F, "INSMODE");
}

static int f_inkey(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 1) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "INKEY() expects 0 or 1 arguments");
        return err->code;
    }
    (void)argv;
    f_num(c, out, 0); /* batch: no key pending */
    return R_OK;
}

static int f_lastkey(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "LASTKEY() expects no arguments");
        return err->code;
    }
    (void)argv;
    f_num(c, out, 0);
    (void)c;
    return R_OK;
}

static int f_readkey(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "READKEY() expects no arguments");
        return err->code;
    }
    (void)argv;
    f_num(c, out, 0);
    (void)c;
    return R_OK;
}

static int f_set(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc < 1 || argc > 2) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "SET() expects 1 or 2 arguments");
        return err->code;
    }
    const char *s;
    size_t n;
    if (!rv_to_str(&argv[0], &s, &n)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "SET() expects character option name");
        return err->code;
    }
    char nm[32];
    size_t k = n < 31 ? n : 31;
    memcpy(nm, s, k);
    nm[k] = '\0';
    rx_strtoupper(nm);
    char buf[1100];
    buf[0] = '\0';
    if (rx_strcasecmp(nm, "TALK") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.talk ? "ON" : "OFF");
    } else if (rx_strcasecmp(nm, "HEADINGS") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.headings ? "ON" : "OFF");
    } else if (rx_strcasecmp(nm, "EXACT") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.exact ? "ON" : "OFF");
    } else if (rx_strcasecmp(nm, "DECIMALS") == 0) {
        snprintf(buf, sizeof(buf), "%d", c->set.decimals);
    } else if (rx_strcasecmp(nm, "CENTURY") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.century ? "ON" : "OFF");
    } else if (rx_strcasecmp(nm, "DELETED") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.deleted ? "ON" : "OFF");
    } else if (rx_strcasecmp(nm, "ESCAPE") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.escape ? "ON" : "OFF");
    } else if (rx_strcasecmp(nm, "FIXED") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.fixed ? "ON" : "OFF");
    } else if (rx_strcasecmp(nm, "CONSOLE") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.console ? "ON" : "OFF");
    } else if (rx_strcasecmp(nm, "SAFETY") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.safety ? "ON" : "OFF");
    } else if (rx_strcasecmp(nm, "MOUSE") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.mouse ? "ON" : "OFF");
    } else if (rx_strcasecmp(nm, "PATH") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.path);
    } else if (rx_strcasecmp(nm, "DEFAULT") == 0) {
        snprintf(buf, sizeof(buf), "%s", c->set.defdir);
    } else {
        snprintf(buf, sizeof(buf), "%s", "ON"); /* accepted-but-unimplemented options */
    }
    f_str(out, rx_strdup(buf), strlen(buf));
    return R_OK;
}

static int f_scols(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "SCOLS() expects no arguments");
        return err->code;
    }
    (void)argv;
    f_num(c, out, 80);
    return R_OK;
}

static int f_srows(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "SROWS() expects no arguments");
        return err->code;
    }
    (void)argv;
    f_num(c, out, 24);
    return R_OK;
}

static int f_curdir(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (argc > 0) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "CURDIR() expects no arguments");
        return err->code;
    }
    (void)argv;
    char buf[260];
    if (!getcwd(buf, sizeof(buf))) {
        snprintf(buf, sizeof(buf), "/");
    }
    f_str(out, rx_strdup(buf), strlen(buf));
    (void)c;
    return R_OK;
}

static int f_file(rx_ctx *c, size_t argc, const rval *argv, rval *out, r_err *err)
{
    if (f_argc(err, argc, 1, "FILE") != 0) {
        return err->code;
    }
    const char *s;
    size_t n;
    if (!rv_to_str(&argv[0], &s, &n)) {
        err->code = R_ERR_TYPE;
        snprintf(err->msg, sizeof(err->msg), "FILE() expects character");
        return err->code;
    }
    char nm[260];
    size_t k = n < 259 ? n : 259;
    memcpy(nm, s, k);
    nm[k] = '\0';
    f_logic(out, access(nm, F_OK) == 0 ? L_T : L_F);
    (void)c;
    return R_OK;
}

const rx_funcdef rx_funcs_sys[] = {
    { "OS", f_os },           { "VERSION", f_version }, { "GETENV", f_getenv },
    { "MEMORY", f_memory },   { "DISKSPACE", f_diskspace }, { "SYS", f_sys },
    { "CAPSLOCK", f_capslock }, { "NUMLOCK", f_numlock }, { "INSMODE", f_insmode },
    { "INKEY", f_inkey },     { "LASTKEY", f_lastkey }, { "READKEY", f_readkey },
    { "SET", f_set },         { "SCOLS", f_scols },     { "SROWS", f_srows },
    { "CURDIR", f_curdir },   { "FILE", f_file },
};

const size_t rx_funcs_sys_n = sizeof(rx_funcs_sys) / sizeof(rx_funcs_sys[0]);
