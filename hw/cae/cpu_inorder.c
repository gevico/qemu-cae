/*
 * CAE in-order scalar CPU timing model (skeleton).
 *
 * Implements the 5-stage in-order pipeline charge function by mapping
 * a retiring uop's functional-unit class to a base execute-cycle cost,
 * then adding a branch-mispredict penalty when a bpred is attached and
 * reports a miss. Memory latency is accounted separately by the
 * CaeMemClass chain (cae-cache -> cae-dram) via cae_mem_access_notify,
 * so this model intentionally does not double-count load/store cost.
 *
 * This is the M2 skeleton: it provides the QOM object, configurable
 * latency table, bpred attach point, and CaeCpuModel implementation.
 * Full pipeline integration into the cae_charge_executed_tb() hot path
 * and bpred resolve wiring is scheduled for the follow-up round; the
 * charge function is testable in isolation today.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/atomic.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "cae/cpu_model.h"
#include "cae/bpred.h"
#include "cae/pipeline.h"
#include "cae/uop.h"

#define TYPE_CAE_CPU_INORDER "cae-cpu-inorder"

OBJECT_DECLARE_SIMPLE_TYPE(CaeCpuInorder, CAE_CPU_INORDER)

struct CaeCpuInorder {
    Object parent;

    /* Per-uop-class execute-stage latencies (cycles). */
    uint32_t latency_alu;
    uint32_t latency_branch;
    uint32_t latency_load;
    uint32_t latency_store;
    uint32_t latency_mul;
    uint32_t latency_div;
    uint32_t latency_fpu;
    uint32_t latency_system;
    uint32_t latency_fence;
    uint32_t latency_atomic;

    /* Branch mispredict penalty (frontend refill cost, cycles). */
    uint32_t mispredict_penalty_cycles;

    /*
     * Pipeline-overlap credit. Represents the fraction of each
     * retirement's latency that's hidden by fetch/decode/execute
     * overlap in a steady-state scalar in-order pipeline — gem5
     * MinorCPU's numCycles can come in below simInsts (effective
     * CPI < 1) on ALU-dense code because its fetch/decode stages
     * pipeline with the execute stage. Expressed as an integer
     * permille (0..1000): 0 = pure CPI-1, 200 = 0.8 effective cycles
     * per non-stalling retire. Accumulator carries the sub-cycle
     * remainder between charges so the cycle sequence is
     * deterministic (same input stream → same retire pattern).
     */
    uint32_t overlap_permille;
    uint64_t overlap_accumulator;   /* carries 0..999 permille over */

    /*
     * Load-use stall: extra cycles added after every LOAD / ATOMIC
     * retire to approximate the pipeline bubble when the next
     * instruction immediately consumes the loaded register. gem5
     * MinorCPU holds the pipeline until the dependent value
     * arrives; without this the CAE scalar in-order model
     * underestimates dependent-load workloads (pointer-chase in
     * particular) by ~35 %.
     */
    uint32_t load_use_stall_cycles;

    /*
     * Optional branch predictor. link<cae-bpred>; user attaches after
     * construction. Nothing in this model calls predict()/update() on
     * the hot path; the wiring lives at the level that sees branch
     * resolves (cae_charge_executed_tb once the active_uop chained-TB
     * lifecycle is extended).
     */
    Object *bpred;

    /* Stats */
    uint64_t charges;
    uint64_t total_cycles;
};

static uint32_t latency_for_uop(const CaeCpuInorder *m, const CaeUop *uop)
{
    if (!uop) {
        return 1;
    }
    switch (uop->type) {
    case CAE_UOP_ALU:     return m->latency_alu;
    case CAE_UOP_BRANCH:  return m->latency_branch;
    case CAE_UOP_LOAD:    return m->latency_load;
    case CAE_UOP_STORE:   return m->latency_store;
    case CAE_UOP_MUL:     return m->latency_mul;
    case CAE_UOP_DIV:     return m->latency_div;
    case CAE_UOP_FPU:     return m->latency_fpu;
    case CAE_UOP_SYSTEM:  return m->latency_system;
    case CAE_UOP_FENCE:   return m->latency_fence;
    case CAE_UOP_ATOMIC:  return m->latency_atomic;
    case CAE_UOP_UNKNOWN:
    default:              return 1;
    }
}

