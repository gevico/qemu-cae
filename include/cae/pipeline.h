/*
 * CAE (Cycle Approximate Engine) - Pipeline Stage Abstraction
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_PIPELINE_H
#define CAE_PIPELINE_H

#include "qom/object.h"
#include "qemu/queue.h"
#include "exec/cpu-common.h"
#include "cae/uop.h"
#include "cae/bpred.h"
#include "cae/checkpoint.h"

#define TYPE_CAE_CPU "cae-cpu"

/*
 * Size of the per-CaeCpu active-uop pool (M3' scaffold for the OoO
 * core; in-order uses index 0 only). 64 slots comfortably exceed
 * the kmhv3.py fetch_width (32) so the OoO dispatch path can
 * stage per-FU uops without per-retire allocation while keeping the
 * CaeCpu struct modestly sized.
 */
#define CAE_ACTIVE_UOP_POOL_SIZE 64

typedef struct CaeEngine CaeEngine;
typedef struct CaeUop CaeUop;
typedef struct CaeCpu CaeCpu;
typedef struct CaeCpuClass CaeCpuClass;

/*
 * Round 38: live speculative-memory stimulus descriptor. One
 * entry in the CaeCpu's pending FIFO. `op` mirrors the hook-op
 * encoding cae_mem_access_notify expects (0=READ, 1=WRITE,
 * 2=FETCH) so the drain can forward the value without
 * translation. `bytes` is the natural access width (1, 2, 4, 8);
 * loads ignore `value`, stores use it for trace-value capture
 * (reserved for round 39 sbuffer-side extension — round 38 only
 * exercises loads, so the field is unused by the round-38
 * regression).
 */
typedef struct CaeSpecStimulus {
    uint64_t addr;
    uint32_t bytes;
    uint8_t  op;
    uint64_t value;
} CaeSpecStimulus;

#define CAE_SPEC_STIMULI_MAX 16

DECLARE_OBJ_CHECKERS(CaeCpu, CaeCpuClass, CAE_CPU, TYPE_CAE_CPU)

struct CaeCpuClass {
    ObjectClass parent_class;

    void (*fetch)(CaeCpu *cpu);
    void (*decode)(CaeCpu *cpu);
    void (*dispatch)(CaeCpu *cpu);
    void (*issue)(CaeCpu *cpu);
    void (*execute)(CaeCpu *cpu);
    void (*memory)(CaeCpu *cpu);
    void (*writeback)(CaeCpu *cpu);
    void (*retire)(CaeCpu *cpu);
    void (*flush)(CaeCpu *cpu);
    void (*stall)(CaeCpu *cpu);
};

struct CaeCpu {
    Object parent;

    CaeEngine *engine;
    CPUState *qemu_cpu;
    uint32_t cpu_id;
    uint64_t clock_ratio;

    /*
     * Active UOP pool. `active_uop` points at
     * `active_uop_pool[0]` for the lifetime of the CaeCpu. Keeping
     * the classification persistent across cae_cpu_exec slices is
     * required for AC-11 serial determinism: the RR kick timer
     * fires on QEMU_CLOCK_HOST, breaking cpu_exec into
     * nondeterministic-sized slices, and if active_uop were a
     * stack-local zeroed on every slice entry, the FIRST TB of
     * each new slice would dispatch through HELPER(lookup_tb_ptr)
     * with a blank classification — silently losing the bpred
     * charge for that TB. The pool lives on CaeCpu so the
     * classifier's output survives slice boundaries and every
     * branch's mispredict counts once (AC-11 + AC-K-8).
     *
     * M3' OoO uses `active_uop_pool[0..N-1]` for in-flight uops;
     * the in-order track keeps using index 0 exclusively, so its
     * charge path and determinism-check semantics are bit-identical
     * pre- and post-refactor (AC-K-8). N sized to comfortably
     * exceed the kmhv3.py fetch_width (32) so the OoO core can
     * stage per-FU uops without per-retire allocation.
     */
    CaeUop *active_uop;
    CaeUop active_uop_pool[CAE_ACTIVE_UOP_POOL_SIZE];

    /* Statistics */
    uint64_t cycle_count;
    uint64_t insn_count;
    uint64_t stall_cycles;
    uint64_t tlb_miss_count;

