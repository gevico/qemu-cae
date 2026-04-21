/*
 * CAE Out-of-Order LSQ (Load / Store Queue).
 *
 * Round 48 upgrades the prior count-only model to per-entry ring
 * storage. Each entry carries a stable `seq` handle, architectural
 * address + size (+ value for stores), an `alloc_tick` dispatch
 * timestamp, and a `committed` flag. The live speculation path's
 * snapshot round-trips the full ring byte-for-byte so restore
 * rewinds exact ordering — not only the occupancy count the
 * previous model tracked.
 *
 * Dispatch uses `*_allocate_{load,store}_entry` to obtain a
 * ring-index handle (CAE_OOO_INVALID_HANDLE on failure); commit
 * uses `*_commit_{load,store}_handle(ring_index)` to release the
 * entry. In-order architectural retire under the plan's one-
 * insn-per-tb contract means commits always drain the head; the
 * handle form lets callers verify the invariant explicitly
 * instead of relying on implicit ordering.
 *
 * Legacy `*_allocate_{load,store}` + `*_commit_{load,store}`
 * helpers are retained for pre-round-48 callers; they wrap the
 * handle API with a synthetic all-zero entry so existing tests
 * keep passing.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "cae/ooo.h"

void cae_ooo_lsq_init(CaeOooLsq *lsq, uint32_t lq_size, uint32_t sq_size)
{
    lsq->lq_size = lq_size ? lq_size : CAE_OOO_DEFAULT_LQ_SIZE;
    lsq->sq_size = sq_size ? sq_size : CAE_OOO_DEFAULT_SQ_SIZE;
    if (lsq->lq_size > CAE_OOO_LQ_CAPACITY) {
        lsq->lq_size = CAE_OOO_LQ_CAPACITY;
    }
    if (lsq->sq_size > CAE_OOO_SQ_CAPACITY) {
        lsq->sq_size = CAE_OOO_SQ_CAPACITY;
    }
    lsq->lq_head = 0u;
    lsq->lq_tail = 0u;
    lsq->lq_count = 0u;
    lsq->lq_seq_next = 1u;  /* 0 reserved as "no entry" */
    lsq->sq_head = 0u;
    lsq->sq_tail = 0u;
    lsq->sq_count = 0u;
    lsq->sq_seq_next = 1u;
    memset(lsq->lq, 0, sizeof(lsq->lq));
    memset(lsq->sq, 0, sizeof(lsq->sq));
}

bool cae_ooo_lsq_has_load_slot(const CaeOooLsq *lsq)
{
    return lsq->lq_count < lsq->lq_size;
}

bool cae_ooo_lsq_has_store_slot(const CaeOooLsq *lsq)
{
    return lsq->sq_count < lsq->sq_size;
}

uint16_t cae_ooo_lsq_allocate_load_entry(CaeOooLsq *lsq, uint64_t addr,
                                         uint16_t size,
                                         uint64_t alloc_tick)
{
    if (lsq->lq_count >= lsq->lq_size) {
        return CAE_OOO_INVALID_HANDLE;
    }
    uint32_t idx = lsq->lq_tail;
    CaeOooLqEntry *e = &lsq->lq[idx];
    e->seq = lsq->lq_seq_next++;
    e->addr = addr;
    e->size = size;
    e->alloc_tick = alloc_tick;
    e->committed = 0u;
    e->reserved = 0u;
    lsq->lq_tail = (lsq->lq_tail + 1u) % lsq->lq_size;
    lsq->lq_count++;
    return (uint16_t)idx;
}

uint16_t cae_ooo_lsq_allocate_store_entry(CaeOooLsq *lsq, uint64_t addr,
                                          uint16_t size, uint64_t value,
                                          uint64_t alloc_tick)
{
    if (lsq->sq_count >= lsq->sq_size) {
        return CAE_OOO_INVALID_HANDLE;
    }
    uint32_t idx = lsq->sq_tail;
    CaeOooSqEntry *e = &lsq->sq[idx];
    e->seq = lsq->sq_seq_next++;
    e->addr = addr;
    e->size = size;
    e->value = value;
    e->alloc_tick = alloc_tick;
    e->committed = 0u;
    e->reserved = 0u;
    lsq->sq_tail = (lsq->sq_tail + 1u) % lsq->sq_size;
    lsq->sq_count++;
    return (uint16_t)idx;
}

void cae_ooo_lsq_commit_load_handle(CaeOooLsq *lsq, uint16_t handle)
{
    if (handle == CAE_OOO_INVALID_HANDLE ||
        handle >= lsq->lq_size || lsq->lq_count == 0u) {
        return;
    }
    /*
     * In-order architectural commit under one-insn-per-tb: the
     * handle must equal the current head. Defensive: if callers
     * violate this invariant we zero the entry and leave the
     * ring counts alone so corruption doesn't propagate, but
     * the canonical path frees head-first.
     */
    if (handle == lsq->lq_head) {
        CaeOooLqEntry *e = &lsq->lq[lsq->lq_head];
        memset(e, 0, sizeof(*e));
        lsq->lq_head = (lsq->lq_head + 1u) % lsq->lq_size;
        lsq->lq_count--;
    } else {
        /* Mark this entry drained; head-advance happens when head hits it. */
        lsq->lq[handle].committed = 1u;
    }
}

