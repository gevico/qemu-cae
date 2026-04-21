/*
 * CAE (Cycle Approximate Engine) - Event-Driven Engine
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_ENGINE_H
#define CAE_ENGINE_H

#include "qom/object.h"
#include "qemu/queue.h"

#define TYPE_CAE_ENGINE "cae-engine"

typedef struct CaeCpu CaeCpu;
typedef struct CaeEvent CaeEvent;
typedef struct CaeEngine CaeEngine;

DECLARE_INSTANCE_CHECKER(CaeEngine, CAE_ENGINE, TYPE_CAE_ENGINE)

struct CaeEngine {
    Object parent;

    uint64_t current_cycle;
    uint32_t tlb_miss_cycles;
    GTree *event_queue;
    /*
     * Monotonically-increasing sequence number assigned to each event
     * at schedule time; used as the deterministic tie-breaker when
     * two events share a cycle. Written only from the scheduling
     * path (single-threaded during event enqueue).
     */
    uint64_t next_event_seq;
    QTAILQ_HEAD(, CaeCpu) cpus;

    /* Memory backend (implements CaeMemClass) */
    Object *mem_backend;

    /*
     * Round 18 t-icache: separate instruction-cache backend.
     * When attached, CAE_MEM_HOOK_FETCH hooks are routed here
     * instead of to mem_backend, so the I-cache models fetch
     * timing independently of the D-cache's read/write path.
     * NULL means fall back to mem_backend (round-9 behaviour
     * where fetches shared the D-cache).
     */
    Object *icache_backend;

    /*
     * CPU timing model (implements CaeCpuModelClass). NULL means the
     * engine falls back to Phase-1 CPI=1 in cae_charge_executed_tb.
     */
    Object *cpu_model;

    /*
     * Branch predictor (implements CaeBPredClass). NULL means branches
     * cost their base latency without mispredict penalties.
     */
    Object *bpred;

    /*
     * Optional sentinel-write freeze for AC-11 serial determinism.
     * When `sentinel_addr` is non-zero and a guest store targets
     * it, the memory hook flips `counters_frozen` to true and the
     * hot-path charge + warp-idle paths become no-ops thereafter.
     * Purpose: stop the reported counter values from drifting while
     * the benchmark's post-sentinel halt loop keeps retiring
     * (wfi + j halt). Before the freeze, cont/stop polling sampled
     * halt-loop iterations nondeterministically; after the freeze,
     * every sampling point returns identical counters.
     */
    uint64_t sentinel_addr;
    /*
     * AC-K-10 trace + first-PC lower bound. When non-zero, the
     * retire hook skips first-PC latching and trace emission for
     * any uop whose pc is below this value. Set from the accel
     * `trace-start-pc=<pc>` property — typically the benchmark
     * entry PC from MANIFEST.reset_pc, matching NEMU's direct-boot
     * start so the CAE-side trace aligns with NEMU's first record.
     */
    uint64_t trace_start_pc;
    bool counters_frozen;
    /*
     * Round 47 AC-K-2.4 byte-identity: one-shot gate that lets the
     * sentinel-store's OWN retire-charge complete before
     * counters_frozen takes effect. The softmmu hook at
     * cae_mem_access_notify detects the sentinel write DURING the
     * TB body and used to flip counters_frozen to true in the same
     * callback; cae_charge_executed_tb's early-return guard then
     * swallowed the sentinel store's own retire record, leaving CAE
     * one record short of NEMU at byte-diff tail time. Instead, the
     * hook now sets this pending flag, then cae_charge_executed_tb
     * promotes it to counters_frozen AFTER the current emit. Exactly
     * one more retire record lands (the sentinel store itself);
     * all subsequent charges early-return as before.
     */
    bool freeze_pending;
    /*
     * Diagnostic: count of post-freeze charge attempts + post-freeze
     * notify attempts. Exposed on the engine's QOM surface to catch
     * remaining deterministic-mutation leaks when
     * `determinism-check.sh` fails. Should stay at whatever both
     * runs compute identically.
     */
    uint64_t frozen_charge_calls;
    uint64_t frozen_notify_calls;

    /* QOM properties */
    uint32_t num_cpus;
    uint64_t base_freq_hz;
    uint32_t sync_interval;

    /*
     * Round 41 harness promotion (directive step 5): a
     * deterministic config string that auto-queues
     * speculative-memory stimuli on every live-window drain.
     * Format: "<op>:<addr>:<bytes>[:<value>][;...]" where
     * <op> is r / R / w / W / f / F for READ / WRITE / FETCH,
     * addr and value are 0x-prefixed hex, and bytes is
     * decimal 1/2/4/8. Example:
     *   "r:0x1000:8;w:0x2000:8:0xdeadbeef"
     *
     * NULL or empty string means "no auto-queue", which is
     * the default and matches round-40 behavior. Tests that
     * pre-populate the per-CPU FIFO manually override the
     * program for that single drain (round 41 Corollary J).
     *
     * Ownership: the setter validates via the parser and
     * duplicates the incoming string; instance_finalize frees
     * it.
     */
    gchar *spec_stimulus_program;
};

typedef struct CaeEvent {
    uint64_t cycle;
    /*
     * Assigned by cae_engine_schedule_event() from
     * engine->next_event_seq++. Used only as a deterministic
     * tie-breaker in cae_event_compare() when two events share
     * a cycle. Callers should not set this directly.
     */
    uint64_t seq;
    void (*handler)(void *opaque);
    void *opaque;
    QTAILQ_ENTRY(CaeEvent) next;
} CaeEvent;

