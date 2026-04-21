/*
 * CAE (Cycle Approximate Engine) - Pipeline Base Class
 *
 * CaeCpu base class with virtual method table for pipeline stages
 * and statistics counters.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "cae/pipeline.h"
#include "cae/engine.h"
#include "cae/uop.h"
#include "cae/bpred.h"

/*
 * Flag set by target-side type_init() constructors after they register
 * a CAE AccelCPUClass. Read by the arch-neutral accel gate to prove
 * that CAE CPU realization is wired up for the current guest target.
 * Written once during QOM type registration (single-threaded); read
 * later on the main thread, so no synchronisation is needed.
 */
static bool accel_cpu_registered;

void cae_accel_cpu_mark_registered(void)
{
    accel_cpu_registered = true;
}

bool cae_accel_cpu_is_registered(void)
{
    return accel_cpu_registered;
}

/*
 * AC-K-10 pre-exec hook registry. Single writer (per-binary target's
 * type_init constructor), many readers (cae_cpu_exec on every slice
 * entry). The hook is invoked with the CaeCpu whose active_uop->pc
 * still reads zero — i.e. the very first slice of the session — so
 * it can fill in the architectural reset PC. Subsequent slices leave
 * active_uop->pc non-zero per BL-20260418-active-uop-persist-across-
 * slices and the hook short-circuits.
 */
static CaeCpuPrepForExecFn g_prep_for_exec_fn;

void cae_cpu_register_prep_for_exec(CaeCpuPrepForExecFn fn)
{
    g_prep_for_exec_fn = fn;
}

void cae_cpu_prep_for_exec(CaeCpu *cpu)
{
    if (!g_prep_for_exec_fn || !cpu) {
        return;
    }
    g_prep_for_exec_fn(cpu);
}

void cae_cpu_init(CaeCpu *cpu, CaeEngine *engine, CPUState *qemu_cpu,
                  uint32_t cpu_id)
{
    cpu->engine = engine;
    cpu->qemu_cpu = qemu_cpu;
    cpu->cpu_id = cpu_id;
    cpu->clock_ratio = 1;
    cpu->cycle_count = 0;
    cpu->insn_count = 0;
    cpu->stall_cycles = 0;
    cpu->bpred_predictions = 0;
    cpu->bpred_mispredictions = 0;
    cpu->l1d_hits = 0;
    cpu->l1d_misses = 0;
    cpu->memory_stall_cycles = 0;
    cpu->load_hits = 0;
    cpu->load_misses = 0;
    cpu->load_stall_cycles = 0;
    /*
     * BS-38 round-16: clear the pending-bpred-resolve slot so a
     * fresh CaeCpu never inherits a ghost pending from a prior
     * benchmark run. `bpred_pending_valid=false` means the next
     * branch retire has nothing to drain — the lag is primed
     * starting from the first observed branch.
     */
    memset(&cpu->bpred_pending_resolve, 0,
           sizeof(cpu->bpred_pending_resolve));
    cpu->bpred_pending_valid = false;
    /*
     * Round 31: clear any inherited live spec slot. Use the
     * drop-if-live helper so a zombie snapshot from a prior
     * benchmark is freed through the owning target emitter
     * before the slot is cleared. On a fresh CaeCpu the fields
     * are zero (via the QOM instance init), so the helper's
     * NULL-safe no-op path runs.
     */
    cae_cpu_spec_slot_drop_if_live(cpu);
    memset(&cpu->spec_predicted, 0, sizeof(cpu->spec_predicted));
    cpu->spec_squash_sqn = 0;
    /*
     * Round 38: clear the live speculative-memory stimulus FIFO
     * so a fresh CaeCpu starts with an empty queue. The drained
     * counter is zeroed alongside; tests observe both.
     * Round 40: `spec_stimuli_rejected` joins the same init
     * family — monotonic counter paired with `_drained`.
     */
    memset(cpu->spec_stimuli, 0, sizeof(cpu->spec_stimuli));
    cpu->spec_stimuli_count = 0;
    cpu->spec_stimuli_drained = 0;
    cpu->spec_stimuli_rejected = 0;
    memset(cpu->active_uop_pool, 0, sizeof(cpu->active_uop_pool));
    cpu->active_uop = &cpu->active_uop_pool[0];
}

