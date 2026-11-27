/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file errlist.c
 * @brief Appendix D error catalog: code -> short string (English, v1.0).
 */
#include "util/util.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int code;
    const char* str;
} entry;

static const entry catalog[] = {
    {R_OK, "OK"},
    {R_ERR_IO_OPEN, "Cannot open file"},
    {R_ERR_IO_CLOSE, "Cannot close file"},
    {R_ERR_IO_READ, "Read error"},
    {R_ERR_IO_WRITE, "Write error"},
    {R_ERR_IO_SEEK, "Seek error"},
    {R_ERR_IO_EOF, "End of file"},
    {R_ERR_IO_NOTFOUND, "File not found"},
    {R_ERR_IO_PERM, "Permission denied"},
    {R_ERR_DBF_MAGIC, "Not a DBF file (bad magic)"},
    {R_ERR_DBF_HEADER, "Corrupt DBF header"},
    {R_ERR_DBF_FIELDS, "Corrupt DBF field descriptors"},
    {R_ERR_DBF_RECOFF, "Record offset out of range"},
    {R_ERR_DBF_MEMO, "Memo file error"},
    {R_ERR_DBF_CODEPAGE, "Unknown code page mark"},
    {R_ERR_DBF_BADRXP, "Corrupt or mismatched compiled program"},
    {R_ERR_DBF_TRUNCATED, "File truncated"},
    {R_ERR_DBF_UNSUPPORTED, "Unsupported feature for this file"},
    {R_ERR_DBF_NOTOPEN, "No table open in work area"},
    {R_ERR_DBF_RECNO, "Record number out of range"},
    {R_ERR_CDX_NODE, "Corrupt CDX node"},
    {R_ERR_CDX_TAG, "Tag not found"},
    {R_ERR_CDX_VARIANT, "Incompatible index variant"},
    {R_ERR_LOCK_CONFLICT, "Record is locked by another user"},
    {R_ERR_FILE_IN_USE, "File is in use by another user"},
    {R_ERR_LOCK_PERM, "Lock permission denied"},
    {R_ERR_SYNTAX, "Syntax error"},
    {R_ERR_VAR, "Variable not found"},
    {R_ERR_FIELD, "Unknown field"},
    {R_ERR_TYPE, "Data type mismatch"},
    {R_ERR_DIVZERO, "Division by zero"},
    {R_ERR_MACRO_DEPTH, "Macro expansion depth limit exceeded"},
    {R_ERR_UNDEF_FUNC, "Undefined function"},
    {R_ERR_STACK, "Stack overflow (procedure depth limit)"},
    {R_ERR_RANGE, "Value out of range"},
    {R_ERR_NOUI, "Command requires the interactive UI"},
    {R_ERR_INTERACTIVE_ONLY, "Command is interactive-only"},
    {R_ERR_EMPTY, "Empty value misuse"},
    {R_ERR_STR_OVERFLOW, "String too long"},
    {R_ERR_DATE_RANGE, "Date out of range"},
    {R_ERR_NO_CONTINUE, "CONTINUE without LOCATE"},
    {R_ERR_BROWSE, "Browse failure"},
    {R_ERR_OS, "OS error"},
};

const char* rx_status_str(int code) {
    for (size_t i = 0; i < sizeof(catalog) / sizeof(catalog[0]); i++) {
        if (catalog[i].code == code) {
            return catalog[i].str;
        }
    }
    return "Internal error";
}

void rx_status_message(int code, char* msg, size_t msgsz) {
    snprintf(msg, msgsz, "%s", rx_status_str(code));
}
