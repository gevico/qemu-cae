/*
 * CAE Out-of-Order scalar CPU timing model (M3' scaffold).
 *
 * Implements TYPE_CAE_CPU_OOO, parallel to TYPE_CAE_CPU_INORDER, as
 * a TYPE_CAE_CPU_MODEL-conformant variant that a CaeCpu can attach
 * via the `cpu-model=ooo-kmhv3` selector. The charge path runs
 * per retired architectural insn (CAE keeps one-insn-per-tb
 * throughout M3'-M5'; multi-insn-per-tb is out of scope) and
 * composes ROB / IQ / LSQ / RAT / cache_mshr updates to produce a
 * cycle count.
 *
 * The M3' functional oracle's goal is AC-K-3 (max IPC error ≤ 30%,
 * geomean ≤ 15% vs kmhv3.py). This file lands the scaffold and the
 * charge loop; M3' calibration happens in round 3 via
 * /humanize:ask-codex -> t-suite-fn-calibration.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "cae/cpu_model.h"
#include "cae/cpu_ooo.h"
#include "cae/bpred.h"
#include "cae/pipeline.h"
#include "cae/uop.h"
#include "cae/ooo.h"
#include "cae/sbuffer.h"

/*
 * Default capacity for the embedded speculative store buffer.
 * Matches the XS-GEM5 L1_SBUFFER_SIZE=16 knob that BS-28 round 10
 * codified in the sbuffer spec; cpu-model=ooo-kmhv3 defaults
 * therefore track the calibration target without additional
 * plumbing. The value is tunable via sb.sbuffer-size on the
 * child object if a future `paired-yaml` recipe needs a
 * different size.
 */
#define CAE_CPU_OOO_DEFAULT_SBUFFER_SIZE    16u

OBJECT_DECLARE_SIMPLE_TYPE(CaeCpuOoo, CAE_CPU_OOO)

struct CaeCpuOoo {
    Object parent;

    /* Parameters, defaults match the kmhv3.py baseline. */
    uint32_t rob_size;
    uint32_t lq_size;
    uint32_t sq_size;
    uint32_t issue_width;
    uint32_t commit_width;
    uint32_t rename_width;
    uint32_t num_phys_int_regs;
    uint32_t num_phys_float_regs;

    /* Sub-structures (allocated in complete()). */
    CaeOooRob rob;
    CaeOooIq iq;
    CaeOooLsq lsq;
    CaeOooRat rat;
    /*
     * Round 49: live-wired AC-K-5 modules. scheduler.c tracks
     * per-segment issue pressure alongside the flat IQ (the flat
     * IQ stays as a coarse allocation counter; the scheduler is
     * the KMH-V3-aligned 3-segment x 2-port issue accounting).
     * violation.c records every load / store commit and flags RAW
     * overlaps; scheduler_replays counts replay-slot consumption
     * on retire-side RAW detection, exposed as a QMP observable.
     */
    CaeOooScheduler scheduler;
    CaeOooViolation violation;

    /* Charge-path shared monotonic clock. */
    uint64_t now_cycle;

    /* Optional branch predictor. */
    Object *bpred;

    /*
     * Embedded speculative store buffer (BS-28 round 11). Stores
     * that pass the LSQ allocate check enter the sbuffer with a
     * monotonic store_sqn; stores that commit drain FIFO through
     * cae_sbuffer_drain_head and surface their payload as
     * last_committed_store_* for reviewer observability. Owned by
     * the cpu-model object so that cpu-model=ooo-kmhv3 always
     * builds a working sbuffer without additional -object wiring
     * (matches the rest of the sub-structures: rob, iq, lsq, rat).
     * sbuffer_size is tunable via the child object's
     * sbuffer-size property for the future kmhv3 calibration
     * recipe (L1_SBUFFER_SIZE variants).
     */
    Object *sbuffer;             /* CaeSbuffer instance */
    uint32_t sbuffer_size;       /* per-cpu-model sbuffer capacity */
    /*
     * Round 50 AC-K-5: sbuffer evict-threshold knob forwarded to
     * the embedded sbuffer child at construction. Zero leaves the
     * sbuffer evict tracker disabled; non-zero enables
     * evict-threshold-events reporting when the sbuffer crosses
     * the watermark. Accel-level `sbuffer-evict-threshold` knob
     * drives this so xs-1c-kmhv3.yaml can deliver a live value.
     */
    uint32_t sbuffer_evict_threshold;
    /*
     * Tick-driver thresholds forwarded to the embedded sbuffer
     * child at construction. Zero defaults keep the
     * corresponding tick branches disabled so existing unit
     * tests and pre-configured callers see no behaviour change;
     * the xs-1c-kmhv3 live run sets them via QMP qom-set at
     * startup to arm the Timeout / SQFull causes for natural
     * store-dense evidence.
     */
    uint32_t sbuffer_inactive_threshold;
    uint32_t sbuffer_sqfull_commit_lag_threshold;
    uint64_t rename_stalls;      /* Round 50: rename_width=0 events */
    uint64_t store_sqn_next;     /* monotonic per-store sequence */

    /*
     * Round 52 AC-K-5: live bank-conflict state on the cpu-model
     * retire path. `bank_count` + `bank_conflict_stall_cycles` are
     * forwarded from the accel class so the charge path can
     * account the conflict stall in-line without routing through
     * the MSHR (whose now_cycle granularity is coarser than the
     * retire-by-retire cadence we want to observe). Zero /
     * `bank_count<=1` / `bank_conflict_stall_cycles==0` leaves
     * the tracker inert, preserving pre-round-52 behaviour.
     */
    uint32_t bank_count;
    uint32_t bank_conflict_stall_cycles;
    uint32_t last_mem_access_bank;
    uint64_t last_mem_access_cycle;
    bool     last_mem_access_valid;
    uint64_t bank_conflict_cpu_events;
    uint64_t sbuffer_evict_cpu_events;

    /*
     * Real-eviction counter, lifetime-only. Accumulates the
     * deltas returned by cae_sbuffer_tick on every retire so QMP
     * observers can separate "pressure signal"
     * (sbuffer_evict_cpu_events, unchanged semantics per AC-6)
     * from "actual drain" (sbuffer_eviction_events). Covers the
     * Timeout cause today; the Full / SQFull causes fold into
     * the same delta once their branches land.
     */
    uint64_t sbuffer_eviction_events;

    /*
     * Runtime scheduler issue-port cap. Default CAE_OOO_SCHED_PORTS
     * (2); structural max CAE_OOO_SCHED_PORTS_MAX (8). The QOM
     * property stores the raw requested value; charge-time logic
     * folds 0→default and >MAX→MAX with a one-shot warning.
     */
    uint32_t sched_issue_ports;
    bool     sched_issue_ports_warn_latch;

    /*
     * Virtual-issue batching. When virtual_issue_window > 0,
     * consecutive independent retires share a single structural
     * issue cost instead of each paying ceil(rename_width/issue_cap).
     * Independence = current uop src_regs has no overlap with the
     * previous retire's dst_regs (x0 excluded).
     */
    uint32_t virtual_issue_window;
    uint32_t virtual_issue_batch_count;
    uint8_t  last_retire_dst_regs[2];
    uint8_t  last_retire_num_dst;