    /*
     * AC-K-10 first-PC latch. Captures the architectural PC of the
     * first retired instruction observed on this CaeCpu (typically
     * the reset vector). Exposed as `first-pc` QOM property so
     * run-xs-suite.sh can cross-check against NEMU's first trace
     * record and the manifest's `reset_pc` before invoking any
     * timing gate. The latch is one-shot per CaeCpu lifetime —
     * subsequent retirements do not overwrite it.
     *
     * `first_pc_latched` guards the write. Both fields start zeroed
     * at cae_cpu_init so a pre-realize qom-get returns 0 which the
     * suite treats as "not-yet-observed / skip".
     */
    uint64_t first_pc;
    bool first_pc_latched;

    /*
     * AC-K-3.3 / AC-K-5: instruction-fetch counter. Incremented by
     * cae_mem_access_notify on every CAE_MEM_HOOK_FETCH firing
     * (wired from accel/tcg/cputlb.c's
     * get_page_addr_code_hostp). The M4' I-cache model (when it
     * lands) consumes this counter for its demand-fetch accounting;
     * in the interim it serves as a liveness probe for the fetch
     * hook.
     */
    uint64_t insn_fetch_count;

    /*
     * Per-CPU branch-predictor attribution. `bpred_predictions` and
     * `bpred_mispredictions` are written directly by the engine's
     * charge path on every retired branch; btb/ras counters are not
     * stored here — they are sampled lazily from engine->bpred at
     * QMP-get time. Lazy sampling keeps the hot path off-the-clock
     * for those counters while preserving single-core correctness
     * (engine->bpred is shared, so its counters are this CPU's
     * counters). Round M4's multi-core expansion will turn the lazy
     * getters into proper per-CPU storage.
     */
    uint64_t bpred_predictions;
    uint64_t bpred_mispredictions;

    /*
     * BS-38 round-16: split predict/update across retires so
     * a decoupled-BPU FTQ actually runs ahead. Round 15's
     * branch handler called predict() + update() back-to-back
     * in the same charge call, so FTQ occupancy always
     * returned to zero between retires and the queue layer
     * was inert.
     *
     * The handler now stages the current branch's resolve
     * here and delays the update() call to the NEXT branch
     * retire. FTQ occupancy steady-state: 1 (between
     * retires) / 2 (transient, during a retire after the
     * push but before the delayed update pops).
     *
     * `bpred_pending_valid` distinguishes "no prior branch
     * retired yet" (first branch of a run, or after a
     * cpu_init clear) from "pending slot holds a real
     * resolve". Cleared by cae_cpu_init so a fresh CaeCpu
     * never sees a ghost pending carried over from a prior
     * benchmark.
     */
    CaeBPredResolve bpred_pending_resolve;
    bool bpred_pending_valid;

    /*
     * Round 31 live speculation slot (t-tcg-spec-path). Holds one
     * in-flight checkpoint snapshot across a predicted-path
     * retire. Lifecycle:
     *   - HELPER(lookup_tb_ptr), after frontend-predict fires for
     *     a branch: cae_checkpoint_save() populates `spec_snap`,
     *     sets `spec_snap_valid=true`, captures the pre-TB
     *     `store_sqn_next` as `spec_squash_sqn`, and stashes the
     *     frontend prediction metadata in `spec_predicted`.
     *   - cae_charge_executed_tb() at branch retire: compares the
     *     stashed prediction against the actual resolve; on
     *     mispredict restore+squash_after+drop, on correct
     *     drop; either way clears the slot.
     *   - Exit paths (cpu_init, sentinel freeze, bpred
     *     flush-pending, teardown): invoke
     *     cae_cpu_spec_slot_drop_if_live() exactly once.
     *
     * `spec_snap_valid` is the source of truth for "live slot";
     * `spec_snap` may be NULL even when `spec_snap_valid` would
     * otherwise be true IF no target emitter is registered —
     * the save path then leaves `spec_snap_valid=false` so the
     * resolve block is a no-op.
     */
    CaeCheckpointSnapshot *spec_snap;
    bool spec_snap_valid;
    uint64_t spec_squash_sqn;
    CaeBPredPrediction spec_predicted;

    /*
     * Per-CPU load/store attribution written by cae_mem_access_notify
     * when the memory backend reports hit/miss on this access.
     */
    uint64_t l1d_hits;
    uint64_t l1d_misses;
    uint64_t memory_stall_cycles;

    /*
     * Load-only L1D accounting. `avg_load_latency` on the gate
     * contract is per-LOAD, not per-data-access, so the plan's AC-4
     * metric can only be computed from counters that exclude
     * stores and AMO writes. Stores still contribute to the
     * all-data l1d_hits / l1d_misses counters above; these three
     * split the load half out separately.
     */
    uint64_t load_hits;
    uint64_t load_misses;
    uint64_t load_stall_cycles;

