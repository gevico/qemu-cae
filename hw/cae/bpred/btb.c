/*
 * CAE branch target buffer helpers.
 *
 * Set-associative BTB used by direction predictors to supply the
 * predicted target of taken / unconditional / indirect branches.
 * Exposed as internal functions only (no QOM type of its own); the
 * owning predictor embeds a CaeBtb inside its instance struct.
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
#include "hw/cae/bpred/btb.h"

static inline bool is_power_of_two(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

static inline uint32_t log2u(uint32_t v)
{
    uint32_t r = 0;
    while (v > 1) {
        v >>= 1;
        r++;
    }
    return r;
}

static uint32_t btb_index(const CaeBtb *btb, uint64_t pc)
{
    /*
     * Strip the 2 low bits (RV compressed / standard both drop bit 0;
     * we also drop bit 1 so compressed branches at odd hword boundaries
     * don't collide with adjacent 4-byte branches).
     */
    uint64_t tag = pc >> 2;
    return (uint32_t)(tag & (btb->num_sets - 1));
}

static uint64_t btb_tag(uint64_t pc)
{
    return pc >> 2;
}

bool cae_btb_init(CaeBtb *btb, uint32_t num_entries, uint32_t assoc,
                  Error **errp)
{
    if (!is_power_of_two(num_entries)) {
        error_setg(errp, "BTB num-entries must be power of two, got %u",
                   num_entries);
        return false;
    }
    if (assoc == 0 || assoc > num_entries) {
        error_setg(errp, "BTB assoc must be non-zero and <= num-entries (%u)",
                   num_entries);
        return false;
    }
    if (num_entries % assoc != 0) {
        error_setg(errp, "BTB num-entries (%u) must divide evenly by assoc (%u)",
                   num_entries, assoc);
        return false;
    }
    btb->num_entries = num_entries;
    btb->assoc = assoc;
    btb->num_sets = num_entries / assoc;
    if (!is_power_of_two(btb->num_sets)) {
        error_setg(errp, "BTB num-sets derived as %u (must be power of two)",
                   btb->num_sets);
        return false;
    }
    btb->index_bits = log2u(btb->num_sets);
    btb->tags = g_new0(uint64_t, num_entries);
    btb->targets = g_new0(uint64_t, num_entries);
    btb->valid = g_new0(bool, num_entries);
    btb->lru = g_new0(uint16_t, num_entries);
    return true;
}

void cae_btb_release(CaeBtb *btb)
{
    if (!btb) {
        return;
    }
    g_free(btb->tags);
    g_free(btb->targets);
    g_free(btb->valid);
    g_free(btb->lru);
    btb->tags = NULL;
    btb->targets = NULL;
    btb->valid = NULL;
    btb->lru = NULL;
}

void cae_btb_reset(CaeBtb *btb)
{
    if (!btb) {
        return;
    }
    memset(btb->valid, 0, sizeof(bool) * btb->num_entries);
    memset(btb->tags, 0, sizeof(uint64_t) * btb->num_entries);
    memset(btb->targets, 0, sizeof(uint64_t) * btb->num_entries);
    memset(btb->lru, 0, sizeof(uint16_t) * btb->num_entries);
    btb->hits = 0;
    btb->misses = 0;
}

static void btb_touch(CaeBtb *btb, uint32_t set, uint32_t way)
{
    /* MRU at 0, LRU at assoc-1. */
    uint16_t *order = &btb->lru[set * btb->assoc];
    uint16_t hit_way = (uint16_t)way;
    uint32_t i;

    for (i = 0; i < btb->assoc; i++) {
        if (order[i] == hit_way) {
            memmove(&order[1], &order[0], i * sizeof(uint16_t));
            order[0] = hit_way;
            return;
        }
    }
    /*
     * First-time touch: shift all back and insert as MRU. This matches
     * the cold-fill path invoked by cae_btb_insert().
     */
    memmove(&order[1], &order[0], (btb->assoc - 1) * sizeof(uint16_t));
    order[0] = hit_way;
}

bool cae_btb_lookup(CaeBtb *btb, uint64_t pc, uint64_t *target_out)
{
    uint32_t set;
    uint32_t way;
    uint32_t base;
    uint64_t tag;

    if (!btb || !btb->valid) {
        return false;
    }
    set = btb_index(btb, pc);
    tag = btb_tag(pc);
    base = set * btb->assoc;
    for (way = 0; way < btb->assoc; way++) {
        if (btb->valid[base + way] && btb->tags[base + way] == tag) {
            if (target_out) {
                *target_out = btb->targets[base + way];
            }
            btb_touch(btb, set, way);
            btb->hits++;
            return true;
        }
    }
    btb->misses++;
    return false;
}

void cae_btb_insert(CaeBtb *btb, uint64_t pc, uint64_t target)
{
    uint32_t set;
    uint32_t way;
    uint32_t base;
    uint64_t tag;
    uint16_t victim_way;

    if (!btb || !btb->valid) {
        return;
    }
    set = btb_index(btb, pc);
    tag = btb_tag(pc);
    base = set * btb->assoc;

    /* If already present, update target and mark MRU. */
    for (way = 0; way < btb->assoc; way++) {
        if (btb->valid[base + way] && btb->tags[base + way] == tag) {
            btb->targets[base + way] = target;
            btb_touch(btb, set, way);
            return;
        }
    }
    /* Prefer invalid slots before evicting LRU. */
    for (way = 0; way < btb->assoc; way++) {
        if (!btb->valid[base + way]) {
            btb->valid[base + way] = true;
            btb->tags[base + way] = tag;
            btb->targets[base + way] = target;
            btb_touch(btb, set, way);
            return;
        }
    }
    /* All valid: evict LRU (tail of lru_order). */
    victim_way = btb->lru[base + btb->assoc - 1];
    btb->tags[base + victim_way] = tag;
    btb->targets[base + victim_way] = target;
    btb_touch(btb, set, victim_way);
}

void cae_btb_invalidate(CaeBtb *btb, uint64_t pc)
{
    uint32_t set;
    uint32_t way;
    uint32_t base;
    uint64_t tag;

    if (!btb || !btb->valid) {
        return;
    }
    set = btb_index(btb, pc);
    tag = btb_tag(pc);
    base = set * btb->assoc;
    for (way = 0; way < btb->assoc; way++) {
        if (btb->valid[base + way] && btb->tags[base + way] == tag) {
            btb->valid[base + way] = false;
            return;
        }
    }
}
