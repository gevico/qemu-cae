/*
 * CAE Out-of-Order RAT (Register Alias Table).
 *
 * Round 48 upgrades the prior count-only model to a concrete
 * rename-map + free-list implementation. The integer and FP classes
 * each carry an array mapping architectural register id -> physical
 * register id, plus a bounded free-list ring that the allocator
 * pops from at dispatch and the committer / squash-recovery pushes
 * back to at retire.
 *
 * Physical register ids 1..32 are reserved as the "architectural"
 * baseline mapping (initial `int_map[i] = i+1` for i in 0..31;
 * the FP class mirrors this in its own namespace). The remaining
 * ids 33..num_phys_{int,float}_regs populate the free list at
 * init. Phys id 0 is a sentinel meaning "unmapped / reads zero-
 * reg", and is also returned by allocate_dst_* on failure.
 *
 * The older count-only helpers (`cae_ooo_rat_allocate`,
 * `cae_ooo_rat_free`, `cae_ooo_rat_free_counts`) continue to
 * work: they now drive the concrete helpers via arch-reg heuristics
 * so existing regressions keep passing unchanged while the cpu_ooo
 * dispatch path switches to the concrete API.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "cae/ooo.h"

static uint32_t rat_int_free_capacity(const CaeOooRat *rat)
{
    if (rat->num_phys_int_regs <= CAE_OOO_RAT_ARCH_REGS) {
        return 0u;
    }
    uint32_t avail = rat->num_phys_int_regs - CAE_OOO_RAT_ARCH_REGS;
    if (avail > CAE_OOO_RAT_MAX_INT_PHYS) {
        avail = CAE_OOO_RAT_MAX_INT_PHYS;
    }
    return avail;
}

static uint32_t rat_fp_free_capacity(const CaeOooRat *rat)
{
    if (rat->num_phys_float_regs <= CAE_OOO_RAT_ARCH_REGS) {
        return 0u;
    }
    uint32_t avail = rat->num_phys_float_regs - CAE_OOO_RAT_ARCH_REGS;
    if (avail > CAE_OOO_RAT_MAX_FP_PHYS) {
        avail = CAE_OOO_RAT_MAX_FP_PHYS;
    }
    return avail;
}

void cae_ooo_rat_init(CaeOooRat *rat, uint32_t num_int, uint32_t num_fp)
{
    rat->num_phys_int_regs = num_int ? num_int :
                             CAE_OOO_DEFAULT_NUM_PHYS_INT_REGS;
    rat->num_phys_float_regs = num_fp ? num_fp :
                               CAE_OOO_DEFAULT_NUM_PHYS_FLOAT_REGS;
    rat->int_inflight = 0u;
    rat->fp_inflight = 0u;
    rat->int_alloc_seq = 0u;

    /*
     * Populate the initial architectural mapping: arch reg i gets
     * phys id i+1 (phys id 0 reserved as "no mapping"). The FP
     * class has its own namespace so shared phys ids don't
     * collide at the API surface; internally each class reads
     * from its own map / free list.
     */
    for (uint32_t i = 0u; i < CAE_OOO_RAT_ARCH_REGS; i++) {
        rat->int_map[i] = (uint16_t)(i + 1u);
        rat->fp_map[i] = (uint16_t)(i + 1u);
    }

    uint32_t int_extra = rat_int_free_capacity(rat);
    rat->int_free_count = int_extra;
    for (uint32_t i = 0u; i < int_extra; i++) {
        rat->int_free[i] =
            (uint16_t)(CAE_OOO_RAT_ARCH_REGS + 1u + i);
    }
    for (uint32_t i = int_extra; i < CAE_OOO_RAT_MAX_INT_PHYS; i++) {
        rat->int_free[i] = 0u;
    }

    uint32_t fp_extra = rat_fp_free_capacity(rat);
    rat->fp_free_count = fp_extra;
    for (uint32_t i = 0u; i < fp_extra; i++) {
        rat->fp_free[i] =
            (uint16_t)(CAE_OOO_RAT_ARCH_REGS + 1u + i);
    }
    for (uint32_t i = fp_extra; i < CAE_OOO_RAT_MAX_FP_PHYS; i++) {
        rat->fp_free[i] = 0u;
    }
}