    /*
     * Dependent-load serialization. Tracks the most recent load's
     * destination register across intervening retires. Cleared only
     * when a later retire overwrites that specific register. This
     * handles the pointer-chase pattern: ld t0; addi t2; bne; ld t0.
     */
    uint32_t dependent_load_stall_cycles;
    uint8_t  tracked_load_dst_reg;
    bool     tracked_load_dst_valid;

    /*
     * Watermark advanced by the commit loop to the highest sqn
     * whose ROB entry has retired. The async sbuffer drain step
     * (wired up in a later change) will compare this against the
     * oldest sbuffer entry's sqn before releasing one payload per
     * tick, decoupling store commit from sbuffer drain so
     * occupancy can actually climb to watermark. Participates in
     * speculative save/restore so a mispredict rollback rewinds
     * the watermark along with the ROB it tracks.
     */
    uint64_t sbuffer_commit_sqn;

    /* Stats. */
    uint64_t charges;
    uint64_t total_cycles;
    uint64_t dispatch_stalls;     /* ROB / RAT / LSQ backpressure */
    uint64_t commit_width_stalls; /* commit_width < count */
    uint64_t sbuffer_stalls;      /* sbuffer full -> store alloc fail */
    uint64_t sbuffer_commits;     /* payload-preserving drains */
    uint64_t sbuffer_squashes;    /* entries discarded on pipeline squash */

    /*
     * Reviewer-visible payload of the most recent store to drain
     * through the sbuffer. Exposed via read-only QOM properties
     * so a QMP probe (or the unit-test harness) can confirm the
     * sbuffer really carries store payloads across the retire
     * boundary, not just a count. Zeroed on init; stays zero when
     * no stores have committed.
     */
    uint64_t last_committed_store_sqn;
    uint64_t last_committed_store_pc;
    uint64_t last_committed_store_addr;
    uint64_t last_committed_store_size;
    uint64_t last_committed_store_value;
};

static void cae_cpu_ooo_complete(UserCreatable *uc, Error **errp)
{
    CaeCpuOoo *m = CAE_CPU_OOO(uc);

    cae_ooo_rob_init(&m->rob, m->rob_size);
    cae_ooo_iq_init(&m->iq, m->rob_size, m->issue_width);
    cae_ooo_lsq_init(&m->lsq, m->lq_size, m->sq_size);
    cae_ooo_rat_init(&m->rat, m->num_phys_int_regs,
                     m->num_phys_float_regs);
    cae_ooo_scheduler_init(&m->scheduler);
    cae_ooo_violation_init(&m->violation);
    m->now_cycle = 0;

    /*
     * BS-28 round 11: build the embedded sbuffer as a QOM child
     * of this cpu-model instance. Parenting is required for the
     * standard QOM canonical-path invariants (BL-20260417-qom-
     * link-target-needs-canonical-path); the child is named
     * "sbuffer" so QMP probes can observe its counters under
     * the sbuffer child path.
     */
    Object *sb_obj = object_new(TYPE_CAE_SBUFFER);
    object_property_set_uint(sb_obj, "sbuffer-size",
                             m->sbuffer_size, &error_abort);
    /*
     * Round 50 AC-K-5: forward the evict-threshold knob into the
     * sbuffer child before completing its construction. Zero
     * leaves the tracker disabled (round-49 behaviour).
     */
    if (m->sbuffer_evict_threshold) {
        object_property_set_uint(sb_obj, "evict-threshold",
                                 m->sbuffer_evict_threshold,
                                 &error_abort);
    }
    /*
     * Forward the Round-4 tick-driver thresholds before
     * `user_creatable_complete` runs so the sbuffer's own setter
     * clamp (inactive-threshold == 1 -> 0 + warn) sees the
     * intended value. Zero leaves each tick branch disabled,
     * matching the pre-forwarding behaviour for callers that
     * have not set the QOM knobs on this cpu-model.
     */
    if (m->sbuffer_inactive_threshold) {
        object_property_set_uint(sb_obj, "inactive-threshold",
                                 m->sbuffer_inactive_threshold,
                                 &error_abort);
    }
    if (m->sbuffer_sqfull_commit_lag_threshold) {
        object_property_set_uint(sb_obj,
                                 "sqfull-commit-lag-threshold",
                                 m->sbuffer_sqfull_commit_lag_threshold,
                                 &error_abort);
    }
    user_creatable_complete(USER_CREATABLE(sb_obj), &error_abort);
    object_property_add_child(OBJECT(m), "sbuffer", sb_obj);
    object_unref(sb_obj);
    m->sbuffer = sb_obj;
    m->store_sqn_next = 1u;   /* 0 reserved as "no prior store" */
    (void)errp;
}

static void cae_cpu_ooo_finalize(Object *obj)
{
    CaeCpuOoo *m = CAE_CPU_OOO(obj);

    cae_ooo_rob_destroy(&m->rob);
}

/*
 * BS-30 round 12: helper to drain one committed ROB entry's
 * side-effects — LSQ slot release, RAT register release, sbuffer
 * payload drain. Called from the per-entry commit loop so every
 * released slot is tied to the specific ROB entry that retired,
 * not to the current dispatch's uop. Round 11 mirrored the
 * dispatch-time uop in the commit loop, which freed the wrong
 * resources whenever dispatch was stalled but commit still made
 * progress.
 */
static void cae_cpu_ooo_drain_one(CaeCpuOoo *m, const CaeOooEntry *e)
{
    /*
     * Round 48 (AC-K-4): prefer the concrete ring-handle form so
     * commit releases the EXACT LSQ entry that dispatch reserved.
     * Fall back to the count-only path when dispatch did not
     * allocate a handle (legacy callers, test-seeded ROB entries).
     */
    if (e->is_load) {
        if (e->lq_handle != CAE_OOO_INVALID_HANDLE) {
            cae_ooo_lsq_commit_load_handle(&m->lsq, e->lq_handle);
        } else {
            cae_ooo_lsq_commit_load(&m->lsq);
        }
    }
    if (e->is_store) {
        if (e->sq_handle != CAE_OOO_INVALID_HANDLE) {
            cae_ooo_lsq_commit_store_handle(&m->lsq, e->sq_handle);
        } else {
            cae_ooo_lsq_commit_store(&m->lsq);
        }
    }
    /*
     * Round 48: release concrete RAT phys-id if dispatch recorded
     * one. The concrete path handles both the normal "allocated a
     * phys id" case AND the no-op "arch_dst == 0 (x0)" case —
     * the x0 sentinel means dispatch DID NOT consume a phys reg,
     * so commit must not return one either. Fall back to the
     * count-only free only when dispatch never went through the
     * concrete API (pre-round-48 callers / seeded ROB entries).
     *
     * Architectural commit pushes the PREVIOUS arch->phys mapping
     * onto the free list (that phys-id is now architecturally
     * dead); the current map (= new_phys) stays in place. Squash-
     * restore uses the spec snapshot which rewinds the map to the
     * save-time state.
     */
    if (e->dst_arch_int != CAE_OOO_INVALID_REG) {
        /* Concrete int path. x0-dst (new_phys_int=0) is a no-op. */
        if (e->new_phys_int != 0u) {
            CaeOooRat *rat = &m->rat;
            if (rat->int_free_count < CAE_OOO_RAT_MAX_INT_PHYS &&
                e->prev_phys_int != 0u) {
                rat->int_free[rat->int_free_count++] =
                    e->prev_phys_int;
            }
            if (rat->int_inflight > 0u) {
                rat->int_inflight--;
            }
            rat->int_alloc_seq++;
        }
    } else if (e->dst_arch_fp != CAE_OOO_INVALID_REG) {
        if (e->new_phys_fp != 0u) {
            CaeOooRat *rat = &m->rat;
            if (rat->fp_free_count < CAE_OOO_RAT_MAX_FP_PHYS &&
                e->prev_phys_fp != 0u) {
                rat->fp_free[rat->fp_free_count++] =
                    e->prev_phys_fp;
            }
            if (rat->fp_inflight > 0u) {
                rat->fp_inflight--;
            }
        }
    } else if (e->num_dst_int || e->num_dst_float) {
        cae_ooo_rat_free_counts(&m->rat,
                                e->num_dst_int, e->num_dst_float);
    }
    if (e->sbuffer_sqn != 0u) {
        /*
         * Advance the tick-driver commit watermark to this
         * entry's sqn. The actual sbuffer ring release — plus
         * the downstream sbuffer-commits / last-committed-store-*
         * bookkeeping — happens one step later inside
         * cae_sbuffer_tick's limited-drain branch, so sbuffer
         * occupancy can legitimately remain non-zero across a
         * ROB commit. This decouples store commit (ROB side)
         * from sbuffer drain (tick side) so natural workloads
         * finally let occupancy climb to the Full watermark
         * instead of bouncing back to zero on every retire.
         */
        if (e->sbuffer_sqn > m->sbuffer_commit_sqn) {
            m->sbuffer_commit_sqn = e->sbuffer_sqn;
        }
    }
}

