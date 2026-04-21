/*
 * CAE OoO CPU timing model — public entry points.
 *
 * Most of the CaeCpuOoo interface is internal to hw/cae/cpu_ooo.c
 * (the QOM struct is private, the charge callback is accessed
 * via CaeCpuModelClass). This header exposes the couple of
 * symbols that cross the module boundary:
 *
 *   - TYPE_CAE_CPU_OOO: the QOM type name (also referenced by
 *     accel/cae/cae-all.c when constructing the model from the
 *     `cpu-model=ooo-kmhv3` accel knob).
 *   - cae_cpu_ooo_squash_after(): the wrong-path squash entry
 *     point. Called by the M4' TCG predicted-path squash handler
 *     (t-tcg-spec-path) once it lands, and exercised directly by
 *     the round-12 unit test `squash-discards-younger-stores`.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_CPU_OOO_H
#define CAE_CPU_OOO_H

#include <stdint.h>
#include "qom/object.h"

#define TYPE_CAE_CPU_OOO "cae-cpu-ooo"

/*
 * Squash every in-flight speculative state whose sbuffer sqn is
 * >= squash_sqn. Also flushes the ROB / IQ / LSQ / RAT so the
 * core resumes from a clean checkpoint. The returned count of
 * discarded sbuffer entries is added to the `sbuffer-squashes`
 * QOM read-only counter.
 *
 * Callers:
 *   - M4' TCG predicted-path squash handler (t-tcg-spec-path).
 *   - Unit test /cae/cpu-ooo/squash-discards-younger-stores.
 */
void cae_cpu_ooo_squash_after(Object *obj, uint64_t squash_sqn);

/* ------------------------------------------------------------------ */
/*  Speculation save/restore: OoO scalar lane                         */
/* ------------------------------------------------------------------ */

/*
 * Arch-neutral snapshot of the full CaeCpuOoo state that the M4'
 * speculation code path needs to unwind on a mispredict. As of
 * round 30 this is NOT just a scalar lane — the composer in
 * hw/cae/cpu_ooo.c composes five owning-module sub-blobs on top
 * of the scalar lane:
 *   - scalar lane: now_cycle, store_sqn_next
 *   - ROB sub-blob: slots[] + head/tail/count/size
 *     (cae_ooo_rob_spec_snapshot_* in hw/cae/ooo/rob.c)
 *   - IQ sub-blob: size/count/issue_width
 *     (cae_ooo_iq_spec_snapshot_* in hw/cae/ooo/iq.c)
 *   - LSQ sub-blob: lq_size/lq_count/sq_size/sq_count
 *     (cae_ooo_lsq_spec_snapshot_* in hw/cae/ooo/lsq.c)
 *   - RAT sub-blob: num_phys_int_regs + num_phys_float_regs +
 *     int_inflight + fp_inflight (cae_ooo_rat_spec_snapshot_*
 *     in hw/cae/ooo/rat.c)
 *   - sbuffer sub-blob: entries[] + head/tail/occupancy + stats
 *     (cae_sbuffer_spec_snapshot_* in hw/cae/ooo/sbuffer.c)
 *
 * Each sub-blob's struct definition lives in its owning .c file
 * per the round-28/29/30 owning-module pattern; this composer
 * treats them as opaque pointers. Restore order inside this
 * blob: scalars first, then each sub-blob in save order.
 *
 * Save returns NULL on `obj == NULL` or when `obj` is not a
 * cae-cpu-ooo instance (safe no-op for in-order / cpi1
 * configurations). Restore is a no-op on NULL snap or NULL obj.
 * Drop cascades to each sub-blob's drop before freeing the
 * outer struct.
 */
typedef struct CaeOooSpecSnapshot CaeOooSpecSnapshot;

CaeOooSpecSnapshot *cae_cpu_ooo_spec_snapshot_save(Object *obj);
void cae_cpu_ooo_spec_snapshot_restore(Object *obj,
                                       const CaeOooSpecSnapshot *snap);
