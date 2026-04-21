/*
 * CAE 2-bit local branch predictor.
 *
 * Per-PC saturating 2-bit counter table for direction prediction,
 * combined with an internal BTB for direct/indirect targets and a RAS
 * for returns. Exposed as TYPE_USER_CREATABLE so it can be attached by
 * accel property wiring, and implements TYPE_CAE_BPRED.
 *
 * Deterministic by construction:
 *  - counters initialise to weak-not-taken (0b01)
 *  - BTB / RAS start empty
 *  - all state updates are pure functions of the resolved outcome
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
#include "cae/bpred.h"
#include "hw/cae/bpred/btb.h"
#include "hw/cae/bpred/ras.h"

#define TYPE_CAE_BPRED_2BIT_LOCAL "cae-bpred-2bit-local"

OBJECT_DECLARE_SIMPLE_TYPE(CaeBPred2bitLocal, CAE_BPRED_2BIT_LOCAL)

/*
 * 2-bit counter encoding:
 *   0 = strong not-taken
 *   1 = weak not-taken
 *   2 = weak taken
 *   3 = strong taken
 * Predict-taken iff counter >= 2.
 */
#define CAE_2BIT_INIT 1   /* weak not-taken */

struct CaeBPred2bitLocal {
    Object parent;

    /* Config */
    uint32_t local_history_bits;     /* log2(counter table size) */
    uint32_t btb_entries;
    uint32_t btb_assoc;
    uint32_t ras_depth;
    uint32_t mispredict_penalty_cycles;

    /* Derived */
    uint32_t num_counters;
    uint8_t *counters;               /* [num_counters] */

    CaeBtb btb;
    CaeRas ras;

    /* Stats */
    uint64_t predictions;
    uint64_t mispredictions;
    uint64_t direction_mismatches;
    uint64_t target_mismatches;

    bool initialised;
};

static uint32_t counter_index(const CaeBPred2bitLocal *p, uint64_t pc)
{
    /* Drop the two low bits (RV insn alignment) and mask with table size. */
    return (uint32_t)((pc >> 2) & (p->num_counters - 1));
}

static bool counter_predicts_taken(uint8_t counter)
{
    return counter >= 2;
}

static uint8_t counter_update(uint8_t counter, bool actual_taken)
{
    if (actual_taken) {
        return counter == 3 ? 3 : counter + 1;
    }
    return counter == 0 ? 0 : counter - 1;
}

static CaeBPredPrediction cae_bpred_2bit_local_predict(Object *obj,
                                                       const CaeBPredQuery *q)
{
    CaeBPred2bitLocal *p = CAE_BPRED_2BIT_LOCAL(obj);
    CaeBPredPrediction resp = {
        .target_pc = q->fallthrough_pc,
        .taken = false,
        .target_known = true,   /* fallthrough is always known */
    };
    uint64_t target = 0;

    if (!p->initialised) {
        return resp;
    }

    /* Direction. */
    if (q->is_conditional) {
        uint32_t idx = counter_index(p, q->pc);
        resp.taken = counter_predicts_taken(p->counters[idx]);
    } else {
        resp.taken = true;
    }

    /* Target. */
    if (resp.taken) {
        if (q->is_return && cae_ras_peek(&p->ras, &target)) {
            resp.target_pc = target;
            resp.target_known = true;
        } else if (cae_btb_lookup(&p->btb, q->pc, &target)) {
            resp.target_pc = target;
            resp.target_known = true;
        } else {
            resp.target_known = q->is_conditional;
            /*
             * Conditional-and-BTB-miss: we still have the fallthrough
             * as a valid "target" if predicted not-taken. For taken
             * prediction without BTB target, mark unknown so callers
             * charge the mispredict penalty.
             */
            if (resp.taken) {
                resp.target_known = false;
            }
        }
    }
    return resp;
}

