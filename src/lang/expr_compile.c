/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file expr_compile.c
 * @brief Compiles expression ASTs to linear bytecode (spec §7.2/§7.3).
 *        .AND./.OR. compile to short-circuit jumps (lazy evaluation,
 *        spec §7.6). The .RXP on-disk cache lands at M5.
 */
#include "lang/lang.h"

#include <stdlib.h>
#include <string.h>

#include "util/util.h"

static int emit(rx_code* c, uint8_t op, uint32_t arg) {
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 32;
        uint8_t* no = realloc(c->ops, nc);
        uint32_t* na = realloc(c->arg, nc * sizeof(uint32_t));
        if (!no || !na) {
            RX_FATAL("out of memory");
        }
        c->ops = no;
        c->arg = na;
        c->cap = nc;
    }
    c->ops[c->n] = op;
    c->arg[c->n] = arg;
    c->n++;
    return R_OK;
}

static uint32_t add_str(rx_code* c, const char* s) {
    if (c->nstrs == c->capstrs) {
        size_t nc = c->capstrs ? c->capstrs * 2 : 8;
        char** ns = realloc(c->strs, nc * sizeof(char*));
        if (!ns) {
            RX_FATAL("out of memory");
        }
        c->strs = ns;
        c->capstrs = nc;
    }
    c->strs[c->nstrs] = rx_strdup(s);
    return (uint32_t)c->nstrs++;
}

static uint32_t add_num(rx_code* c, double d) {
    if (c->nnums == c->capnums) {
        size_t nc = c->capnums ? c->capnums * 2 : 8;
        double* nn = realloc(c->nums, nc * sizeof(double));
        if (!nn) {
            RX_FATAL("out of memory");
        }
        c->nums = nn;
        c->capnums = nc;
    }
    c->nums[c->nnums] = d;
    return (uint32_t)c->nnums++;
}

static int compile(const rx_expr* e, rx_code* c, r_err* err) {
    (void)err;
    switch (e->kind) {
    case EX_NUM:
        emit(c, BOP_PUSH_NUM, add_num(c, e->lit.u.n.d));
        return R_OK;
    case EX_STR:
        emit(c, BOP_PUSH_STR, add_str(c, e->lit.u.c.utf8 ? e->lit.u.c.utf8 : ""));
        return R_OK;
    case EX_LOGIC:
        emit(c, BOP_PUSH_LOGIC, (uint32_t)e->lit.u.l);
        return R_OK;
    case EX_DATE:
    case EX_UNDEF:
        emit(c, BOP_PUSH_DATE, add_str(c, e->lit.u.c.utf8 ? e->lit.u.c.utf8 : ""));
        return R_OK;
    case EX_VAR:
        emit(c, BOP_VAR, add_str(c, e->name));
        return R_OK;
    case EX_FIELD:
        emit(c, BOP_FIELD, add_str(c, e->name));
        return R_OK;
    case EX_UN: {
        int rc = compile(e->r, c, err);
        if (rc != R_OK) {
            return rc;
        }
        if (e->op == OP_MINUS) {
            emit(c, BOP_NEG, 0);
        } else if (e->op == OP_NOT_LOGICAL) {
            emit(c, BOP_NOT, 0);
        }
        /* unary plus is a no-op */
        return R_OK;
    }
    case EX_BIN: {
        if (e->op == OP_AND_LOGICAL) {
            int rc = compile(e->l, c, err);
            if (rc != R_OK) {
                return rc;
            }
            size_t jmpf = c->n;
            emit(c, BOP_JMPF, 0); /* patch */
            rc = compile(e->r, c, err);
            if (rc != R_OK) {
                return rc;
            }
            c->arg[jmpf] = (uint32_t)c->n;
            return R_OK;
        }
        if (e->op == OP_OR_LOGICAL) {
            int rc = compile(e->l, c, err);
            if (rc != R_OK) {
                return rc;
            }
            size_t jmpt = c->n;
            emit(c, BOP_JMPT, 0);
            rc = compile(e->r, c, err);
            if (rc != R_OK) {
                return rc;
            }
            c->arg[jmpt] = (uint32_t)c->n;
            return R_OK;
        }
        int rc = compile(e->l, c, err);
        if (rc != R_OK) {
            return rc;
        }
        rc = compile(e->r, c, err);
        if (rc != R_OK) {
            return rc;
        }
        uint8_t bop;
        switch (e->op) {
        case OP_PLUS:
            bop = BOP_ADD;
            break;
        case OP_MINUS:
            bop = BOP_SUB;
            break;
        case OP_MUL:
            bop = BOP_MUL;
            break;
        case OP_DIV:
            bop = BOP_DIV;
            break;
        case OP_MOD:
            bop = BOP_MOD;
            break;
        case OP_POW:
            bop = BOP_POW;
            break;
        case OP_EQ:
            bop = BOP_EQ;
            break;
        case OP_EEQ:
            bop = BOP_EEQ;
            break;
        case OP_NE:
            bop = BOP_NE;
            break;
        case OP_LT:
            bop = BOP_LT;
            break;
        case OP_LE:
            bop = BOP_LE;
            break;
        case OP_GT:
            bop = BOP_GT;
            break;
        case OP_GE:
            bop = BOP_GE;
            break;
        case OP_IN:
            bop = BOP_IN;
            break;
        default:
            bop = BOP_NOP;
            break;
        }
        emit(c, bop, 0);
        return R_OK;
    }
    case EX_FUNC: {
        emit(c, BOP_PUSH_STR, add_str(c, e->name));
        for (size_t i = 0; i < e->nargs; i++) {
            /* a bare variable argument may be an array reference (AELEMENT(a,..)):
               BOP_ARGNAME pushes the name for arrays, the value otherwise */
            if (e->args[i]->kind == EX_VAR) {
                emit(c, BOP_ARGNAME, add_str(c, e->args[i]->name));
            } else {
                int rc = compile(e->args[i], c, err);
                if (rc != R_OK) {
                    return rc;
                }
            }
        }
        emit(c, BOP_FUNC, (uint32_t)e->nargs);
        return R_OK;
    }
    case EX_ARRAY: {
        /* M5: a[i, j] -> push name, evaluate indexes, BOP_ARRAY_LOAD argc */
        emit(c, BOP_PUSH_STR, add_str(c, e->name));
        for (size_t i = 0; i < e->nargs; i++) {
            int rc = compile(e->args[i], c, err);
            if (rc != R_OK) {
                return rc;
            }
        }
        emit(c, BOP_ARRAY_LOAD, (uint32_t)e->nargs);
        return R_OK;
    }
    }
    return R_ERR_SYNTAX;
}

int rx_compile_expr(const rx_expr* e, rx_code* code, r_err* err) {
    memset(code, 0, sizeof(*code));
    return compile(e, code, err);
}

void rx_code_free(rx_code* code) {
    free(code->ops);
    free(code->arg);
    free(code->nums);
    for (size_t i = 0; i < code->nstrs; i++) {
        free(code->strs[i]);
    }
    free(code->strs);
    memset(code, 0, sizeof(*code));
}