void cae_cpu_ooo_spec_snapshot_drop(CaeOooSpecSnapshot *snap);

/*
 * Test-only seed helper. Writes `now_cycle` and `store_sqn_next`
 * directly so a unit regression can mutate the saved scalars
 * before calling save/restore. Non-matching Object * is a safe
 * no-op. Production callers (the charge path, the squash entry
 * point) never touch these scalars through this helper — it
 * exists solely so the /cae/checkpoint/ooo-scalar-roundtrip
 * regression can prove a real restore against a real mutation,
 * rather than against a no-op nudge.
 */
void cae_cpu_ooo_spec_test_seed(Object *obj, uint64_t now_cycle,
                                uint64_t store_sqn_next);

/*
 * Test-only seed seam for the tick-driver watermark. Pairs with
 * the snapshot-classified `sbuffer_commit_sqn` field so the
 * tick-driver checkpoint regression can inject a non-zero
 * save-time value. NULL or wrong-type object is a safe no-op.
 * Production callers advance this watermark through the commit
 * loop; they must not call this seam.
 */
void cae_cpu_ooo_test_seed_sbuffer_commit_sqn(Object *obj, uint64_t value);

/*
 * Test-only observer for the tick-driver watermark. Returns 0 on
 * NULL or wrong-type object. Lets the checkpoint regression
 * assert save/restore without reaching into the private
 * CaeCpuOoo layout.
 */
uint64_t cae_cpu_ooo_sbuffer_commit_sqn(const Object *obj);

/*
 * Test-only seed seam for the cpu-model-level
 * `sbuffer_eviction_events` lifetime counter. Pairs with the
 * sbuffer-side lifetime-counter seam so the checkpoint
 * regression can seed/mutate the full lifetime-only set across
 * a save/restore round-trip. NULL or wrong-type object is a
 * safe no-op; production callers advance this counter through
 * the charge path's tick result.
 */
void cae_cpu_ooo_test_seed_sbuffer_eviction_events(Object *obj,
                                                   uint64_t value);

/*
 * Current `store_sqn_next` scalar. The live speculation lane
 * (t-tcg-spec-path) uses this at save-time to record the squash
 * boundary — the sqn value passed later to
 * `cae_cpu_ooo_squash_after()` on a mispredict. Returns 0 on NULL
 * obj or non-OoO cpu-model (in-order / cpi1): the caller's squash
 * call is then a no-op, which is the correct degradation for
 * those configurations.
 */
uint64_t cae_cpu_ooo_current_store_sqn(Object *obj);

/*
 * Test-only probe: current ROB occupancy (`rob.count`). Returns 0
 * on NULL obj or non-OoO cpu-model. Exists so the round-32
 * regression `/cae/checkpoint/live-preserves-restored-ooo-
 * containers` can observe ROB state without reaching into the
 * private CaeCpuOoo struct. Production callers should not depend
 * on this; the ROB is owned by hw/cae/cpu_ooo.c and its commit /
 * flush semantics run through the in-module retire loop.
 */
uint32_t cae_cpu_ooo_rob_count(Object *obj);

/*
 * Test-only seed seam: dispatches a single ALU uop at `pc` into
 * the embedded ROB. Returns true on success, false on NULL obj /
 * non-OoO cpu-model / ROB full. Exists for round-33 engine-path
 * regressions that need to populate the CaeCpuOoo's composed
 * ROB occupancy before the live save/restore path runs so the
 * save-time NON-ZERO occupancy can be observed to survive the
 * round-31/32 Bug #2 restore+squash sequence. Production
 * callers do not use this — dispatch happens through the retire
 * loop in cae_cpu_ooo_charge.
 */
bool cae_cpu_ooo_test_seed_rob_entry(Object *obj, uint64_t pc);