static void cae_bpred_2bit_local_update(Object *obj, const CaeBPredResolve *r)
{
    CaeBPred2bitLocal *p = CAE_BPRED_2BIT_LOCAL(obj);
    CaeBPredPrediction predicted;
    CaeBPredQuery q = {
        .pc = r->pc,
        .fallthrough_pc = 0,
        .is_conditional = r->is_conditional,
        .is_call = r->is_call,
        .is_return = r->is_return,
        .is_indirect = r->is_indirect,
    };

    if (!p->initialised) {
        return;
    }

    /* Snapshot the pre-update prediction for stats. */
    predicted = cae_bpred_2bit_local_predict(obj, &q);

    /* Direction counter. */
    if (r->is_conditional) {
        uint32_t idx = counter_index(p, r->pc);
        p->counters[idx] = counter_update(p->counters[idx], r->actual_taken);
    }

    /* BTB learning: insert target whenever branch was taken. */
    if (r->actual_taken && !r->is_return) {
        cae_btb_insert(&p->btb, r->pc, r->actual_target);
    }

    /*
     * RAS semantics:
     *  - call: push return address = pc + insn_bytes (2 for RVC, 4 for
     *    standard). r->insn_bytes must be filled by the caller; fall
     *    back to 4 only when unset so mis-populated resolves still
     *    produce a workable RAS.
     *  - return: pop.
     */
    if (r->is_call) {
        uint8_t bytes = r->insn_bytes ? r->insn_bytes : 4;
        cae_ras_push(&p->ras, r->pc + bytes);
    } else if (r->is_return) {
        uint64_t dummy;
        cae_ras_pop(&p->ras, &dummy);
    }

    /* Stats. */
    p->predictions++;
    if (cae_bpred_is_mispredict(&predicted, r)) {
        p->mispredictions++;
        if (predicted.taken != r->actual_taken) {
            p->direction_mismatches++;
        } else if (predicted.target_pc != r->actual_target) {
            p->target_mismatches++;
        }
    }
}

static void cae_bpred_2bit_local_reset(Object *obj)
{
    CaeBPred2bitLocal *p = CAE_BPRED_2BIT_LOCAL(obj);
    uint32_t i;

    if (!p->initialised) {
        return;
    }
    for (i = 0; i < p->num_counters; i++) {
        p->counters[i] = CAE_2BIT_INIT;
    }
    cae_btb_reset(&p->btb);
    cae_ras_reset(&p->ras);
    p->predictions = 0;
    p->mispredictions = 0;
    p->direction_mismatches = 0;
    p->target_mismatches = 0;
}

static uint64_t *stat_ptr(CaeBPred2bitLocal *p, const char *name)
{
    if (g_str_equal(name, "predictions")) {
        return &p->predictions;
    }
    if (g_str_equal(name, "mispredictions")) {
        return &p->mispredictions;
    }
    if (g_str_equal(name, "direction-mismatches")) {
        return &p->direction_mismatches;
    }
    if (g_str_equal(name, "target-mismatches")) {
        return &p->target_mismatches;
    }
    if (g_str_equal(name, "btb-hits")) {
        return &p->btb.hits;
    }
    if (g_str_equal(name, "btb-misses")) {
        return &p->btb.misses;
    }
    if (g_str_equal(name, "ras-pushes")) {
        return &p->ras.pushes;
    }
    if (g_str_equal(name, "ras-pops")) {
        return &p->ras.pops;
    }
    if (g_str_equal(name, "ras-overflows")) {
        return &p->ras.overflows;
    }
    if (g_str_equal(name, "ras-underflows")) {
        return &p->ras.underflows;
    }
    return NULL;
}

static void cae_bpred_2bit_local_stat_get(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    CaeBPred2bitLocal *p = CAE_BPRED_2BIT_LOCAL(obj);
    uint64_t *field = stat_ptr(p, name);
    uint64_t value;

    if (!field) {
        value = 0;
    } else {
        value = qatomic_read(field);
    }
    visit_type_uint64(v, name, &value, errp);
}

static void cae_bpred_2bit_local_add_stat(Object *obj, const char *name)
{
    object_property_add(obj, name, "uint64",
                        cae_bpred_2bit_local_stat_get,
                        NULL, NULL, NULL);
}

