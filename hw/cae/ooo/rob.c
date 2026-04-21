/*
 * CAE Out-of-Order ROB (Reorder Buffer).
 *
 * Fixed-size circular buffer of CaeOooEntry. Dispatch inserts at
 * tail; commit drains from head subject to each entry's
 * ready_cycle ≤ now_cycle and the caller's commit_width. Flush
 * (branch squash) resets count to zero so the OoO core can resume
 * from a checkpoint in M4' (t-checkpoint-h / t-checkpoint-c).
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "cae/ooo.h"

void cae_ooo_rob_init(CaeOooRob *rob, uint32_t size)
{
    rob->size = size ? size : CAE_OOO_DEFAULT_ROB_SIZE;
    rob->slots = g_new0(CaeOooEntry, rob->size);
    rob->head = 0;
    rob->tail = 0;
    rob->count = 0;
}

void cae_ooo_rob_destroy(CaeOooRob *rob)
{
    g_free(rob->slots);
    rob->slots = NULL;
    rob->size = 0;
    rob->head = 0;
    rob->tail = 0;
    rob->count = 0;
}

bool cae_ooo_rob_has_slot(const CaeOooRob *rob)
{
    return rob->count < rob->size;
}

bool cae_ooo_rob_dispatch_ex(CaeOooRob *rob, const CaeUop *uop,
                             uint32_t dispatch_cycle,
                             uint64_t sbuffer_sqn,
                             const CaeOooDispatchHandles *handles)
{
    CaeOooEntry *slot;

    if (rob->count >= rob->size) {
        return false;
    }
    slot = &rob->slots[rob->tail];
    slot->pc = uop->pc;
    slot->fu_type = (uint8_t)uop->fu_type;
    slot->is_branch = uop->is_branch ? 1u : 0u;
    slot->is_mem = (uop->is_load || uop->is_store) ? 1u : 0u;
    slot->is_load = uop->is_load ? 1u : 0u;
    slot->is_store = uop->is_store ? 1u : 0u;
    /*
     * BS-30 round 12: record the dst-reg class counts so the
     * commit walk can release RAT slots per entry instead of
     * mirroring the current dispatch's uop. The OoO oracle
     * currently distinguishes only int vs fp classes; extended
     * classes (vector, CSR) are M5' work.
     */
    slot->num_dst_int = 0u;
    slot->num_dst_float = 0u;
    if (uop->num_dst > 0) {
        if (uop->fu_type == CAE_FU_FPU) {
            slot->num_dst_float = (uint8_t)uop->num_dst;
        } else {
            slot->num_dst_int = (uint8_t)uop->num_dst;
        }
    }
    /* Round 48 concrete handles. */
    if (handles) {
        slot->dst_arch_int  = handles->dst_arch_int;
        slot->dst_arch_fp   = handles->dst_arch_fp;
        slot->new_phys_int  = handles->new_phys_int;
        slot->new_phys_fp   = handles->new_phys_fp;
        slot->prev_phys_int = handles->prev_phys_int;
        slot->prev_phys_fp  = handles->prev_phys_fp;
        slot->lq_handle     = handles->lq_handle;
        slot->sq_handle     = handles->sq_handle;
    } else {
        slot->dst_arch_int  = CAE_OOO_INVALID_REG;
        slot->dst_arch_fp   = CAE_OOO_INVALID_REG;
        slot->new_phys_int  = 0u;
        slot->new_phys_fp   = 0u;
        slot->prev_phys_int = 0u;
        slot->prev_phys_fp  = 0u;
        slot->lq_handle     = CAE_OOO_INVALID_HANDLE;
        slot->sq_handle     = CAE_OOO_INVALID_HANDLE;
    }
    slot->valid = 1u;
    slot->issue_cycle = dispatch_cycle;
    /*
     * Round-2 OoO charge assumes each retired uop's latency is the
     * classifier's per-type default — MUL=3, DIV=20, FPU=4, others
     * 1. The full per-FU port contention model lands in M5'
     * (t-scheduler-kmhv3); until then we stamp a conservative
     * ready_cycle = dispatch + default_latency so the commit loop
     * still advances deterministically.
     */
    slot->ready_cycle = dispatch_cycle + cae_uop_default_latency(uop->type);
    slot->sbuffer_sqn = sbuffer_sqn;
    rob->tail = (rob->tail + 1) % rob->size;
    rob->count++;
    return true;
}