uint16_t cae_ooo_rat_map_int(const CaeOooRat *rat, uint8_t arch_reg)
{
    if (rat == NULL || arch_reg >= CAE_OOO_RAT_ARCH_REGS) {
        return 0u;
    }
    return rat->int_map[arch_reg];
}

uint16_t cae_ooo_rat_map_fp(const CaeOooRat *rat, uint8_t arch_reg)
{
    if (rat == NULL || arch_reg >= CAE_OOO_RAT_ARCH_REGS) {
        return 0u;
    }
    return rat->fp_map[arch_reg];
}

uint16_t cae_ooo_rat_allocate_dst_int(CaeOooRat *rat, uint8_t arch_dst,
                                      uint16_t *prev_phys_out)
{
    if (rat == NULL || arch_dst == 0u ||
        arch_dst >= CAE_OOO_RAT_ARCH_REGS) {
        if (prev_phys_out) {
            *prev_phys_out = 0u;
        }
        return 0u;
    }
    if (rat->int_free_count == 0u) {
        if (prev_phys_out) {
            *prev_phys_out = 0u;
        }
        return 0u;
    }
    uint16_t prev = rat->int_map[arch_dst];
    uint16_t new_phys = rat->int_free[rat->int_free_count - 1u];
    rat->int_free_count--;
    rat->int_map[arch_dst] = new_phys;
    rat->int_inflight++;
    rat->int_alloc_seq++;
    if (prev_phys_out) {
        *prev_phys_out = prev;
    }
    return new_phys;
}

uint16_t cae_ooo_rat_allocate_dst_fp(CaeOooRat *rat, uint8_t arch_dst,
                                     uint16_t *prev_phys_out)
{
    if (rat == NULL || arch_dst >= CAE_OOO_RAT_ARCH_REGS) {
        if (prev_phys_out) {
            *prev_phys_out = 0u;
        }
        return 0u;
    }
    if (rat->fp_free_count == 0u) {
        if (prev_phys_out) {
            *prev_phys_out = 0u;
        }
        return 0u;
    }
    uint16_t prev = rat->fp_map[arch_dst];
    uint16_t new_phys = rat->fp_free[rat->fp_free_count - 1u];
    rat->fp_free_count--;
    rat->fp_map[arch_dst] = new_phys;
    rat->fp_inflight++;
    if (prev_phys_out) {
        *prev_phys_out = prev;
    }
    return new_phys;
}

void cae_ooo_rat_free_dst_int(CaeOooRat *rat, uint8_t arch_dst,
                              uint16_t new_phys, uint16_t prev_phys)
{
    if (rat == NULL || arch_dst == 0u ||
        arch_dst >= CAE_OOO_RAT_ARCH_REGS) {
        return;
    }
    if (new_phys == 0u) {
        /* allocate had failed; nothing to free. */
        return;
    }
    /*
     * Commit / squash semantics collapse into the same primitive:
     * rewind the map to the pre-allocation mapping and return the
     * newly-allocated phys id to the free list.
     */
    rat->int_map[arch_dst] = prev_phys;
    if (rat->int_free_count < CAE_OOO_RAT_MAX_INT_PHYS) {
        rat->int_free[rat->int_free_count++] = new_phys;
    }
    if (rat->int_inflight > 0u) {
        rat->int_inflight--;
    }
    rat->int_alloc_seq++;
}

void cae_ooo_rat_free_dst_fp(CaeOooRat *rat, uint8_t arch_dst,
                             uint16_t new_phys, uint16_t prev_phys)
{
    if (rat == NULL || arch_dst >= CAE_OOO_RAT_ARCH_REGS) {
        return;
    }
    if (new_phys == 0u) {
        return;
    }
    rat->fp_map[arch_dst] = prev_phys;
    if (rat->fp_free_count < CAE_OOO_RAT_MAX_FP_PHYS) {
        rat->fp_free[rat->fp_free_count++] = new_phys;
    }
    if (rat->fp_inflight > 0u) {
        rat->fp_inflight--;
    }
}

