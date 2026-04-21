/*
 * CAE return address stack — internal helper, embedded in predictors.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef HW_CAE_BPRED_RAS_H
#define HW_CAE_BPRED_RAS_H

#include <stdint.h>
#include <stdbool.h>
#include "qapi/error.h"

typedef struct CaeRas {
    uint32_t depth;     /* capacity               */
    uint32_t top;       /* index of MRU entry + 1 */
    uint64_t *stack;    /* [depth]                */
    uint64_t pushes;
    uint64_t pops;
    uint64_t overflows; /* push over full stack — drops oldest */
    uint64_t underflows;/* pop when empty                      */
} CaeRas;

bool cae_ras_init(CaeRas *ras, uint32_t depth, Error **errp);
void cae_ras_release(CaeRas *ras);
void cae_ras_reset(CaeRas *ras);
void cae_ras_push(CaeRas *ras, uint64_t return_addr);
bool cae_ras_pop(CaeRas *ras, uint64_t *return_addr_out);
/* Non-destructive peek; useful for prediction without commit. */
bool cae_ras_peek(const CaeRas *ras, uint64_t *return_addr_out);

#endif /* HW_CAE_BPRED_RAS_H */
