/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file scrfmt.h
 * @brief Screen file formats (spec 9.2, M7): .SCR text model, .RSO compiled
 *        form, .PRG code generation. Lives in util so the language layer can
 *        GENERATE SCREEN headless; the interactive designer is in ui/screenb.c.
 *        Layouts are specified in docs/formats.md section 6.
 */
#ifndef REYNARD_SCRFMT_H
#define REYNARD_SCRFMT_H

#include <stddef.h>
#include "util/util.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SCR_SAY = 1,
    SCR_GET,
    SCR_BOX,
    SCR_LINE,
    SCR_BUTTON,
    SCR_RADIO,
    SCR_CHECK,
    SCR_POPUP,
    SCR_SPIN,
    SCR_INVB
};

typedef struct scr_item {
    int type;
    int row, col;      /**< SAY/GET/controls: position; BOX/LINE: corner 1 */
    int row2, col2;    /**< BOX/LINE: corner 2 */
    int h, w;          /**< SIZE h,w (GET/INVB) */
    int scheme;
    char text[256];    /**< SAY text / control prompt */
    char var[64];      /**< GET/control variable */
    char pic[64];      /**< GET PICTURE */
    char func[16];     /**< GET FUNCTION */
    char when[128];    /**< GET WHEN raw expr */
    char valid[128];   /**< GET VALID raw expr */
    char rang[96];     /**< GET RANGE raw text */
    char deflt[96];    /**< GET DEFAULT raw expr */
} scr_item;

typedef struct scr_file {
    char name[64];
    int h, w; /**< screen size (designer canvas size) */
    scr_item *items;
    size_t nitems, capitems;
} scr_file;

void scrf_init(scr_file *sf, const char *name);
void scrf_free(scr_file *sf);
scr_item *scrf_add(scr_file *sf);
const char *scrf_type_name(int t);
int scrf_load_text(const char *text, scr_file *sf);
int scrf_load(const char *path, scr_file *sf);
int scrf_save(const char *path, const scr_file *sf);
int scrf_serialize(const scr_file *sf, strbuf *sb);
int scrf_compile(const char *rsopath, const scr_file *sf);
int scrf_load_rso(const char *rsopath, scr_file *sf);
int scrf_generate(const scr_file *sf, strbuf *out);

#ifdef __cplusplus
}
#endif

#endif /* REYNARD_SCRFMT_H */