/*
 * M3' charge function — BS-30 round 12 dispatch/commit-coherent
 * rewrite.
 *
 * Per retired architectural insn:
 *   - Pre-check every resource the dispatch needs: ROB slot,
 *     LSQ-load (if load), LSQ-store (if store), sbuffer slot (if
 *     store), RAT free registers. If any check fails, this retire
 *     stalls: +1 cycle, bump the matching stall counter, and
 *     return without touching any of the pipeline's state. This
 *     replaces round-11's "allocate everything optimistically and
 *     leak on ROB failure" path — all allocations are now
 *     atomic-on-success.
 *   - On pre-check success, allocate atomically and dispatch the
 *     ROB entry with the sbuffer_sqn recorded so the commit walk
 *     can drain the right store. Pass uop->mem_value so the
 *     sbuffer carries the real store payload rather than a
 *     hardcoded zero.
 *   - Commit: call cae_ooo_rob_commit_one repeatedly up to
 *     commit_width; for each popped entry, release its own LSQ /
 *     RAT / sbuffer slots via cae_cpu_ooo_drain_one.
 *   - Mispredict-recovery (wrong-path squash) goes through the
 *     cae_cpu_ooo_squash_after() entry point, not this charge
 *     function; charge stays on the happy path.
 */
static uint32_t cae_cpu_ooo_charge(Object *obj, const CaeCpu *cpu,
                                   const CaeUop *uop)
{
    CaeCpuOoo *m = CAE_CPU_OOO(obj);
    uint32_t cycles = 1u;    /* M3' minimum per-retire charge */

    (void)cpu;

    if (!uop) {
        goto done;
    }

    /*
     * Round 51 AC-K-5 (Codex round-50 directive #1): `rename_width=0`
     * is a REAL dispatch gate. Check this BEFORE any resource
     * pre-check, ROB / LSQ / RAT / sbuffer allocation, IQ /
     * scheduler enqueue, or violation record. Under rename_width=0
     * the retire still takes 1 cycle but no sub-structure mutates.
     * Regression:
     * /cae/ooo/cpu-ooo-rename-width-zero-no-pipeline-mutation pins
     * this invariant — five retires must leave `rob_count`,
     * `rat_int_inflight`, `lsq_{load,store}_count`, and sbuffer
     * `occupancy` unchanged while `rename-stalls` advances by 5.
     *
     * Round 50 placed this gate AFTER the allocation block, which
     * Codex correctly rejected as a semantic-only (counter-only)
     * stall: ROB / LSQ / RAT / sbuffer had already mutated by the
     * time `rename_stalls` bumped.
     */
    if (m->rename_width == 0u) {
        m->rename_stalls++;
        goto done;
    }

    /*
     * Resource pre-checks. Any failure stalls this retire and
     * leaves every sub-structure untouched. sbuffer-full is
     * attributed separately from generic dispatch backpressure
     * so the calibration knob (M5' sbuffer-size) has an
     * independently-observable counter.
     */
    bool rob_ok  = cae_ooo_rob_has_slot(&m->rob);
    bool lq_ok   = (!uop->is_load)  || cae_ooo_lsq_has_load_slot(&m->lsq);
    bool sq_ok   = (!uop->is_store) || cae_ooo_lsq_has_store_slot(&m->lsq);
    bool sb_ok   = (!uop->is_store)
                   || cae_sbuffer_has_slot((CaeSbuffer *)m->sbuffer);
    bool rat_ok  = cae_ooo_rat_has_slot(&m->rat, uop);

    if (!rob_ok || !lq_ok || !sq_ok || !rat_ok) {
        m->dispatch_stalls++;
        goto commit;
    }
    if (!sb_ok) {
        m->sbuffer_stalls++;
        goto commit;
    }

    /*
     * All resources available — allocate atomically. Round 48
     * (AC-K-4) captures concrete RAT phys-ids and LSQ ring
     * handles into the ROB entry so commit / squash / live_restore
     * release the EXACT reservations rather than count-matched
     * approximations.
     */
    uint64_t sbuffer_sqn = 0u;
    CaeOooDispatchHandles handles = {
        .dst_arch_int  = CAE_OOO_INVALID_REG,
        .dst_arch_fp   = CAE_OOO_INVALID_REG,
        .new_phys_int  = 0u,
        .new_phys_fp   = 0u,
        .prev_phys_int = 0u,
        .prev_phys_fp  = 0u,
        .lq_handle     = CAE_OOO_INVALID_HANDLE,
        .sq_handle     = CAE_OOO_INVALID_HANDLE,
    };
    if (uop->is_store) {
        sbuffer_sqn = m->store_sqn_next++;
        bool ok = cae_sbuffer_alloc((CaeSbuffer *)m->sbuffer,
                                    sbuffer_sqn, uop->pc,
                                    uop->mem_addr, uop->mem_size,
                                    uop->mem_value);
        assert(ok);
    }
    if (uop->is_load) {
        handles.lq_handle =
            cae_ooo_lsq_allocate_load_entry(&m->lsq, uop->mem_addr,
                                            uop->mem_size,
                                            m->now_cycle);
        assert(handles.lq_handle != CAE_OOO_INVALID_HANDLE);
    }
    if (uop->is_store) {
        handles.sq_handle =
            cae_ooo_lsq_allocate_store_entry(&m->lsq, uop->mem_addr,
                                             uop->mem_size,
                                             uop->mem_value,
                                             m->now_cycle);
        assert(handles.sq_handle != CAE_OOO_INVALID_HANDLE);
    }
    if (uop->num_dst > 0) {
        uint8_t arch_dst = (uop->num_dst > 0u && uop->dst_regs[0] < 32u)
                           ? uop->dst_regs[0] : 0u;
        if (uop->fu_type == CAE_FU_FPU) {
            handles.dst_arch_fp = arch_dst;
            handles.new_phys_fp = cae_ooo_rat_allocate_dst_fp(
                &m->rat, arch_dst, &handles.prev_phys_fp);
        } else {
            handles.dst_arch_int = arch_dst;
            handles.new_phys_int = cae_ooo_rat_allocate_dst_int(
                &m->rat, arch_dst, &handles.prev_phys_int);
        }
    }
    bool disp_ok = cae_ooo_rob_dispatch_ex(&m->rob, uop,
                                           (uint32_t)m->now_cycle,
                                           sbuffer_sqn, &handles);
    assert(disp_ok);
    (void)disp_ok;

    /*
     * IQ accounting (legacy flat counter) + scheduler accounting
     * (round-49 live wiring of `hw/cae/ooo/scheduler.c`). The
     * scheduler's segmented issue surface runs in parallel with
     * the flat IQ: flat IQ tracks overall allocation pressure
     * (used by round-2 stall accounting); scheduler tracks
     * per-segment 3-seg x 2-port issue behaviour the plan's
     * AC-K-5 terminal gate calibrates against. Both are
     * live-enqueued at dispatch and live-drained below on the
     * commit tick, so the scheduler counters (`enqueued`,
     * `issued`, `backpressure`) track actual retire traffic
     * rather than sitting as unused file-surface.
     *
     * Round 51 AC-K-5: `rename_width=0` is handled at the top of
     * this function (dispatch gate — no mutation). By the time
     * we reach here, rename_width >= 1. The flat IQ's `try_issue`
     * takes `rename_width` as the ready-uops ceiling; the
     * segmented scheduler enqueues one entry per retire (one
     * real arch uop per TB under CAE's one-insn-per-TB model).
     */
    cae_ooo_iq_enqueue(&m->iq);
    cae_ooo_iq_try_issue(&m->iq, m->rename_width,
                         (uint32_t)m->now_cycle);
    cae_ooo_scheduler_enqueue(&m->scheduler, uop->pc,
                              (uint8_t)uop->fu_type);

    /*
     * Round 49 AC-K-5: live violation tracker. Every load / store
     * commit records (addr, size) into the RARQ / RAWQ rings; a
     * new load that overlaps a pending RAWQ entry flags a
     * `raw_violations` bump and populates the replay slot. The
     * slot is consumed on the next retire tick so a violation
     * produces observable replay traffic rather than a dead
     * helper.
     */
    if (uop->is_load) {
        if (cae_ooo_violation_check_raw(&m->violation,
                                        uop->mem_addr,
                                        uop->mem_size)) {
            /* Replay consumed on this same retire. */
            cae_ooo_violation_consume_replay(&m->violation, NULL);
        }
        cae_ooo_violation_record_load(&m->violation,
                                      uop->mem_addr,
                                      uop->mem_size);
    }
    if (uop->is_store) {
        cae_ooo_violation_record_store(&m->violation,
                                       uop->mem_addr,
                                       uop->mem_size);
    }

commit:
    /* Commit drain. Advance clock before consulting ready_cycle. */
    m->now_cycle++;

    /*
     * Round 51 AC-K-5: scheduler drives live timing via per-retire
     * cycle charge. Round 52 (Codex round-51 directive #1)
     * DECOUPLES the commit loop from `sched_issued`: the round-51
     * code gated commit on `min(commit_width, sched_issued)`,
     * which wedged ROB drain on long-latency stall ticks (the
     * scheduler's single per-retire enqueue drains on the first
     * tick; subsequent stall ticks saw `sched_issued == 0` and
     * blocked commits even for ready ROB heads). The live-timing
     * influence of `issue_width` is now expressed PURELY through
     * the `ceil(rename_width / issue_cap)` cycle charge; the
     * commit loop drains up to `commit_width` ready ROB entries
     * per cycle, same as pre-round-51 behaviour, so DIV-heavy
     * stall patterns still make forward progress. Regression
     * `/cae/ooo/cpu-ooo-commit-drains-under-stall-after-sched-empty`
     * pins this. Round 52 additionally wires bank-conflict and
     * sbuffer-evict-threshold into the live cycle charge below
     * so those knobs measurably affect total_cycles on natural
     * workload memory traffic.
     */
    QEMU_BUILD_BUG_ON(CAE_OOO_SCHED_PORTS_MAX != 8u);
    CaeOooSchedEntry sched_out[CAE_OOO_SCHED_PORTS_MAX];
    uint8_t sched_cap = (m->issue_width > 255u)
                        ? 255u : (uint8_t)m->issue_width;
    (void)cae_ooo_scheduler_issue_cycle_bounded(
              &m->scheduler, sched_cap, sched_out);

    /*
     * Effective scheduler issue-port cap. Resolve the runtime
     * field: 0 folds to the default, values above the structural
     * maximum clamp with a one-shot warning. The final issue_cap
     * is the minimum of issue_width and the resolved port count.
     */
    uint32_t effective_sched_issue_ports;
    {
        uint32_t raw = m->sched_issue_ports;
        if (raw == 0u) {
            effective_sched_issue_ports = CAE_OOO_SCHED_PORTS;
        } else if (raw > CAE_OOO_SCHED_PORTS_MAX) {
            effective_sched_issue_ports = CAE_OOO_SCHED_PORTS_MAX;
            if (!m->sched_issue_ports_warn_latch) {
                warn_report("cae-cpu-ooo: sched-issue-ports=%u exceeds"
                            " structural max %u, clamping to %u",
                            raw, CAE_OOO_SCHED_PORTS_MAX,
                            CAE_OOO_SCHED_PORTS_MAX);
                m->sched_issue_ports_warn_latch = true;
            }
        } else {
            effective_sched_issue_ports = raw;
        }
    }
    uint32_t issue_cap = m->issue_width;
    if (issue_cap == 0u ||
        issue_cap > effective_sched_issue_ports) {
        issue_cap = effective_sched_issue_ports;
    }

    /*
     * Cycle charge from the rename/issue imbalance. rename=1
     * collapses the formula to 1 cycle (unit tests and
     * pre-round-51 callers). rename >= 2 charges the full
     * `ceil(rename/issue_cap)` — the structural issue-latency.
     *
     * Virtual-issue batching: when enabled, consecutive independent
     * retires share a single issue window. The FIRST retire in a
     * batch pays the full structural cost; SUBSEQUENT independent
     * retires in the batch get cycles=0 (co-issue). This enables
     * IPC > 1.0 when the model sees independent instruction streams.
     */
    if (m->rename_width > 1u) {
        uint32_t issue_cycles =
            (m->rename_width + issue_cap - 1u) / issue_cap;

        bool independent = true;
        if (m->virtual_issue_window > 0u && m->last_retire_num_dst > 0u) {
            for (uint8_t s = 0; s < uop->num_src && independent; s++) {
                for (uint8_t d = 0; d < m->last_retire_num_dst; d++) {
                    if (m->last_retire_dst_regs[d] == 0u) {
                        continue;
                    }
                    if (uop->src_regs[s] == m->last_retire_dst_regs[d]) {
                        independent = false;
                        break;
                    }
                }
            }
        }

        /*
         * Also check against the tracked load dependency register,
         * but ONLY for load retires. Non-load consumers of a
         * load-produced register still co-issue normally against
         * the immediately previous retire's dst only.
         */
        if (independent && uop->is_load &&
            m->tracked_load_dst_valid &&
            m->tracked_load_dst_reg != 0u) {
            for (uint8_t s = 0; s < uop->num_src; s++) {
                if (uop->src_regs[s] == m->tracked_load_dst_reg) {
                    independent = false;
                    break;
                }
            }
        }

        if (m->virtual_issue_window > 0u &&
            independent &&
            m->virtual_issue_batch_count > 0u &&
            m->virtual_issue_batch_count < m->virtual_issue_window) {
            m->virtual_issue_batch_count++;
            cycles = 0;
        } else {
            m->virtual_issue_batch_count = 1;
            if (issue_cycles > cycles) {
                cycles = issue_cycles;
            }
        }
    }

    /*
     * Dependent-load serialization: when the current load sources
     * from a register produced by a previous load, add stall cycles.
     * The tracked register persists across non-overwriting retires,
     * so ld t0; addi t2; bne; ld t0 correctly detects the chain.
     */
    if (m->dependent_load_stall_cycles > 0u &&
        uop->is_load && m->tracked_load_dst_valid &&
        m->tracked_load_dst_reg != 0u) {
        bool dep = false;
        for (uint8_t s = 0; s < uop->num_src; s++) {
            if (uop->src_regs[s] == m->tracked_load_dst_reg) {
                dep = true;
                break;
            }
        }
        if (dep) {
            cycles += m->dependent_load_stall_cycles;
        }
    }

    /*
     * Update tracked load register. A load sets the tracked reg.
     * Any retire that overwrites the tracked reg clears it.
     */
    if (uop->is_load && uop->num_dst > 0 && uop->dst_regs[0] != 0u) {
        m->tracked_load_dst_reg = uop->dst_regs[0];
        m->tracked_load_dst_valid = true;
    } else if (m->tracked_load_dst_valid) {
        for (uint8_t d = 0; d < uop->num_dst; d++) {
            if (uop->dst_regs[d] == m->tracked_load_dst_reg) {
                m->tracked_load_dst_valid = false;
                break;
            }
        }
    }

    memcpy(m->last_retire_dst_regs, uop->dst_regs,
           sizeof(m->last_retire_dst_regs));
    m->last_retire_num_dst = uop->num_dst;

    /*
     * Round 52 AC-K-5 (Codex round-51 directive #4):
     * bank-conflict live cycle charge. When a memory retire's
     * bank collides with the previous memory retire's bank
     * within `bank_conflict_stall_cycles` of now_cycle, add the
     * remaining window to this retire's cycle charge. Under
     * CAE's one-insn-per-TB constraint, consecutive retires
     * sit exactly 1 cycle apart, so `stall_cycles == 2` makes
     * EVERY same-bank back-to-back access charge +1 cycle.
     * `bank_count <= 1` or `stall_cycles == 0` (round-50
     * defaults) disable the tracker — the round-49+ unit
     * regressions that exercise the bank-conflict counter on
     * the mshr path are unaffected since they bypass the
     * cpu-model charge path entirely.
     */
    if (m->bank_count > 1u && m->bank_conflict_stall_cycles > 0u &&
        (uop->is_load || uop->is_store) && uop->mem_size > 0u) {
        uint32_t bank = (uint32_t)((uop->mem_addr >> 6) %
                                    m->bank_count);
        if (m->last_mem_access_valid &&
            m->last_mem_access_bank == bank) {
            uint64_t busy_until = m->last_mem_access_cycle +
                                  m->bank_conflict_stall_cycles;
            if (m->now_cycle < busy_until) {
                uint32_t stall =
                    (uint32_t)(busy_until - m->now_cycle);
                cycles += stall;
                m->bank_conflict_cpu_events++;
            }
        }
        m->last_mem_access_bank = bank;
        m->last_mem_access_cycle = m->now_cycle;
        m->last_mem_access_valid = true;
    }

    /*
     * Round 52 AC-K-5: sbuffer-evict live cycle charge. When
     * sbuffer occupancy at retire crosses `sbuffer_evict_
     * threshold`, add 1 cycle to this retire's charge as
     * "drain pressure" — the sbuffer back-pressures the
     * retire pipeline when it sits at the watermark. Zero
     * threshold (round-50 default) leaves the tracker inert.
     */
    if (m->sbuffer_evict_threshold > 0u && m->sbuffer != NULL) {
        uint32_t occ = cae_sbuffer_occupancy(
                           (CaeSbuffer *)m->sbuffer);
        if (occ >= m->sbuffer_evict_threshold) {
            cycles += 1u;
            m->sbuffer_evict_cpu_events++;
        }
    }

    /*
     * Plan segment 3: sbuffer drain + eviction tick. The tick
     * observes the PREVIOUS charge's `sbuffer_commit_sqn` so a
     * store dispatched and committed in the SAME charge is NOT
     * tick-drainable until the next charge — this is the
     * Milestone-C contract that decouples ROB commit from
     * sbuffer drain. Two independent outcomes per call:
     *   - limited commit-drain of one head-of-FIFO entry whose
     *     sqn is already covered by sbuffer_commit_sqn. The
     *     returned payload drives the reviewer-visible
     *     sbuffer-commits and last-committed-store-* surface,
     *     now wholly tick-owned since the ROB commit path stops
     *     releasing sbuffer entries directly.
     *   - at most one eviction cause (SQFull / Full / Timeout)
     *     on the post-drain head. Each cause bumps
     *     sbuffer_eviction_events by one; the existing
     *     sbuffer_evict_cpu_events pressure-signal above keeps
     *     its "retire with occupancy >= watermark" semantics
     *     unchanged — the two counters are intentionally
     *     separate.
     * Plan segment 4 runs AFTER this tick and is where
     * `sbuffer_commit_sqn` advances for the next charge's tick.
     */
    if (m->sbuffer != NULL) {
        CaeSbufferTickResult tick_res;
        cae_sbuffer_tick((CaeSbuffer *)m->sbuffer,
                         m->sbuffer_commit_sqn,
                         m->store_sqn_next,
                         &tick_res);
        if (tick_res.drained) {
            m->sbuffer_commits++;
            m->last_committed_store_sqn   = tick_res.drained_view.sqn;
            m->last_committed_store_pc    = tick_res.drained_view.pc;
            m->last_committed_store_addr  = tick_res.drained_view.addr;
            m->last_committed_store_size  = tick_res.drained_view.size;
            m->last_committed_store_value = tick_res.drained_view.value;
        }
        if (tick_res.cause != CAE_SBUFFER_EVICT_NONE) {
            m->sbuffer_eviction_events++;
        }
    }

    /*
     * Plan segment 4: commit drain. Release ROB/LSQ/RAT slots
     * immediately (existing semantics) and advance
     * `sbuffer_commit_sqn` to the latest committed store's sqn
     * so the NEXT charge's segment-3 tick can drain it. Under
     * the plan's ordering, the store allocated this charge does
     * NOT drain in this charge — it waits for the next tick.
     */
    uint32_t commit_cap = m->commit_width;

    uint32_t committed = 0u;
    CaeOooEntry entry;
    while (committed < commit_cap
           && cae_ooo_rob_commit_one(&m->rob,
                                     (uint32_t)m->now_cycle,
                                     &entry)) {
        cae_cpu_ooo_drain_one(m, &entry);
        committed++;
    }
    if (committed == 0u && m->rob.count > 0) {
        m->commit_width_stalls++;
    }

done:
    qatomic_set(&m->charges, qatomic_read(&m->charges) + 1);
    qatomic_set(&m->total_cycles,
                    qatomic_read(&m->total_cycles) + cycles);
    return cycles;
}