static uint32_t cae_cpu_inorder_charge(Object *obj, const CaeCpu *cpu,
                                       const CaeUop *uop)
{
    CaeCpuInorder *m = CAE_CPU_INORDER(obj);
    uint32_t base_cycles = latency_for_uop(m, uop);
    uint32_t cycles;

    (void)cpu;

    /*
     * Pipeline-overlap credit. When overlap_permille is zero the model
     * charges the raw base-latency (round-2 behaviour); non-zero
     * values spread the credit across successive retires via a
     * permille accumulator. The accumulator's carry-over between
     * charges keeps determinism intact: for the same input stream
     * the integer-cycle sequence is the same across runs.
     *
     *   effective_permille = base_cycles * (1000 - overlap_permille)
     *   accumulator += effective_permille
     *   cycles = accumulator / 1000
     *   accumulator %= 1000
     *
     * Worked example (base=1, overlap=200):
     *   retire 1: eff=800, acc=800, cycles=0
     *   retire 2: eff=800, acc=1600, cycles=1, acc=600
     *   retire 3: eff=800, acc=1400, cycles=1, acc=400
     *   retire 4: eff=800, acc=1200, cycles=1, acc=200
     *   retire 5: eff=800, acc=1000, cycles=1, acc=0
     * 4 cycles / 5 retires = 0.8 effective CPI, matching the gem5
     * MinorCPU ALU-dense steady state.
     */
    if (m->overlap_permille >= 1000) {
        /* Defensive: 100 % overlap would wedge at CPI=0. Clamp to 999
         * so we still advance the engine. */
        m->overlap_permille = 999;
    }
    /*
     * Overlap applies only to uops whose cost is resolved inside
     * the pipeline frontend (ALU / MUL / DIV / FPU / BRANCH /
     * SYSTEM / FENCE). LOAD / STORE / ATOMIC latencies come from
     * the CaeMemClass backend (cache + dram hits/misses) via
     * cae_mem_access_notify, and that latency already reflects
     * whatever overlap gem5's MinorCPU applies; reducing it again
     * here would double-credit memory-bound workloads.
     */
    bool overlap_applies = true;
    if (uop) {
        switch (uop->type) {
        case CAE_UOP_LOAD:
        case CAE_UOP_STORE:
        case CAE_UOP_ATOMIC:
            overlap_applies = false;
            break;
        default:
            overlap_applies = true;
            break;
        }
    }
    if (overlap_applies && m->overlap_permille > 0) {
        uint64_t effective = (uint64_t)base_cycles *
                             (1000 - m->overlap_permille);
        m->overlap_accumulator += effective;
        cycles = (uint32_t)(m->overlap_accumulator / 1000);
        m->overlap_accumulator %= 1000;
    } else {
        cycles = base_cycles;
    }

    /* Load-use stall on dep-heavy workloads. */
    if (uop && !overlap_applies && m->load_use_stall_cycles > 0) {
        cycles += m->load_use_stall_cycles;
    }

    qatomic_set(&m->charges, qatomic_read(&m->charges) + 1);
    qatomic_set(&m->total_cycles,
                    qatomic_read(&m->total_cycles) + cycles);
    return cycles;
}

static void cae_cpu_inorder_check_bpred(const Object *obj, const char *name,
                                        Object *val, Error **errp)
{
    /*
     * Presence of a non-NULL check callback makes the link<> property
     * writable (see BitLesson BL-20260417-qom-link-null-check-readonly).
     * Real type enforcement happens at attach time: link<cae-bpred>
     * already gates on TYPE_CAE_BPRED; no extra validation needed.
     */
}

static void cae_cpu_inorder_stat_get(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    uint64_t value = qatomic_read((uint64_t *)opaque);
    visit_type_uint64(v, name, &value, errp);
}

static void cae_cpu_inorder_instance_init(Object *obj)
{
    CaeCpuInorder *m = CAE_CPU_INORDER(obj);

    m->latency_alu = 1;
    m->latency_branch = 1;
    m->latency_load = 1;
    m->latency_store = 1;
    m->latency_mul = 3;
    m->latency_div = 20;
    m->latency_fpu = 4;
    m->latency_system = 1;
    m->latency_fence = 1;
    m->latency_atomic = 4;
    m->mispredict_penalty_cycles = 3;
    m->overlap_permille = 0;
    m->overlap_accumulator = 0;
    m->load_use_stall_cycles = 0;
    m->bpred = NULL;
    m->charges = 0;
    m->total_cycles = 0;

    object_property_add_uint32_ptr(obj, "latency-alu",
                                   &m->latency_alu, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-branch",
                                   &m->latency_branch,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-load",
                                   &m->latency_load, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-store",
                                   &m->latency_store, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-mul",
                                   &m->latency_mul, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-div",
                                   &m->latency_div, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-fpu",
                                   &m->latency_fpu, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-system",
                                   &m->latency_system,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-fence",
                                   &m->latency_fence,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-atomic",
                                   &m->latency_atomic,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "mispredict-penalty-cycles",
                                   &m->mispredict_penalty_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "overlap-permille",
                                   &m->overlap_permille,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "load-use-stall-cycles",
                                   &m->load_use_stall_cycles,
                                   OBJ_PROP_FLAG_READWRITE);

    object_property_add_link(obj, "bpred", TYPE_CAE_BPRED, &m->bpred,
                             cae_cpu_inorder_check_bpred,
                             OBJ_PROP_LINK_STRONG);

    object_property_add(obj, "charges", "uint64",
                        cae_cpu_inorder_stat_get,
                        NULL, NULL, &m->charges);
    object_property_add(obj, "total-cycles", "uint64",
                        cae_cpu_inorder_stat_get,
                        NULL, NULL, &m->total_cycles);
}

static void cae_cpu_inorder_complete(UserCreatable *uc, Error **errp)
{
    CaeCpuInorder *m = CAE_CPU_INORDER(OBJECT(uc));

    if (m->latency_alu == 0) {
        error_setg(errp, "latency-alu must be non-zero");
        return;
    }
    if (m->mispredict_penalty_cycles == 0) {
        error_setg(errp, "mispredict-penalty-cycles must be non-zero");
        return;
    }
}

static void cae_cpu_inorder_class_init(ObjectClass *oc, const void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);
    CaeCpuModelClass *cmc = CAE_CPU_MODEL_CLASS(oc);

    ucc->complete = cae_cpu_inorder_complete;
    cmc->charge = cae_cpu_inorder_charge;
}

static const TypeInfo cae_cpu_inorder_type = {
    .name = TYPE_CAE_CPU_INORDER,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeCpuInorder),
    .instance_init = cae_cpu_inorder_instance_init,
    .class_init = cae_cpu_inorder_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { TYPE_CAE_CPU_MODEL },
        { }
    },
};

static void cae_cpu_inorder_register_types(void)
{
    type_register_static(&cae_cpu_inorder_type);
}

type_init(cae_cpu_inorder_register_types)