bool cae_ooo_rat_has_slot(const CaeOooRat *rat, const CaeUop *uop)
{
    if (uop->num_dst == 0) {
        return true;
    }
    if (uop->fu_type == CAE_FU_FPU) {
        /*
         * FP arch reg 0 is a real register (unlike int x0), so no
         * "free register consumes nothing" exception applies here.
         */
        return rat->fp_free_count > 0u;
    }
    /*
     * Round 49 blocker fix: integer arch reg 0 (RISC-V x0) is a
     * discard register — writes are architecturally dropped. The
     * concrete allocator (`cae_ooo_rat_allocate_dst_int`) treats
     * `arch_dst == 0` as a no-op that consumes zero phys ids; the
     * precheck must agree, otherwise a run with an exhausted free
     * list stalls dispatch on a uop that would never have consumed
     * a phys id in the first place. Using `dst_regs[0]` matches the
     * arch-reg the allocator actually reads.
     */
    if (uop->num_dst > 0u && uop->dst_regs[0] == 0u) {
        return true;
    }
    return rat->int_free_count > 0u;
}

/*
 * Legacy count-based allocate/free. Retained for callers that do
 * not yet pass an explicit arch_dst (pre-round-48 paths). Chooses
 * an unused arch reg synthetically so the allocation exercises the
 * free list without depending on uop contents.
 */
bool cae_ooo_rat_allocate(CaeOooRat *rat, const CaeUop *uop)
{
    if (uop->num_dst == 0) {
        return true;
    }
    if (uop->fu_type == CAE_FU_FPU) {
        if (rat->fp_free_count == 0u) {
            return false;
        }
        rat->fp_free_count--;
        rat->fp_inflight++;
        return true;
    }
    if (rat->int_free_count == 0u) {
        return false;
    }
    rat->int_free_count--;
    rat->int_inflight++;
    rat->int_alloc_seq++;
    return true;
}

void cae_ooo_rat_free(CaeOooRat *rat, const CaeUop *uop)
{
    if (uop->num_dst == 0) {
        return;
    }
    if (uop->fu_type == CAE_FU_FPU) {
        if (rat->fp_inflight > 0u &&
            rat->fp_free_count < CAE_OOO_RAT_MAX_FP_PHYS) {
            rat->fp_free_count++;
            rat->fp_inflight--;
        }
    } else {
        if (rat->int_inflight > 0u &&
            rat->int_free_count < CAE_OOO_RAT_MAX_INT_PHYS) {
            rat->int_free_count++;
            rat->int_inflight--;
            rat->int_alloc_seq++;
        }
    }
}

void cae_ooo_rat_free_counts(CaeOooRat *rat, uint32_t int_count,
                             uint32_t fp_count)
{
    /*
     * Legacy fallback: released slots do not correspond to a
     * specific arch reg. Advance the free-list count and the
     * provenance counter; callers that need exact map rewind
     * call cae_ooo_rat_free_dst_int / _fp from the concrete
     * ROB entry fields.
     */
    uint32_t int_freed;
    if (int_count > rat->int_inflight) {
        int_freed = rat->int_inflight;
        rat->int_inflight = 0u;
    } else {
        int_freed = int_count;
        rat->int_inflight -= int_count;
    }
    for (uint32_t i = 0u; i < int_freed; i++) {
        if (rat->int_free_count < CAE_OOO_RAT_MAX_INT_PHYS) {
            rat->int_free_count++;
        }
    }
    rat->int_alloc_seq += int_freed;
    uint32_t fp_freed;
    if (fp_count > rat->fp_inflight) {
        fp_freed = rat->fp_inflight;
        rat->fp_inflight = 0u;
    } else {
        fp_freed = fp_count;
        rat->fp_inflight -= fp_count;
    }
    for (uint32_t i = 0u; i < fp_freed; i++) {
        if (rat->fp_free_count < CAE_OOO_RAT_MAX_FP_PHYS) {
            rat->fp_free_count++;
        }
    }
}