void cae_cpu_advance(CaeCpu *cpu, uint64_t cycles)
{
    qatomic_set(&cpu->cycle_count,
                    qatomic_read(&cpu->cycle_count) + cycles);
    qatomic_set(&cpu->insn_count,
                    qatomic_read(&cpu->insn_count) + 1);
}

/*
 * Per-CPU timing counters are written on the vCPU thread
 * (cae_cpu_advance, cae_charge_executed_tb, cae_engine_warp_idle,
 * cae_mem_access_notify) and read on the I/O thread via QMP qom-get.
 * On 32-bit hosts plain 64-bit loads tear, so route every QMP read
 * through qatomic_read. The field address is passed via opaque,
 * resolved at registration time in cae_cpu_instance_init().
 */
static void cae_cpu_get_u64_atomic(Object *obj, Visitor *v,
                                   const char *name,
                                   void *opaque, Error **errp)
{
    uint64_t value = qatomic_read((uint64_t *)opaque);
    visit_type_uint64(v, name, &value, errp);
}

/*
 * Lazy per-CPU predictor-stat getter. Reads the bound engine's bpred
 * object at QMP-get time so the value always reflects the live
 * counter without any hot-path mirror write. For single-core (M2)
 * the engine's bpred is shared across all CPUs, so per-CPU view ==
 * global view; M4 multi-core will replace this with per-CPU storage.
 * opaque is the property name on the bpred object, a static string.
 */
static void cae_cpu_get_bpred_stat(Object *obj, Visitor *v,
                                   const char *name,
                                   void *opaque, Error **errp)
{
    CaeCpu *cpu = CAE_CPU(obj);
    const char *prop_name = (const char *)opaque;
    uint64_t value = 0;

    if (cpu->engine && cpu->engine->bpred && prop_name) {
        Error *local_err = NULL;
        value = object_property_get_uint(cpu->engine->bpred,
                                         prop_name, &local_err);
        if (local_err) {
            /*
             * Missing property on the bpred object (e.g. bpred-model
             * was set to 'none' and the installed object has a
             * different stat surface): silently return 0. The
             * config-equivalence gate catches type mismatches at
             * setup; a missing-prop-at-sample is not a correctness
             * bug, just a "bpred doesn't expose that stat" signal.
             */
            error_free(local_err);
            value = 0;
        }
    }
    visit_type_uint64(v, name, &value, errp);
}

/* Default no-op pipeline stage implementations */
static void cae_cpu_nop_stage(CaeCpu *cpu)
{
    /* No-op: to be overridden by concrete pipeline implementations */
}