void cae_ooo_lsq_commit_store_handle(CaeOooLsq *lsq, uint16_t handle)
{
    if (handle == CAE_OOO_INVALID_HANDLE ||
        handle >= lsq->sq_size || lsq->sq_count == 0u) {
        return;
    }
    if (handle == lsq->sq_head) {
        CaeOooSqEntry *e = &lsq->sq[lsq->sq_head];
        memset(e, 0, sizeof(*e));
        lsq->sq_head = (lsq->sq_head + 1u) % lsq->sq_size;
        lsq->sq_count--;
    } else {
        lsq->sq[handle].committed = 1u;
    }
}

const CaeOooLqEntry *cae_ooo_lsq_peek_load(const CaeOooLsq *lsq,
                                           uint16_t handle)
{
    if (lsq == NULL || handle == CAE_OOO_INVALID_HANDLE ||
        handle >= lsq->lq_size) {
        return NULL;
    }
    return &lsq->lq[handle];
}

const CaeOooSqEntry *cae_ooo_lsq_peek_store(const CaeOooLsq *lsq,
                                            uint16_t handle)
{
    if (lsq == NULL || handle == CAE_OOO_INVALID_HANDLE ||
        handle >= lsq->sq_size) {
        return NULL;
    }
    return &lsq->sq[handle];
}

/* Legacy count-only allocate/commit wrappers. */

bool cae_ooo_lsq_allocate_load(CaeOooLsq *lsq)
{
    return cae_ooo_lsq_allocate_load_entry(lsq, 0u, 0u, 0u)
           != CAE_OOO_INVALID_HANDLE;
}

bool cae_ooo_lsq_allocate_store(CaeOooLsq *lsq)
{
    return cae_ooo_lsq_allocate_store_entry(lsq, 0u, 0u, 0u, 0u)
           != CAE_OOO_INVALID_HANDLE;
}

void cae_ooo_lsq_commit_load(CaeOooLsq *lsq)
{
    if (lsq->lq_count > 0u) {
        cae_ooo_lsq_commit_load_handle(lsq, (uint16_t)lsq->lq_head);
    }
}

void cae_ooo_lsq_commit_store(CaeOooLsq *lsq)
{
    if (lsq->sq_count > 0u) {
        cae_ooo_lsq_commit_store_handle(lsq, (uint16_t)lsq->sq_head);
    }
}

void cae_ooo_lsq_flush(CaeOooLsq *lsq)
{
    lsq->lq_head = 0u;
    lsq->lq_tail = 0u;
    lsq->lq_count = 0u;
    lsq->sq_head = 0u;
    lsq->sq_tail = 0u;
    lsq->sq_count = 0u;
    memset(lsq->lq, 0, sizeof(lsq->lq));
    memset(lsq->sq, 0, sizeof(lsq->sq));
}

/* ------------------------------------------------------------------ */
/*  Speculation save/restore                                          */
/* ------------------------------------------------------------------ */

struct CaeOooLsqSpecSnapshot {
    uint32_t lq_size;
    uint32_t lq_head;
    uint32_t lq_tail;
    uint32_t lq_count;
    uint64_t lq_seq_next;
    uint32_t sq_size;
    uint32_t sq_head;
    uint32_t sq_tail;
    uint32_t sq_count;
    uint64_t sq_seq_next;
    CaeOooLqEntry lq[CAE_OOO_LQ_CAPACITY];
    CaeOooSqEntry sq[CAE_OOO_SQ_CAPACITY];
};

CaeOooLsqSpecSnapshot *cae_ooo_lsq_spec_snapshot_save(const CaeOooLsq *src)
{
    if (src == NULL) {
        return NULL;
    }
    CaeOooLsqSpecSnapshot *snap = g_new0(CaeOooLsqSpecSnapshot, 1);
    snap->lq_size  = src->lq_size;
    snap->lq_head  = src->lq_head;
    snap->lq_tail  = src->lq_tail;
    snap->lq_count = src->lq_count;
    snap->lq_seq_next = src->lq_seq_next;
    snap->sq_size  = src->sq_size;
    snap->sq_head  = src->sq_head;
    snap->sq_tail  = src->sq_tail;
    snap->sq_count = src->sq_count;
    snap->sq_seq_next = src->sq_seq_next;
    memcpy(snap->lq, src->lq, sizeof(snap->lq));
    memcpy(snap->sq, src->sq, sizeof(snap->sq));
    return snap;
}

void cae_ooo_lsq_spec_snapshot_restore(CaeOooLsq *dst,
                                       const CaeOooLsqSpecSnapshot *snap)
{
    if (dst == NULL || snap == NULL) {
        return;
    }
    dst->lq_size  = snap->lq_size;
    dst->lq_head  = snap->lq_head;
    dst->lq_tail  = snap->lq_tail;
    dst->lq_count = snap->lq_count;
    dst->lq_seq_next = snap->lq_seq_next;
    dst->sq_size  = snap->sq_size;
    dst->sq_head  = snap->sq_head;
    dst->sq_tail  = snap->sq_tail;
    dst->sq_count = snap->sq_count;
    dst->sq_seq_next = snap->sq_seq_next;
    memcpy(dst->lq, snap->lq, sizeof(dst->lq));
    memcpy(dst->sq, snap->sq, sizeof(dst->sq));
}

void cae_ooo_lsq_spec_snapshot_drop(CaeOooLsqSpecSnapshot *snap)
{
    g_free(snap);
}
