/*
 * CAE Out-of-Order ordering violation + load-miss replay.
 *
 * Round 48 (AC-K-5): RARQ (read-after-read) and RAWQ
 * (read-after-write) trackers. Every load / store retire records a
 * (seq, addr, size) entry in the corresponding ring; a new load
 * is checked against the RAWQ for overlap with a live prior
 * store. An overlap marks the load for replay via the dedicated
 * single-slot replay payload which the scheduler consumes on the
 * next issue cycle.
 *
 * RARQ is bookkeeping only in this implementation: RVWMO allows
 * load-load reorders when no intervening fence exists, so the
 * counter is exposed as `rar_reorders` for observability (never
 * drives a stall). Round-49+ calibration may promote RARQ into
 * active-stall semantics on fence-heavy workloads.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "cae/ooo.h"

void cae_ooo_violation_init(CaeOooViolation *v)
{
    memset(v, 0, sizeof(*v));
    v->seq_next = 1u;
}

void cae_ooo_violation_reset(CaeOooViolation *v)
{
    memset(v, 0, sizeof(*v));
    v->seq_next = 1u;
}

static void viol_push_ring(CaeOooViolEntry *ring, uint32_t capacity,
                           uint32_t *head, uint32_t *tail,
                           uint32_t *count, uint64_t *drops,
                           uint64_t seq, uint64_t addr, uint16_t size)
{
    if (*count >= capacity) {
        /* Drop the oldest — the head entry — and advance. */
        ring[*head].valid = 0u;
        *head = (*head + 1u) % capacity;
        (*count)--;
        (*drops)++;
    }
    CaeOooViolEntry *e = &ring[*tail];
    e->seq = seq;
    e->addr = addr;
    e->size = size;
    e->valid = 1u;
    e->reserved = 0u;
    *tail = (*tail + 1u) % capacity;
    (*count)++;
}

void cae_ooo_violation_record_load(CaeOooViolation *v, uint64_t addr,
                                   uint16_t size)
{
    uint64_t seq = v->seq_next++;

    /*
     * RARQ: if any prior valid load overlaps this one, count a
     * reorder. Overlap is two-range-intersect.
     */
    for (uint32_t i = 0u; i < CAE_OOO_VIOL_RARQ_CAPACITY; i++) {
        CaeOooViolEntry *e = &v->rarq[i];
        if (!e->valid) {
            continue;
        }
        uint64_t a_end = addr + (size ? size : 1u);
        uint64_t b_end = e->addr + (e->size ? e->size : 1u);
        if (addr < b_end && e->addr < a_end) {
            v->rar_reorders++;
            break;
        }
    }

    viol_push_ring(v->rarq, CAE_OOO_VIOL_RARQ_CAPACITY,
                   &v->rarq_head, &v->rarq_tail, &v->rarq_count,
                   &v->drops, seq, addr, size);
    v->loads_observed++;
}

void cae_ooo_violation_record_store(CaeOooViolation *v, uint64_t addr,
                                    uint16_t size)
{
    uint64_t seq = v->seq_next++;
    viol_push_ring(v->rawq, CAE_OOO_VIOL_RAWQ_CAPACITY,
                   &v->rawq_head, &v->rawq_tail, &v->rawq_count,
                   &v->drops, seq, addr, size);
    v->stores_observed++;
}

bool cae_ooo_violation_check_raw(CaeOooViolation *v, uint64_t addr,
                                 uint16_t size)
{
    uint64_t a_end = addr + (size ? size : 1u);
    for (uint32_t i = 0u; i < CAE_OOO_VIOL_RAWQ_CAPACITY; i++) {
        CaeOooViolEntry *e = &v->rawq[i];
        if (!e->valid) {
            continue;
        }
        uint64_t b_end = e->addr + (e->size ? e->size : 1u);
        if (addr < b_end && e->addr < a_end) {
            v->raw_violations++;
            v->replay.valid = 1u;
            v->replay.seq = v->seq_next++;
            v->replay.addr = addr;
            v->replay.size = size;
            v->replay.reserved = 0u;
            return true;
        }
    }
    return false;
}

bool cae_ooo_violation_consume_replay(CaeOooViolation *v,
                                      CaeOooViolReplaySlot *out)
{
    if (!v->replay.valid) {
        return false;
    }
    if (out) {
        *out = v->replay;
    }
    v->replay.valid = 0u;
    v->replay_consumed++;
    return true;
}