/* Engine lifecycle */
void cae_engine_finalize(CaeEngine *engine);

/* Event queue operations */
bool cae_engine_schedule_event(CaeEngine *engine, CaeEvent *event);
CaeEvent *cae_engine_pop_event(CaeEngine *engine);
void cae_engine_process_events(CaeEngine *engine, uint64_t until_cycle);

/* Cycle management */
void cae_engine_advance_cycle(CaeEngine *engine, uint64_t cycles);
void cae_engine_sync_virtual_clock(CaeEngine *engine);

/*
 * Warp the engine clock forward to target_cycle during an idle
 * interval (no vCPU is executing instructions). Advances
 * engine->current_cycle and adds the same delta to every registered
 * CaeCpu's cycle_count and stall_cycles, so per-CPU timing counters
 * stay consistent with the engine clock across idle deadline jumps
 * (wfi waiting on stimecmp / vstimecmp / PMU). No-op when the target
 * does not exceed the current engine cycle.
 */
void cae_engine_warp_idle(CaeEngine *engine, uint64_t target_cycle);

/* Overflow-safe cycle-to-nanosecond conversion */
static inline int64_t cae_cycles_to_ns(uint64_t cycles, uint64_t freq_hz)
{
    if (freq_hz == 0) {
        return 0;
    }
    /*
     * Avoid overflow: cycles * 1e9 can exceed uint64 at ~18.4s (1GHz).
     * Split into quotient and remainder to stay within 64-bit range.
     */
    uint64_t sec = cycles / freq_hz;
    uint64_t rem = cycles % freq_hz;
    return (int64_t)(sec * 1000000000ULL + rem * 1000000000ULL / freq_hz);
}

/* CPU registration */
void cae_engine_register_cpu(CaeEngine *engine, CaeCpu *cpu);

/* Memory backend attachment */
bool cae_engine_set_mem_backend(CaeEngine *engine, Object *backend, Error **errp);

/*
 * Round 18 t-icache: attach a separate I-cache backend. When
 * set, CAE_MEM_HOOK_FETCH routes here; CAE_MEM_HOOK_READ /
 * _WRITE continue to route to mem_backend. Passing NULL clears
 * the I-cache and restores the round-9 shared-cache fallback.
 * Validates the object implements CaeMemClass (same shape as
 * cae_engine_set_mem_backend).
 */
bool cae_engine_set_icache_backend(CaeEngine *engine, Object *backend,
                                   Error **errp);

/* CPU model attachment (implementer of CaeCpuModelClass). */
bool cae_engine_set_cpu_model(CaeEngine *engine, Object *model, Error **errp);

/* Branch predictor attachment (implementer of CaeBPredClass). */
bool cae_engine_set_bpred(CaeEngine *engine, Object *bpred, Error **errp);

/*
 * Round 17 drift-recovery: frontend-side predict hook.
 *
 * Called from HELPER(lookup_tb_ptr) at TB entry, immediately
 * after the classifier populates cpu->active_uop. When the
 * incoming TB's first insn is a branch AND an engine-level
 * predictor is attached, this hook queries the predictor
 * (pushing into a DecoupledBPU FTQ if one is wrapping the
 * inner predictor) and stashes the prediction into
 * active_uop->pred_* fields. The retire path
 * (cae_charge_executed_tb) consumes those fields for
 * mispredict detection instead of calling predict() again.
 *
 * Safe to call when engine->bpred is NULL or active_uop is
 * not a branch — the call becomes a no-op. This is why the
 * TCG call site does not need to pre-check; it just calls
 * unconditionally when cae_allowed is true.
 */
void cae_engine_on_frontend_predict(CaeEngine *engine, CaeCpu *cpu);

/*
 * Round 17 drift-recovery: end-of-run drain for any branch
 * resolve still parked in cpu->bpred_pending_*.
 *
 * The round-16 pending-slot design defers update() by one
 * retire so the FTQ actually carries in-flight predictions.
 * If a benchmark ends with a pending resolve (natural, since
 * the final branch's update fires on the NEXT retire which
 * never happens), that pending entry leaks unless an explicit
 * drain runs. Called from sentinel-freeze on AC-11 benchmark
 * shutdown and on CAE_SET_CURRENT_CPU(NULL) teardown paths.
 *
 * Safe to call when bpred_pending_valid is already false or
 * when engine->bpred is NULL.
 */
void cae_engine_bpred_flush_pending(CaeEngine *engine, CaeCpu *cpu);

/* Per-instruction execution context (set during cae_cpu_exec) */
void cae_set_current_cpu(CaeCpu *cpu);
CaeCpu *cae_get_current_cpu(void);

/* Global engine instance */
CaeEngine *cae_get_engine(void);

/*
 * Round 19 t-mem-async-iface: return the engine's current global
 * cycle via atomic read. Returns 0 when no engine is registered
 * (unit tests without -accel cae, or early boot before
 * cae_init_machine). Timing backends (cache_mshr) use this to expire
 * outstanding completions against the global clock when inspected
 * outside the request dispatch path (e.g. can_accept()).
 */
uint64_t cae_engine_current_cycle(void);

/* Find CaeCpu associated with a QEMU CPUState */
CaeCpu *cae_engine_find_cpu(CaeEngine *engine, CPUState *qemu_cpu);

/*
 * cae_allowed is declared in include/cae/cae-mem-hook.h, which is the
 * lightweight header that cputlb.c / cpu-exec.c include without
 * pulling in QOM. Callers that already include cae-mem-hook.h pick up
 * the declaration automatically; engine.h does not redeclare it.
 */

#endif /* CAE_ENGINE_H */