/*
 * Test-only probes for the RAT inflight-state fields. These are
 * the observables the round-37 restore-sensitive engine-path
 * regression uses to prove `cae_checkpoint_live_restore()`
 * actually composes through the RAT sub-blob, not just the
 * counter-surface bookkeeping that the engine runs regardless.
 * Return 0 on a NULL or wrong-type object.
 */
uint32_t cae_cpu_ooo_rat_int_inflight(Object *obj);
uint32_t cae_cpu_ooo_rat_fp_inflight(Object *obj);

/*
 * Test-only seed seam: direct-write the RAT inflight counts.
 * Mirrors `cae_cpu_ooo_test_seed_rob_entry` in spirit — unit
 * tests need to force the RAT into specific states to assert
 * restore-sensitive invariants. Production callers dispatch
 * through the retire loop and must never use this. Returns false
 * on a NULL or wrong-type object.
 */
/*
 * Round 49 AC-K-4: concrete rename-map test seam.
 * `cae_cpu_ooo_test_alloc_rat_int` allocates a phys id for
 * arch_dst through the production allocator and returns the
 * new phys id (0 on failure or arch_dst==0 no-op). prev_phys_out
 * receives the phys id mapped BEFORE the allocation. The
 * engine-live-path-restores-rat-map regression uses these to
 * mutate the map before/after a live snapshot and assert exact
 * architectural-register bindings survive the restore.
 *
 * `cae_cpu_ooo_rat_map_int_at` / `_int_free_count` are
 * observers: return 0 for non-OoO cpu-models or out-of-range
 * arch_reg (>=32).
 */
uint16_t cae_cpu_ooo_test_alloc_rat_int(Object *obj, uint8_t arch_dst,
                                        uint16_t *prev_phys_out);
uint16_t cae_cpu_ooo_rat_map_int_at(Object *obj, uint8_t arch_reg);
uint32_t cae_cpu_ooo_rat_int_free_count(Object *obj);

bool cae_cpu_ooo_test_seed_rat_inflight(Object *obj,
                                        uint32_t int_count,
                                        uint32_t fp_count);

/*
 * Round 39 directive step 4: stage a speculative store into
 * the OoO model's sbuffer at the current `store_sqn_next` and
 * bump it. Called by `cae_cpu_drain_spec_stimuli` when a queued
 * stimulus has `op == WRITE`, so wrong-path stores land in the
 * sbuffer (where the round-32 squash_after path discards them
 * on mispredict) instead of reaching the memory backend (which
 * would violate plan.md:85 Option-X by leaving externally-
 * visible architectural state).
 *
 * Contract: the allocated sqn is strictly >= the spec-window's
 * save-time `spec_squash_sqn` snapshot (captured BEFORE any
 * wrong-path stores), so `cae_sbuffer_squash_after(spec_squash_sqn)`
 * strictly discards all speculatively-staged stores while
 * preserving any committed pre-spec-window entries.
 *
 * Returns true on successful sbuffer allocation; false when
 * `obj` is NULL, not a CaeCpuOoo, the sbuffer is absent
 * (test configurations), or the sbuffer ring is full.
 */
bool cae_cpu_ooo_sbuffer_stage_spec_store(Object *obj,
                                          uint64_t addr,
                                          uint16_t size,
                                          uint64_t value);

/*
 * Round 50 AC-K-5 test seam: enqueue `count` entries into the
 * scheduler's INT segment without going through the retire
 * loop. Used by
 * `/cae/ooo/cpu-ooo-issue-width-changes-scheduler-issued` to
 * pre-fill the scheduler so the bounded `issue_cycle` call
 * (invoked by the NEXT `cae_cpu_ooo_charge` tick) observes a
 * real backlog — without pre-fill the per-tick enqueue/issue
 * ratio stays at 1:1 and `issue_width` cannot differentiate
 * across trials. Production callers must not use this.
 *
 * Returns false on a NULL or wrong-type object.
 */
bool cae_cpu_ooo_test_scheduler_seed(Object *obj, uint32_t count,
                                     uint8_t fu_type);

#endif /* CAE_CPU_OOO_H */
