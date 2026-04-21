/*
 * CAE return address stack — internal helper, embedded in predictors.
 *
 * Fixed-depth ring-buffer stack. Overflow drops the oldest entry and
 * keeps the most recent push; underflow returns false without touching
 * the caller's output. Used by concrete predictors to supply return
 * targets for function returns (JALR rd=x0 rs1=ra in RV ABI).
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/cae/bpred/ras.h"

bool cae_ras_init(CaeRas *ras, uint32_t depth, Error **errp)
{
    if (depth == 0) {
        error_setg(errp, "RAS depth must be non-zero");
        return false;
    }
    ras->depth = depth;
    ras->top = 0;
    ras->stack = g_new0(uint64_t, depth);
    ras->pushes = 0;
    ras->pops = 0;
    ras->overflows = 0;
    ras->underflows = 0;
    return true;
}

void cae_ras_release(CaeRas *ras)
{
    if (!ras) {
        return;
    }
    g_free(ras->stack);
    ras->stack = NULL;
    ras->depth = 0;
    ras->top = 0;
}

void cae_ras_reset(CaeRas *ras)
{
    if (!ras || !ras->stack) {
        return;
    }
    memset(ras->stack, 0, sizeof(uint64_t) * ras->depth);
    ras->top = 0;
    ras->pushes = 0;
    ras->pops = 0;
    ras->overflows = 0;
    ras->underflows = 0;
}

void cae_ras_push(CaeRas *ras, uint64_t return_addr)
{
    if (!ras || !ras->stack) {
        return;
    }
    if (ras->top == ras->depth) {
        /*
         * Full. Drop the oldest entry by shifting down, then place the
         * new entry at the top. Keeps recency for recursive workloads.
         */
        memmove(&ras->stack[0], &ras->stack[1],
                sizeof(uint64_t) * (ras->depth - 1));
        ras->stack[ras->depth - 1] = return_addr;
        ras->overflows++;
    } else {
        ras->stack[ras->top] = return_addr;
        ras->top++;
    }
    ras->pushes++;
}

bool cae_ras_pop(CaeRas *ras, uint64_t *return_addr_out)
{
    if (!ras || !ras->stack) {
        return false;
    }
    if (ras->top == 0) {
        ras->underflows++;
        return false;
    }
    ras->top--;
    if (return_addr_out) {
        *return_addr_out = ras->stack[ras->top];
    }
    ras->pops++;
    return true;
}

bool cae_ras_peek(const CaeRas *ras, uint64_t *return_addr_out)
{
    if (!ras || !ras->stack || ras->top == 0) {
        return false;
    }
    if (return_addr_out) {
        *return_addr_out = ras->stack[ras->top - 1];
    }
    return true;
}