    /*
     * Round 38 live speculative-memory stimulus seam (directive
     * step 1 from Codex's round-37 review). QEMU TCG never
     * executes a wrong-path load/store functionally, so the
     * live AC-K-4 memory proof surface (plan.md:85, plan.md:87)
     * needs an explicit mechanism for tests and benchmark
     * harnesses to push speculative memory requests through
     * `cae_mem_access_notify()` while `spec_snap_valid == true`.
     *
     * Mechanism: a bounded FIFO owned by the CaeCpu. Callers
     * queue `CaeSpecStimulus` entries via
     * `cae_cpu_queue_spec_stimulus` at any time. Immediately
     * after `cae_checkpoint_save()` sets `spec_snap_valid`
     * (the TB-entry call-site in cpu-exec.c:HELPER(lookup_tb_ptr),
     * or a direct `cae_cpu_drain_spec_stimuli` call in unit
     * tests), the drain iterates the FIFO and issues one
     * `cae_mem_access_notify` per stimulus. The notify path
     * already stamps `req.speculative = spec_snap_valid`, so
     * the downstream cache gates (round-34 miss fill,
     * round-35 hit-path LRU) fire on a real engine-driven
     * request, not a synthetic unit-only shortcut.
     *
     * `spec_stimuli_count` is the pending depth; cleared to 0
     * after each drain. `spec_stimuli_drained` is a test-
     * observable accumulator (monotonic; advances by the
     * number of stimuli fired on each drain call). The cap
     * is small by design — round 38 only needs a couple of
     * entries for the unit proofs; a larger buffer would
     * require rethinking the drain-vs-exit-path order.
     */
    CaeSpecStimulus spec_stimuli[CAE_SPEC_STIMULI_MAX];
    uint8_t spec_stimuli_count;
    uint32_t spec_stimuli_drained;
    /*
     * Round 40: monotonic count of stimuli that the drain
     * could not forward to their backend dispatcher (WRITE
     * into a full sbuffer, WRITE on an engine whose cpu_model
     * is NULL or wrong-typed). The queue is still cleared on
     * drain (stimuli expire with the live window whether
     * accepted or rejected), but keeping the reject count
     * separate from `spec_stimuli_drained` lets harness runs
     * and tests distinguish "drain fired N" from "drain saw
     * N+M stimuli, M dropped at dispatch". Same monotonic-
     * across-clear semantics as `spec_stimuli_drained`
     * (Corollary G rule #2).
     */
    uint32_t spec_stimuli_rejected;

    QTAILQ_ENTRY(CaeCpu) next;
};

/* CaeCpu lifecycle */
void cae_cpu_init(CaeCpu *cpu, CaeEngine *engine, CPUState *qemu_cpu,
                  uint32_t cpu_id);

/* Timing */
void cae_cpu_advance(CaeCpu *cpu, uint64_t cycles);

/*
 * Target-side plug-in flag. Each target that wires CAE CPU realization
 * into its AccelCPUClass must call cae_accel_cpu_mark_registered()
 * from its type_init() constructor. Arch-neutral accel init uses
 * cae_accel_cpu_is_registered() together with cae_uop_has_classifier()
 * to decide whether to accept -accel cae for the current binary.
 */
void cae_accel_cpu_mark_registered(void);
bool cae_accel_cpu_is_registered(void);

/*
 * Pre-exec hook (AC-K-10). Registered once from the target's
 * type_init() (target/<arch>/cae/cae-<arch>-cpu.c). Invoked by
 * arch-neutral cae_cpu_exec BEFORE cpu_exec() so that
 * active_uop->pc reflects the CPU's current architectural PC on
 * the FIRST retire of the first cpu_exec slice. Without this, the
 * first HELPER(lookup_tb_ptr) call fires the retire charge with
 * active_uop->pc = 0 (the init value), and the first on-disk
 * trace record / the /first-pc/ QOM latch end up capturing the
 * SECOND retired TB's PC instead of the first. The hook is a
 * no-op when active_uop->pc is already non-zero (subsequent
 * slices per BL-20260418-active-uop-persist-across-slices).
 */
typedef void (*CaeCpuPrepForExecFn)(CaeCpu *cpu);
void cae_cpu_register_prep_for_exec(CaeCpuPrepForExecFn fn);
void cae_cpu_prep_for_exec(CaeCpu *cpu);