bool cae_ooo_rob_dispatch(CaeOooRob *rob, const CaeUop *uop,
                          uint32_t dispatch_cycle,
                          uint64_t sbuffer_sqn)
{
    return cae_ooo_rob_dispatch_ex(rob, uop, dispatch_cycle,
                                   sbuffer_sqn, NULL);
}

bool cae_ooo_rob_commit_one(CaeOooRob *rob, uint32_t now_cycle,
                            CaeOooEntry *out)
{
    if (rob->count == 0u) {
        return false;
    }
    CaeOooEntry *slot = &rob->slots[rob->head];
    if (!slot->valid || slot->ready_cycle > now_cycle) {
        return false;
    }
    if (out) {
        *out = *slot;
    }
    slot->valid = 0u;
    rob->head = (rob->head + 1) % rob->size;
    rob->count--;
    return true;
}

uint32_t cae_ooo_rob_try_commit(CaeOooRob *rob, uint32_t now_cycle,
                                uint32_t commit_width)
{
    uint32_t committed = 0;
    while (committed < commit_width
           && cae_ooo_rob_commit_one(rob, now_cycle, NULL)) {
        committed++;
    }
    return committed;
}

void cae_ooo_rob_flush_after(CaeOooRob *rob)
{
    rob->head = 0;
    rob->tail = 0;
    rob->count = 0;
    if (rob->slots) {
        memset(rob->slots, 0, sizeof(CaeOooEntry) * rob->size);
    }
}

/* ------------------------------------------------------------------ */
/*  Speculation save/restore                                          */
/* ------------------------------------------------------------------ */

struct CaeOooRobSpecSnapshot {
    uint32_t size;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    CaeOooEntry *slots;  /* owned, length == size */
};

CaeOooRobSpecSnapshot *cae_ooo_rob_spec_snapshot_save(const CaeOooRob *src)
{
    if (src == NULL || src->slots == NULL || src->size == 0u) {
        return NULL;
    }
    CaeOooRobSpecSnapshot *snap = g_new0(CaeOooRobSpecSnapshot, 1);
    snap->size = src->size;
    snap->head = src->head;
    snap->tail = src->tail;
    snap->count = src->count;
    snap->slots = g_new0(CaeOooEntry, src->size);
    memcpy(snap->slots, src->slots, sizeof(CaeOooEntry) * src->size);
    return snap;
}

void cae_ooo_rob_spec_snapshot_restore(CaeOooRob *dst,
                                       const CaeOooRobSpecSnapshot *snap)
{
    if (dst == NULL || snap == NULL || dst->slots == NULL) {
        return;
    }
    /*
     * Within-run contract: rob->size is set once at init and never
     * changes. A size mismatch would mean the caller is restoring
     * across cpu-model teardown / rebuild, which the speculation
     * path never does. Fail-silent preserves NULL-safety at the
     * composer boundary.
     */
    if (dst->size != snap->size) {
        return;
    }
    dst->head = snap->head;
    dst->tail = snap->tail;
    dst->count = snap->count;
    memcpy(dst->slots, snap->slots, sizeof(CaeOooEntry) * snap->size);
}

void cae_ooo_rob_spec_snapshot_drop(CaeOooRobSpecSnapshot *snap)
{
    if (snap == NULL) {
        return;
    }
    g_free(snap->slots);
    g_free(snap);
}
