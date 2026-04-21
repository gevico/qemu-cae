/*
 * CAE branch target buffer — internal helper, embedded in predictors.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef HW_CAE_BPRED_BTB_H
#define HW_CAE_BPRED_BTB_H

#include <stdint.h>
#include <stdbool.h>
#include "qapi/error.h"

typedef struct CaeBtb {
    uint32_t num_entries;
    uint32_t assoc;
    uint32_t num_sets;
    uint32_t index_bits;

    uint64_t *tags;     /* [num_entries]                       */
    uint64_t *targets;  /* [num_entries]                       */
    bool *valid;        /* [num_entries]                       */
    uint16_t *lru;      /* [num_entries] — MRU at set*assoc+0  */

    uint64_t hits;
    uint64_t misses;
} CaeBtb;

bool cae_btb_init(CaeBtb *btb, uint32_t num_entries, uint32_t assoc,
                  Error **errp);
void cae_btb_release(CaeBtb *btb);
void cae_btb_reset(CaeBtb *btb);
bool cae_btb_lookup(CaeBtb *btb, uint64_t pc, uint64_t *target_out);
void cae_btb_insert(CaeBtb *btb, uint64_t pc, uint64_t target);
void cae_btb_invalidate(CaeBtb *btb, uint64_t pc);

#endif /* HW_CAE_BPRED_BTB_H */