static void cae_cpu_instance_init(Object *obj)
{
    CaeCpu *cpu = CAE_CPU(obj);
    cpu->engine = NULL;
    cpu->qemu_cpu = NULL;
    cpu->cpu_id = 0;
    cpu->clock_ratio = 1;
    cpu->cycle_count = 0;
    cpu->insn_count = 0;
    cpu->stall_cycles = 0;
    cpu->bpred_predictions = 0;
    cpu->bpred_mispredictions = 0;
    cpu->l1d_hits = 0;
    cpu->l1d_misses = 0;
    cpu->memory_stall_cycles = 0;
    cpu->load_hits = 0;
    cpu->load_misses = 0;
    cpu->load_stall_cycles = 0;
    cpu->first_pc = 0;
    cpu->first_pc_latched = false;
    cpu->insn_fetch_count = 0;

    /*
     * Expose per-CPU timing counters as read-only QOM properties so
     * integration tests can read them through QMP qom-get after
     * execution. The CPU instance is parented under the engine
     * (/objects/cae-engine/cpuN) by cae_engine_register_cpu().
     */
    object_property_add(obj, "cycle-count", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->cycle_count);
    object_property_add(obj, "insn-count", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->insn_count);
    object_property_add(obj, "stall-cycles", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->stall_cycles);
    object_property_add(obj, "bpred-predictions", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->bpred_predictions);
    object_property_add(obj, "bpred-mispredictions", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->bpred_mispredictions);
    object_property_add(obj, "btb-hits", "uint64",
                        cae_cpu_get_bpred_stat,
                        NULL, NULL, (void *)"btb-hits");
    object_property_add(obj, "btb-misses", "uint64",
                        cae_cpu_get_bpred_stat,
                        NULL, NULL, (void *)"btb-misses");
    object_property_add(obj, "ras-pushes", "uint64",
                        cae_cpu_get_bpred_stat,
                        NULL, NULL, (void *)"ras-pushes");
    object_property_add(obj, "ras-pops", "uint64",
                        cae_cpu_get_bpred_stat,
                        NULL, NULL, (void *)"ras-pops");
    object_property_add(obj, "l1d-hits", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->l1d_hits);
    object_property_add(obj, "l1d-misses", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->l1d_misses);
    object_property_add(obj, "memory-stall-cycles", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->memory_stall_cycles);
    object_property_add(obj, "load-hits", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->load_hits);
    object_property_add(obj, "load-misses", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->load_misses);
    object_property_add(obj, "load-stall-cycles", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->load_stall_cycles);
    /*
     * Round 41 directive step 5: QMP-visible accept / reject
     * counters for the speculative-memory stimulus seam.
     * `spec-stimuli-drained` is the monotonic count of
     * stimuli actually fired by their backend dispatcher
     * (round 40 accept accounting); `spec-stimuli-rejected`
     * is the monotonic count of stimuli dropped at dispatch
     * (sbuffer full / missing cpu_model on WRITE). Harness
     * runs read both through qom-get to report the same
     * accept/reject semantics the unit tests pin.
     */
    object_property_add_uint32_ptr(obj, "spec-stimuli-drained",
                                   &cpu->spec_stimuli_drained,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "spec-stimuli-rejected",
                                   &cpu->spec_stimuli_rejected,
                                   OBJ_PROP_FLAG_READ);
    /*
     * AC-K-10 first-PC latch. Plain pointer getter — the field is
     * only written by cae_charge_executed_tb on the one-shot latch
     * branch, so a non-atomic read is fine for QMP sampling.
     */
    object_property_add(obj, "first-pc", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->first_pc);
    object_property_add(obj, "insn-fetch-count", "uint64",
                        cae_cpu_get_u64_atomic,
                        NULL, NULL, &cpu->insn_fetch_count);
}

static void cae_cpu_class_init(ObjectClass *oc, const void *data)
{
    CaeCpuClass *cc = CAE_CPU_CLASS(oc);

    cc->fetch = cae_cpu_nop_stage;
    cc->decode = cae_cpu_nop_stage;
    cc->dispatch = cae_cpu_nop_stage;
    cc->issue = cae_cpu_nop_stage;
    cc->execute = cae_cpu_nop_stage;
    cc->memory = cae_cpu_nop_stage;
    cc->writeback = cae_cpu_nop_stage;
    cc->retire = cae_cpu_nop_stage;
    cc->flush = cae_cpu_nop_stage;
    cc->stall = cae_cpu_nop_stage;
}

static const TypeInfo cae_cpu_type = {
    .name = TYPE_CAE_CPU,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeCpu),
    .class_size = sizeof(CaeCpuClass),
    .instance_init = cae_cpu_instance_init,
    .class_init = cae_cpu_class_init,
    .abstract = false,
};

static void cae_cpu_register_types(void)
{
    type_register_static(&cae_cpu_type);
}
type_init(cae_cpu_register_types);

/* ------------------------------------------------------------------ */
/*  Speculation save/restore: CaeCpu lane                             */
/* ------------------------------------------------------------------ */

/*
 * Private struct definition — opaque to callers. Carries the
 * CaeCpu state the M4' speculation path needs to unwind on a
 * mispredict. `has_active_uop_pred` distinguishes "no active uop
 * at save time" (no pred cache to restore) from "active uop
 * existed but all pred fields happened to be zero" (still a
 * meaningful restore target). The active_uop pointer itself is
 * NOT saved — it may be freed between save and restore;
 * restore attaches to whichever uop is current at restore time,
 * which aligns with the speculation contract (the restore
 * happens at retire of the mispredicted branch, when the active
 * uop is the same uop whose predict cache was stashed).
 */