static void cae_bpred_2bit_local_instance_init(Object *obj)
{
    CaeBPred2bitLocal *p = CAE_BPRED_2BIT_LOCAL(obj);

    p->local_history_bits = 10; /* 1024-entry counter table */
    p->btb_entries = 64;
    p->btb_assoc = 2;
    p->ras_depth = 16;
    p->mispredict_penalty_cycles = 3;

    object_property_add_uint32_ptr(obj, "local-history-bits",
                                   &p->local_history_bits,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "btb-entries",
                                   &p->btb_entries,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "btb-assoc",
                                   &p->btb_assoc,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ras-depth",
                                   &p->ras_depth,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "mispredict-penalty-cycles",
                                   &p->mispredict_penalty_cycles,
                                   OBJ_PROP_FLAG_READWRITE);

    cae_bpred_2bit_local_add_stat(obj, "predictions");
    cae_bpred_2bit_local_add_stat(obj, "mispredictions");
    cae_bpred_2bit_local_add_stat(obj, "direction-mismatches");
    cae_bpred_2bit_local_add_stat(obj, "target-mismatches");
    cae_bpred_2bit_local_add_stat(obj, "btb-hits");
    cae_bpred_2bit_local_add_stat(obj, "btb-misses");
    cae_bpred_2bit_local_add_stat(obj, "ras-pushes");
    cae_bpred_2bit_local_add_stat(obj, "ras-pops");
    cae_bpred_2bit_local_add_stat(obj, "ras-overflows");
    cae_bpred_2bit_local_add_stat(obj, "ras-underflows");
}

static void cae_bpred_2bit_local_finalize(Object *obj)
{
    CaeBPred2bitLocal *p = CAE_BPRED_2BIT_LOCAL(obj);

    g_free(p->counters);
    p->counters = NULL;
    cae_btb_release(&p->btb);
    cae_ras_release(&p->ras);
    p->initialised = false;
}

static void cae_bpred_2bit_local_complete(UserCreatable *uc, Error **errp)
{
    CaeBPred2bitLocal *p = CAE_BPRED_2BIT_LOCAL(OBJECT(uc));
    uint32_t i;

    if (p->local_history_bits == 0 || p->local_history_bits > 20) {
        error_setg(errp,
                   "local-history-bits must be in 1..20 (got %u)",
                   p->local_history_bits);
        return;
    }
    p->num_counters = 1u << p->local_history_bits;
    p->counters = g_new(uint8_t, p->num_counters);
    for (i = 0; i < p->num_counters; i++) {
        p->counters[i] = CAE_2BIT_INIT;
    }
    if (!cae_btb_init(&p->btb, p->btb_entries, p->btb_assoc, errp)) {
        g_free(p->counters);
        p->counters = NULL;
        return;
    }
    if (!cae_ras_init(&p->ras, p->ras_depth, errp)) {
        g_free(p->counters);
        p->counters = NULL;
        cae_btb_release(&p->btb);
        return;
    }
    p->initialised = true;
}

static void cae_bpred_2bit_local_class_init(ObjectClass *oc, const void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);
    CaeBPredClass *bc = CAE_BPRED_CLASS(oc);

    ucc->complete = cae_bpred_2bit_local_complete;

    bc->predict = cae_bpred_2bit_local_predict;
    bc->update = cae_bpred_2bit_local_update;
    bc->reset = cae_bpred_2bit_local_reset;
}

static const TypeInfo cae_bpred_2bit_local_type = {
    .name = TYPE_CAE_BPRED_2BIT_LOCAL,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeBPred2bitLocal),
    .instance_init = cae_bpred_2bit_local_instance_init,
    .instance_finalize = cae_bpred_2bit_local_finalize,
    .class_init = cae_bpred_2bit_local_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { TYPE_CAE_BPRED },
        { }
    },
};

static void cae_bpred_2bit_local_register_types(void)
{
    type_register_static(&cae_bpred_2bit_local_type);
}

type_init(cae_bpred_2bit_local_register_types)