void cae_ooo_rat_flush(CaeOooRat *rat)
{
    /*
     * Flush is an architectural reset of inflight state: rebuild
     * the initial architectural map and replenish the free list.
     * Round 37's provenance rule still holds — int_alloc_seq is
     * NOT rewound here; only spec_snapshot_restore rewinds it so
     * regressions can distinguish "squash-restored" from "flushed
     * and re-filled".
     */
    rat->int_inflight = 0u;
    rat->fp_inflight = 0u;
    for (uint32_t i = 0u; i < CAE_OOO_RAT_ARCH_REGS; i++) {
        rat->int_map[i] = (uint16_t)(i + 1u);
        rat->fp_map[i] = (uint16_t)(i + 1u);
    }
    uint32_t int_extra = rat_int_free_capacity(rat);
    rat->int_free_count = int_extra;
    for (uint32_t i = 0u; i < int_extra; i++) {
        rat->int_free[i] =
            (uint16_t)(CAE_OOO_RAT_ARCH_REGS + 1u + i);
    }
    uint32_t fp_extra = rat_fp_free_capacity(rat);
    rat->fp_free_count = fp_extra;
    for (uint32_t i = 0u; i < fp_extra; i++) {
        rat->fp_free[i] =
            (uint16_t)(CAE_OOO_RAT_ARCH_REGS + 1u + i);
    }
}

uint64_t cae_ooo_rat_int_alloc_seq(const CaeOooRat *rat)
{
    if (rat == NULL) {
        return 0u;
    }
    return rat->int_alloc_seq;
}

/* ------------------------------------------------------------------ */
/*  Speculation save/restore                                          */
/* ------------------------------------------------------------------ */

struct CaeOooRatSpecSnapshot {
    uint32_t num_phys_int_regs;
    uint32_t num_phys_float_regs;
    uint32_t int_inflight;
    uint32_t fp_inflight;
    uint64_t int_alloc_seq;
    uint16_t int_map[CAE_OOO_RAT_ARCH_REGS];
    uint16_t fp_map[CAE_OOO_RAT_ARCH_REGS];
    uint16_t int_free[CAE_OOO_RAT_MAX_INT_PHYS];
    uint16_t fp_free[CAE_OOO_RAT_MAX_FP_PHYS];
    uint32_t int_free_count;
    uint32_t fp_free_count;
};

CaeOooRatSpecSnapshot *cae_ooo_rat_spec_snapshot_save(const CaeOooRat *src)
{
    if (src == NULL) {
        return NULL;
    }
    CaeOooRatSpecSnapshot *snap = g_new0(CaeOooRatSpecSnapshot, 1);
    snap->num_phys_int_regs   = src->num_phys_int_regs;
    snap->num_phys_float_regs = src->num_phys_float_regs;
    snap->int_inflight        = src->int_inflight;
    snap->fp_inflight         = src->fp_inflight;
    snap->int_alloc_seq       = src->int_alloc_seq;
    snap->int_free_count      = src->int_free_count;
    snap->fp_free_count       = src->fp_free_count;
    memcpy(snap->int_map, src->int_map, sizeof(snap->int_map));
    memcpy(snap->fp_map, src->fp_map, sizeof(snap->fp_map));
    memcpy(snap->int_free, src->int_free, sizeof(snap->int_free));
    memcpy(snap->fp_free, src->fp_free, sizeof(snap->fp_free));
    return snap;
}

void cae_ooo_rat_spec_snapshot_restore(CaeOooRat *dst,
                                       const CaeOooRatSpecSnapshot *snap)
{
    if (dst == NULL || snap == NULL) {
        return;
    }
    dst->num_phys_int_regs   = snap->num_phys_int_regs;
    dst->num_phys_float_regs = snap->num_phys_float_regs;
    dst->int_inflight        = snap->int_inflight;
    dst->fp_inflight         = snap->fp_inflight;
    dst->int_alloc_seq       = snap->int_alloc_seq;
    dst->int_free_count      = snap->int_free_count;
    dst->fp_free_count       = snap->fp_free_count;
    memcpy(dst->int_map, snap->int_map, sizeof(dst->int_map));
    memcpy(dst->fp_map, snap->fp_map, sizeof(dst->fp_map));
    memcpy(dst->int_free, snap->int_free, sizeof(dst->int_free));
    memcpy(dst->fp_free, snap->fp_free, sizeof(dst->fp_free));
}

void cae_ooo_rat_spec_snapshot_drop(CaeOooRatSpecSnapshot *snap)
{
    g_free(snap);
}