struct CaeCpuSpecSnapshot {
    CaeBPredResolve bpred_pending_resolve;
    bool bpred_pending_valid;
    bool has_active_uop_pred;
    bool pred_valid;
    bool pred_taken;
    bool pred_target_known;
    uint64_t pred_target;
};

CaeCpuSpecSnapshot *cae_cpu_spec_snapshot_save(CaeCpu *cpu)
{
    if (cpu == NULL) {
        return NULL;
    }
    CaeCpuSpecSnapshot *snap = g_new0(CaeCpuSpecSnapshot, 1);
    snap->bpred_pending_resolve = cpu->bpred_pending_resolve;
    snap->bpred_pending_valid = cpu->bpred_pending_valid;
    if (cpu->active_uop != NULL) {
        const CaeUop *uop = cpu->active_uop;
        snap->has_active_uop_pred = true;
        snap->pred_valid = uop->pred_valid;
        snap->pred_taken = uop->pred_taken;
        snap->pred_target_known = uop->pred_target_known;
        snap->pred_target = uop->pred_target;
    }
    return snap;
}

void cae_cpu_spec_snapshot_restore(CaeCpu *cpu,
                                   const CaeCpuSpecSnapshot *snap)
{
    if (cpu == NULL || snap == NULL) {
        return;
    }
    cpu->bpred_pending_resolve = snap->bpred_pending_resolve;
    cpu->bpred_pending_valid = snap->bpred_pending_valid;
    /*
     * Only rewrite the active uop's prediction cache when the
     * snapshot actually captured one AND the CPU has a live
     * uop to write into. If the save-time uop was freed between
     * save and restore (non-speculation cleanup paths), skip the
     * write — pending_resolve stays authoritative.
     */
    if (snap->has_active_uop_pred && cpu->active_uop != NULL) {
        CaeUop *uop = cpu->active_uop;
        uop->pred_valid = snap->pred_valid;
        uop->pred_taken = snap->pred_taken;
        uop->pred_target_known = snap->pred_target_known;
        uop->pred_target = snap->pred_target;
    }
}

void cae_cpu_spec_snapshot_drop(CaeCpuSpecSnapshot *snap)
{
    g_free(snap);
}

void cae_cpu_spec_slot_drop_if_live(CaeCpu *cpu)
{
    if (cpu == NULL) {
        return;
    }
    if (!cpu->spec_snap_valid && cpu->spec_snap == NULL) {
        return;
    }
    /*
     * Drop through the registered target emitter. cae_checkpoint_drop
     * is NULL-safe on the snap pointer: if save previously returned
     * NULL (no target registered), we still clear the valid bit so
     * future resolves see an empty slot.
     */
    cae_checkpoint_drop(cpu->spec_snap);
    cpu->spec_snap = NULL;
    cpu->spec_snap_valid = false;
    cpu->spec_squash_sqn = 0;
    memset(&cpu->spec_predicted, 0, sizeof(cpu->spec_predicted));
}

void cae_cpu_queue_spec_stimulus(CaeCpu *cpu, const CaeSpecStimulus *s)
{
    if (cpu == NULL || s == NULL) {
        return;
    }
    if (cpu->spec_stimuli_count >= CAE_SPEC_STIMULI_MAX) {
        /*
         * FIFO is full. Silently drop — the queue cap is small by
         * design, and an overflow here means the caller queued
         * more than one round worth of stimuli per live window.
         * Callers that care about overflow should observe
         * `spec_stimuli_count` before queueing.
         */
        return;
    }
    cpu->spec_stimuli[cpu->spec_stimuli_count] = *s;
    cpu->spec_stimuli_count++;
}

void cae_cpu_clear_spec_stimuli(CaeCpu *cpu)
{
    if (cpu == NULL) {
        return;
    }
    cpu->spec_stimuli_count = 0;
}