/*
 * BS-30 round 12: wrong-path squash entry point. A mispredict
 * undoes all in-flight speculative state and rolls the sbuffer
 * back to the last store whose sqn <= squash_sqn - 1. Counts the
 * actual dropped entries into sbuffer_squashes so the QOM stat
 * surface is no longer a dead counter.
 *
 * This is exposed as a public entry point (not a class method)
 * because the caller is the TCG predicted-path squash handler
 * landing in t-tcg-spec-path. For the round-12 unit tests the
 * helper is invoked directly against a CaeCpuOoo Object*.
 */
void cae_cpu_ooo_squash_after(Object *obj, uint64_t squash_sqn)
{
    /*
     * Round 31: the live t-tcg-spec-path resolve block calls this
     * unconditionally from cae_charge_executed_tb on mispredict,
     * and the caller can be wired to a non-OoO cpu-model (cpi1 /
     * inorder-5stage) when the user picks those configurations.
     * Degrade to a safe no-op in that case instead of asserting
     * on the CAE_CPU_OOO() macro. NULL obj is also a no-op.
     */
    if (obj == NULL) {
        return;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return;
    }
    uint32_t dropped = cae_sbuffer_squash_after(
        (CaeSbuffer *)m->sbuffer, squash_sqn);
    m->sbuffer_squashes += dropped;
    cae_ooo_rob_flush_after(&m->rob);
    cae_ooo_iq_flush(&m->iq);
    cae_ooo_lsq_flush(&m->lsq);
    cae_ooo_rat_flush(&m->rat);
    cae_ooo_scheduler_reset(&m->scheduler);
    cae_ooo_violation_reset(&m->violation);
}