/*
 * AC-K-10 first-PC observe point (round 6). Called from
 * accel/tcg/cpu-exec.c at the two TB-boundary sites (the
 * HELPER(lookup_tb_ptr) classify path and the post-loop
 * cpu_loop_exec_tb exit path) with the INCOMING TB's PC. Latches
 * cpu->first_pc on the first call where pc >= engine->trace_start_pc
 * and pc != 0. Also declared in include/cae/cae-mem-hook.h for
 * cpu-exec.c's TCG-side call site to avoid pulling the full
 * cae/pipeline.h tree into the TCG compilation unit.
 */
void cae_first_pc_observe(CaeCpu *cpu, uint64_t pc);

/* ------------------------------------------------------------------ */
/*  Speculation save/restore (CaeCpu-side blob)                       */
/* ------------------------------------------------------------------ */

/*
 * Arch-neutral snapshot of the CaeCpu lane that the M4' speculation
 * code path needs to unwind on a mispredict. Captures the pending-
 * resolve slot (bpred_pending_resolve / bpred_pending_valid) and
 * the active CaeUop's prediction cache (pred_valid / pred_taken /
 * pred_target / pred_target_known) at save time. The struct is
 * opaque to callers; the definition lives in cae/pipeline.c.
 *
 * Save returns NULL on `cpu == NULL`. Restore is a no-op on
 * NULL snap or NULL cpu. If the snapshot was taken with an active
 * uop but restore is called when cpu->active_uop is NULL (e.g. the
 * original uop was already freed between save and restore), the
 * prediction-cache restore is skipped; the pending-resolve slot
 * is still written back unconditionally.
 */
typedef struct CaeCpuSpecSnapshot CaeCpuSpecSnapshot;

CaeCpuSpecSnapshot *cae_cpu_spec_snapshot_save(CaeCpu *cpu);
void cae_cpu_spec_snapshot_restore(CaeCpu *cpu,
                                   const CaeCpuSpecSnapshot *snap);
void cae_cpu_spec_snapshot_drop(CaeCpuSpecSnapshot *snap);

/*
 * Round 31: drop any live checkpoint snapshot stashed on `cpu`
 * and clear the slot. NULL-safe + idempotent — safe to call from
 * every exit path that might leave a snap orphaned (cpu_init,
 * sentinel freeze, bpred flush-pending, teardown). No-op when
 * `cpu` is NULL or no live snap is stashed. After this call,
 * `cpu->spec_snap_valid == false` and `cpu->spec_snap == NULL`.
 */
void cae_cpu_spec_slot_drop_if_live(CaeCpu *cpu);

/*
 * Round 38 live speculative-memory stimulus seam (see CaeCpu
 * field-block comment above). All three functions are NULL-safe
 * (no-op on `cpu == NULL`).
 *
 * `cae_cpu_queue_spec_stimulus`: FIFO append. When the queue is
 * full, the stimulus is silently dropped and `spec_stimuli_count`
 * remains at `CAE_SPEC_STIMULI_MAX`. Callers that care about
 * overflow should observe `spec_stimuli_count` before queueing.
 *
 * `cae_cpu_clear_spec_stimuli`: reset `spec_stimuli_count` to 0
 * without changing `spec_stimuli_drained`. Intended for test
 * cleanup between regressions and for the squash path to
 * guarantee no stale stimuli survive across snapshots.
 *
 * `cae_cpu_drain_spec_stimuli`: defined in cae/engine.c (it
 * calls `cae_mem_access_notify` and the OoO sbuffer stage
 * helper, both engine-layer). Scoped to the live window:
 * when `cpu->spec_snap_valid == false` it is a no-op that
 * returns 0 and leaves the queue untouched.
 *
 * Op-dispatch (round 39): READ / FETCH route through
 * `cae_mem_access_notify`; WRITE routes through
 * `cae_cpu_ooo_sbuffer_stage_spec_store` (sbuffer path).
 *
 * Accept / reject accounting (round 40): returns the number
 * of stimuli ACCEPTED by their backend dispatcher. READ /
 * FETCH always count as accepted (notify never fails). WRITE
 * counts as accepted only when the sbuffer stage returned
 * true; otherwise it counts against `spec_stimuli_rejected`.
 * The queue is cleared on EVERY drain, whether accepted or
 * rejected, because stimuli semantically expire with the live
 * window. `spec_stimuli_drained` advances by the accepted
 * count; `spec_stimuli_rejected` advances by the rejected
 * count.
 */
void cae_cpu_queue_spec_stimulus(CaeCpu *cpu, const CaeSpecStimulus *s);
void cae_cpu_clear_spec_stimuli(CaeCpu *cpu);
uint32_t cae_cpu_drain_spec_stimuli(CaeCpu *cpu);

#endif /* CAE_PIPELINE_H */