static void cae_cpu_ooo_check_bpred(const Object *obj, const char *name,
                                    Object *val, Error **errp)
{
    (void)obj;
    (void)name;
    (void)val;
    (void)errp;
}

static void cae_cpu_ooo_get_stats(Object *obj, Visitor *v,
                                  const char *name,
                                  void *opaque, Error **errp)
{
    CaeCpuOoo *m = CAE_CPU_OOO(obj);
    uint64_t *ptr = opaque;
    uint64_t value;

    (void)name;
    /*
     * Read the 64-bit stat counter via a two-step atomic pattern:
     * qatomic_read on 32-bit hosts is not atomic on aligned
     * 64-bit reads, so we go through a helper that uses the 32-bit
     * low+high pair when needed. Same technique as CaeCpuInorder's
     * stats.
     */
    value = qatomic_read(ptr);
    visit_type_uint64(v, "stat", &value, errp);
    (void)m;
}

static void cae_cpu_ooo_instance_init(Object *obj)
{
    CaeCpuOoo *m = CAE_CPU_OOO(obj);

    m->rob_size = CAE_OOO_DEFAULT_ROB_SIZE;
    m->lq_size = CAE_OOO_DEFAULT_LQ_SIZE;
    m->sq_size = CAE_OOO_DEFAULT_SQ_SIZE;
    m->issue_width = CAE_OOO_DEFAULT_ISSUE_WIDTH;
    m->commit_width = CAE_OOO_DEFAULT_COMMIT_WIDTH;
    m->rename_width = CAE_OOO_DEFAULT_RENAME_WIDTH;
    m->num_phys_int_regs = CAE_OOO_DEFAULT_NUM_PHYS_INT_REGS;
    m->num_phys_float_regs = CAE_OOO_DEFAULT_NUM_PHYS_FLOAT_REGS;
    m->sbuffer_size = CAE_CPU_OOO_DEFAULT_SBUFFER_SIZE;

    object_property_add_uint32_ptr(obj, "rob-size", &m->rob_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "lq-size", &m->lq_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "sq-size", &m->sq_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "issue-width", &m->issue_width,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "commit-width", &m->commit_width,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "rename-width", &m->rename_width,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "num-phys-int-regs",
                                   &m->num_phys_int_regs,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "num-phys-float-regs",
                                   &m->num_phys_float_regs,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_link(obj, "bpred", TYPE_CAE_BPRED,
                             &m->bpred,
                             cae_cpu_ooo_check_bpred,
                             OBJ_PROP_LINK_STRONG);

    object_property_add_uint32_ptr(obj, "sbuffer-size", &m->sbuffer_size,
                                   OBJ_PROP_FLAG_READWRITE);
    /*
     * Round 50 AC-K-5: sbuffer evict-threshold forwarder knob.
     * Applied to the sbuffer child at complete() time; zero leaves
     * the child's tracker disabled. Accel-class forwarder is
     * `sbuffer-evict-threshold`.
     */
    object_property_add_uint32_ptr(obj, "sbuffer-evict-threshold",
                                   &m->sbuffer_evict_threshold,
                                   OBJ_PROP_FLAG_READWRITE);
    /*
     * Round-4 tick-driver threshold knobs. RW so QMP callers
     * (the xs-1c-kmhv3 live run, unit tests exercising the
     * natural-workload eviction path) can arm the Timeout /
     * SQFull branches before `user_creatable_complete` forwards
     * the values to the sbuffer child. Zero keeps the branch
     * inert, preserving the default behaviour of every
     * regression that never sets these knobs.
     */
    object_property_add_uint32_ptr(obj, "sbuffer-inactive-threshold",
                                   &m->sbuffer_inactive_threshold,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(
        obj, "sbuffer-sqfull-commit-lag-threshold",
        &m->sbuffer_sqfull_commit_lag_threshold,
        OBJ_PROP_FLAG_READWRITE);
    /*
     * Round 52 AC-K-5: live bank-conflict knobs on the cpu-model.
     * Separate from the MSHR-side copies (round 49) so the retire
     * path's conflict accounting is independent of the memory
     * backend's per-access tracking. Defaults are 0 (disabled);
     * accel-class forwarders `ooo-bank-count` and
     * `ooo-bank-conflict-stall-cycles` drive the YAML values.
     */
    object_property_add_uint32_ptr(obj, "bank-count",
                                   &m->bank_count,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "bank-conflict-stall-cycles",
                                   &m->bank_conflict_stall_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    m->sched_issue_ports = CAE_OOO_SCHED_PORTS;
    m->sched_issue_ports_warn_latch = false;
    object_property_add_uint32_ptr(obj, "sched-issue-ports",
                                   &m->sched_issue_ports,
                                   OBJ_PROP_FLAG_READWRITE);
    m->virtual_issue_window = 0;
    m->virtual_issue_batch_count = 0;
    m->last_retire_num_dst = 0;
    object_property_add_uint32_ptr(obj, "virtual-issue-window",
                                   &m->virtual_issue_window,
                                   OBJ_PROP_FLAG_READWRITE);
    m->dependent_load_stall_cycles = 0;
    m->tracked_load_dst_reg = 0;
    m->tracked_load_dst_valid = false;
    object_property_add_uint32_ptr(obj, "dependent-load-stall-cycles",
                                   &m->dependent_load_stall_cycles,
                                   OBJ_PROP_FLAG_READWRITE);

    object_property_add(obj, "charges", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->charges);
    object_property_add(obj, "total-cycles", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->total_cycles);
    object_property_add(obj, "dispatch-stalls", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->dispatch_stalls);
    object_property_add(obj, "commit-width-stalls", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->commit_width_stalls);

    /*
     * BS-28 round 11: sbuffer counters and last-committed-store
     * payload surface. All read-only, 64-bit counters so they
     * pair with the existing stats via the same get_stats path.
     */
    object_property_add(obj, "sbuffer-stalls", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->sbuffer_stalls);
    object_property_add(obj, "sbuffer-commits", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->sbuffer_commits);
    object_property_add(obj, "sbuffer-squashes", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->sbuffer_squashes);
    object_property_add(obj, "last-committed-store-sqn", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->last_committed_store_sqn);
    object_property_add(obj, "last-committed-store-pc", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->last_committed_store_pc);
    object_property_add(obj, "last-committed-store-addr", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->last_committed_store_addr);
    object_property_add(obj, "last-committed-store-size", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->last_committed_store_size);
    object_property_add(obj, "last-committed-store-value", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->last_committed_store_value);

    /*
     * Round 49 AC-K-5: scheduler + violation observable counters.
     * These are now fed by the live charge path (`cae_cpu_ooo_
     * charge` enqueue / issue + load-store record), so a live
     * benchmark run's QMP snapshot exposes the AC-K-5 modules'
     * actual traffic rather than always-zero dead surface.
     */
    object_property_add(obj, "scheduler-enqueued", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->scheduler.enqueued);
    object_property_add(obj, "scheduler-issued", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->scheduler.issued);
    object_property_add(obj, "scheduler-backpressure", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->scheduler.backpressure);
    object_property_add(obj, "violation-loads-observed", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->violation.loads_observed);
    object_property_add(obj, "violation-stores-observed", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->violation.stores_observed);
    object_property_add(obj, "violation-raw-violations", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->violation.raw_violations);
    object_property_add(obj, "violation-replay-consumed", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->violation.replay_consumed);
    /*
     * Round 50 AC-K-5: rename-width stall counter. Bumps on every
     * dispatch attempt observed while rename_width==0. Exposed
     * alongside the existing dispatch_stalls counter so QMP /
     * run-cae.py JSON can distinguish "rename throttled" from
     * "ROB / LSQ / RAT resource starved".
     */
    object_property_add(obj, "rename-stalls", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->rename_stalls);
    /*
     * Round 52 AC-K-5: cpu-model-side bank-conflict and
     * sbuffer-evict event counters. Distinct from the mshr / sbuffer
     * module-level counters because these track retire-path stall
     * events driven by the cycle-charge formula; the module-level
     * counters track backend-side bank pressure. Under CAE's
     * one-insn-per-TB retire these two measures can diverge
     * meaningfully.
     */
    object_property_add(obj, "bank-conflict-cpu-events", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->bank_conflict_cpu_events);
    object_property_add(obj, "sbuffer-evict-cpu-events", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->sbuffer_evict_cpu_events);
    /*
     * Real-eviction counter — distinct from the pressure-signal
     * above. Accumulates the delta returned by the sbuffer
     * timeout / full / sqfull tick pump on every retire. Kept
     * separate so existing callers of sbuffer-evict-cpu-events
     * see unchanged "retire with occupancy above watermark"
     * semantics.
     */
    object_property_add(obj, "sbuffer-eviction-events", "uint64",
                        cae_cpu_ooo_get_stats, NULL, NULL,
                        &m->sbuffer_eviction_events);
}

static void cae_cpu_ooo_class_init(ObjectClass *klass, const void *data)
{
    UserCreatableClass *uc = USER_CREATABLE_CLASS(klass);
    CaeCpuModelClass *cc = CAE_CPU_MODEL_CLASS(klass);

    (void)data;
    uc->complete = cae_cpu_ooo_complete;
    cc->charge = cae_cpu_ooo_charge;
}

static const TypeInfo cae_cpu_ooo_type = {
    .name = TYPE_CAE_CPU_OOO,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeCpuOoo),
    .instance_init = cae_cpu_ooo_instance_init,
    .instance_finalize = cae_cpu_ooo_finalize,
    .class_init = cae_cpu_ooo_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { TYPE_CAE_CPU_MODEL },
        { }
    }
};

static void cae_cpu_ooo_register_types(void)
{
    type_register_static(&cae_cpu_ooo_type);
}

type_init(cae_cpu_ooo_register_types)

/* ------------------------------------------------------------------ */
/*  Speculation save/restore: OoO scalar lane                         */
/* ------------------------------------------------------------------ */

/*
 * Private struct definition — opaque to callers. Round 28 carried
 * just the scalar now_cycle / store_sqn_next. Round 30 composes
 * five owning-module sub-blobs for the full container state the
 * M4' speculation path must unwind on a mispredict: ROB slots,
 * IQ / LSQ / RAT scalars, sbuffer ring + stats. Each sub-blob is
 * an opaque pointer owned by its respective hw/cae/ooo/ module so
 * this composer stays encapsulated — this file does not reach into
 * any container's private representation.
 */
struct CaeOooSpecSnapshot {
    uint64_t now_cycle;
    uint64_t store_sqn_next;
    /*
     * Tick-driver watermark. Snapshot-classified because on
     * mispredict rollback the committed-sqn frontier must rewind
     * with the ROB it mirrors — leaving it stale would let the
     * async sbuffer drain release a sqn whose ROB entry no
     * longer committed. Telemetry-style counters
     * (sbuffer_evict_cpu_events etc.) stay lifetime-only and are
     * deliberately absent from this struct.
     */
    uint64_t sbuffer_commit_sqn;

    uint32_t virtual_issue_batch_count;
    uint8_t  last_retire_dst_regs[2];
    uint8_t  last_retire_num_dst;
    uint8_t  tracked_load_dst_reg;
    bool     tracked_load_dst_valid;

    CaeOooRobSpecSnapshot  *rob_snap;
    CaeOooIqSpecSnapshot   *iq_snap;
    CaeOooLsqSpecSnapshot  *lsq_snap;
    CaeOooRatSpecSnapshot  *rat_snap;
    CaeSbufferSpecSnapshot *sbuffer_snap;
};

CaeOooSpecSnapshot *cae_cpu_ooo_spec_snapshot_save(Object *obj)
{
    if (obj == NULL) {
        return NULL;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        /* Non-OoO cpu-model (in-order / cpi1); no scalar lane. */
        return NULL;
    }
    CaeOooSpecSnapshot *snap = g_new0(CaeOooSpecSnapshot, 1);
    snap->now_cycle = m->now_cycle;
    snap->store_sqn_next = m->store_sqn_next;
    snap->sbuffer_commit_sqn = m->sbuffer_commit_sqn;
    snap->virtual_issue_batch_count = m->virtual_issue_batch_count;
    memcpy(snap->last_retire_dst_regs, m->last_retire_dst_regs,
           sizeof(snap->last_retire_dst_regs));
    snap->last_retire_num_dst = m->last_retire_num_dst;
    snap->tracked_load_dst_reg = m->tracked_load_dst_reg;
    snap->tracked_load_dst_valid = m->tracked_load_dst_valid;
    snap->rob_snap     = cae_ooo_rob_spec_snapshot_save(&m->rob);
    snap->iq_snap      = cae_ooo_iq_spec_snapshot_save(&m->iq);
    snap->lsq_snap     = cae_ooo_lsq_spec_snapshot_save(&m->lsq);
    snap->rat_snap     = cae_ooo_rat_spec_snapshot_save(&m->rat);
    snap->sbuffer_snap = cae_sbuffer_spec_snapshot_save(m->sbuffer);
    return snap;
}

void cae_cpu_ooo_spec_snapshot_restore(Object *obj,
                                       const CaeOooSpecSnapshot *snap)
{
    if (obj == NULL || snap == NULL) {
        return;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return;
    }
    m->now_cycle = snap->now_cycle;
    m->store_sqn_next = snap->store_sqn_next;
    m->sbuffer_commit_sqn = snap->sbuffer_commit_sqn;
    m->virtual_issue_batch_count = snap->virtual_issue_batch_count;
    memcpy(m->last_retire_dst_regs, snap->last_retire_dst_regs,
           sizeof(m->last_retire_dst_regs));
    m->last_retire_num_dst = snap->last_retire_num_dst;
    m->tracked_load_dst_reg = snap->tracked_load_dst_reg;
    m->tracked_load_dst_valid = snap->tracked_load_dst_valid;
    cae_ooo_rob_spec_snapshot_restore(&m->rob, snap->rob_snap);
    cae_ooo_iq_spec_snapshot_restore(&m->iq, snap->iq_snap);
    cae_ooo_lsq_spec_snapshot_restore(&m->lsq, snap->lsq_snap);
    cae_ooo_rat_spec_snapshot_restore(&m->rat, snap->rat_snap);
    cae_sbuffer_spec_snapshot_restore(m->sbuffer, snap->sbuffer_snap);
}

void cae_cpu_ooo_spec_snapshot_drop(CaeOooSpecSnapshot *snap)
{
    if (snap == NULL) {
        return;
    }
    cae_ooo_rob_spec_snapshot_drop(snap->rob_snap);
    cae_ooo_iq_spec_snapshot_drop(snap->iq_snap);
    cae_ooo_lsq_spec_snapshot_drop(snap->lsq_snap);
    cae_ooo_rat_spec_snapshot_drop(snap->rat_snap);
    cae_sbuffer_spec_snapshot_drop(snap->sbuffer_snap);
    g_free(snap);
}

void cae_cpu_ooo_spec_test_seed(Object *obj, uint64_t now_cycle,
                                uint64_t store_sqn_next)
{
    if (obj == NULL) {
        return;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return;
    }
    m->now_cycle = now_cycle;
    m->store_sqn_next = store_sqn_next;
}

void cae_cpu_ooo_test_seed_sbuffer_commit_sqn(Object *obj, uint64_t value)
{
    if (obj == NULL) {
        return;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return;
    }
    m->sbuffer_commit_sqn = value;
}

uint64_t cae_cpu_ooo_sbuffer_commit_sqn(const Object *obj)
{
    if (obj == NULL) {
        return 0;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast((Object *)obj,
                                                    TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return 0;
    }
    return m->sbuffer_commit_sqn;
}

void cae_cpu_ooo_test_seed_sbuffer_eviction_events(Object *obj,
                                                   uint64_t value)
{
    if (obj == NULL) {
        return;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return;
    }
    m->sbuffer_eviction_events = value;
}

uint64_t cae_cpu_ooo_current_store_sqn(Object *obj)
{
    if (obj == NULL) {
        return 0;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return 0;
    }
    return m->store_sqn_next;
}

uint32_t cae_cpu_ooo_rob_count(Object *obj)
{
    if (obj == NULL) {
        return 0;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return 0;
    }
    return m->rob.count;
}

bool cae_cpu_ooo_test_seed_rob_entry(Object *obj, uint64_t pc)
{
    if (obj == NULL) {
        return false;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return false;
    }
    CaeUop uop = {
        .pc       = pc,
        .type     = CAE_UOP_ALU,
        .fu_type  = CAE_FU_ALU,
        .num_dst  = 1,
        .is_branch = false,
    };
    return cae_ooo_rob_dispatch(&m->rob, &uop, /*dispatch_cycle=*/0,
                                /*sbuffer_sqn=*/0);
}

uint32_t cae_cpu_ooo_rat_int_inflight(Object *obj)
{
    if (obj == NULL) {
        return 0;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return 0;
    }
    return m->rat.int_inflight;
}

uint32_t cae_cpu_ooo_rat_fp_inflight(Object *obj)
{
    if (obj == NULL) {
        return 0;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return 0;
    }
    return m->rat.fp_inflight;
}

bool cae_cpu_ooo_test_seed_rat_inflight(Object *obj,
                                        uint32_t int_count,
                                        uint32_t fp_count)
{
    if (obj == NULL) {
        return false;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return false;
    }
    m->rat.int_inflight = int_count;
    m->rat.fp_inflight = fp_count;
    return true;
}

/*
 * Round 49 AC-K-4: concrete rename-map test seam. Allocates a
 * phys id for arch_dst via the production allocator, writes the
 * map, and returns (new_phys, prev_phys). Lets the engine-live-
 * path-restores-rat-map regression drive a real rename-map
 * mutation before / after a live snapshot.
 */
uint16_t cae_cpu_ooo_test_alloc_rat_int(Object *obj, uint8_t arch_dst,
                                        uint16_t *prev_phys_out)
{
    if (obj == NULL) {
        if (prev_phys_out) {
            *prev_phys_out = 0u;
        }
        return 0u;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        if (prev_phys_out) {
            *prev_phys_out = 0u;
        }
        return 0u;
    }
    return cae_ooo_rat_allocate_dst_int(&m->rat, arch_dst, prev_phys_out);
}

/*
 * Round 49: observer for the concrete integer rename map at
 * arch_reg. Returns 0 if obj is not a CaeCpuOoo or arch_reg >= 32.
 */
uint16_t cae_cpu_ooo_rat_map_int_at(Object *obj, uint8_t arch_reg)
{
    if (obj == NULL) {
        return 0u;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return 0u;
    }
    return cae_ooo_rat_map_int(&m->rat, arch_reg);
}

/*
 * Round 49: observer for RAT int free-list count.
 */
uint32_t cae_cpu_ooo_rat_int_free_count(Object *obj)
{
    if (obj == NULL) {
        return 0u;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return 0u;
    }
    return m->rat.int_free_count;
}

bool cae_cpu_ooo_test_scheduler_seed(Object *obj, uint32_t count,
                                     uint8_t fu_type)
{
    if (obj == NULL || count == 0u) {
        return false;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL) {
        return false;
    }
    for (uint32_t i = 0u; i < count; i++) {
        cae_ooo_scheduler_enqueue(&m->scheduler, 0x10000u + i, fu_type);
    }
    return true;
}

bool cae_cpu_ooo_sbuffer_stage_spec_store(Object *obj,
                                          uint64_t addr,
                                          uint16_t size,
                                          uint64_t value)
{
    if (obj == NULL) {
        return false;
    }
    CaeCpuOoo *m = (CaeCpuOoo *)object_dynamic_cast(obj, TYPE_CAE_CPU_OOO);
    if (m == NULL || m->sbuffer == NULL) {
        return false;
    }
    /*
     * Round 39 directive step 4: route wrong-path stores into
     * the sbuffer, never to the memory backend. The sqn is the
     * OoO model's monotonic store counter; allocation at
     * store_sqn_next (pre-increment) guarantees the entry is
     * strictly younger than spec_squash_sqn (captured at save
     * time from the same counter), so the round-32 squash_after
     * path discards exactly these entries during the mispredict
     * resolve. PC=0 because this is a test/harness-injected
     * store without a real instruction PC; callers that want
     * PC attribution should extend the stimulus struct.
     */
    uint64_t sqn = m->store_sqn_next++;
    return cae_sbuffer_alloc((CaeSbuffer *)m->sbuffer, sqn,
                             /*pc=*/0, addr, size, value);
}
