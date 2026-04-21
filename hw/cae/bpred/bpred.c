/*
 * CAE branch predictor interface base.
 *
 * Registers the abstract TYPE_CAE_BPRED InterfaceClass and provides
 * safe dispatchers that concrete predictor implementations layer on.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qom/object.h"
#include "cae/bpred.h"

/*
 * Default fallback: assume all branches not-taken, no target known.
 * Used when the interface dispatcher is called with a NULL object.
 */
static const CaeBPredPrediction cae_bpred_default_prediction = {
    .target_pc = 0,
    .taken = false,
    .target_known = false,
};

CaeBPredPrediction cae_bpred_predict(Object *obj, const CaeBPredQuery *q)
{
    CaeBPredClass *cc;
    CaeBPredPrediction resp;

    if (!obj || !q) {
        return cae_bpred_default_prediction;
    }
    cc = CAE_BPRED_CLASS(object_get_class(obj));
    if (!cc || !cc->predict) {
        return cae_bpred_default_prediction;
    }
    resp = cc->predict(obj, q);

    /*
     * Unconditional branches are always taken. Normalise the response
     * so callers never see predicted-not-taken on an unconditional
     * branch, regardless of what the concrete predictor returned.
     */
    if (!q->is_conditional) {
        resp.taken = true;
    }
    return resp;
}

void cae_bpred_update(Object *obj, const CaeBPredResolve *r)
{
    CaeBPredClass *cc;

    if (!obj || !r) {
        return;
    }
    cc = CAE_BPRED_CLASS(object_get_class(obj));
    if (!cc || !cc->update) {
        return;
    }
    cc->update(obj, r);
}

void cae_bpred_reset(Object *obj)
{
    CaeBPredClass *cc;

    if (!obj) {
        return;
    }
    cc = CAE_BPRED_CLASS(object_get_class(obj));
    if (!cc || !cc->reset) {
        return;
    }
    cc->reset(obj);
}

bool cae_bpred_is_mispredict(const CaeBPredPrediction *p,
                             const CaeBPredResolve *r)
{
    if (!p || !r) {
        return false;
    }
    if (r->is_conditional) {
        if (p->taken != r->actual_taken) {
            return true;
        }
        /*
         * Predicted not-taken matched; fallthrough is always correct
         * target.
         */
        if (!r->actual_taken) {
            return false;
        }
        /*
         * Taken conditional: BTB must have produced a target. A cold
         * BTB miss (target_known=false) forces the frontend to stall
         * until the real target is computed, which costs the redirect
         * penalty even though the direction guess matched. Counting
         * this as a mispredict is what downstream cycle accounting
         * and branchy-workload calibration expect.
         */
        if (!p->target_known) {
            return true;
        }
        return p->target_pc != r->actual_target;
    }
    /*
     * Unconditional: only the target is in question. A missing BTB/RAS
     * hint still costs penalty because frontend can't redirect in time.
     */
    if (!p->target_known) {
        return true;
    }
    return p->target_pc != r->actual_target;
}

CaeBPredSpecSnapshot *cae_bpred_spec_snapshot_save(Object *obj)
{
    if (!obj) {
        return NULL;
    }
    CaeBPredClass *cc = CAE_BPRED_CLASS(object_get_class(obj));
    if (!cc || !cc->spec_snapshot) {
        return NULL;
    }
    return cc->spec_snapshot(obj);
}

void cae_bpred_spec_snapshot_restore(Object *obj,
                                     const CaeBPredSpecSnapshot *snap)
{
    if (!obj || !snap) {
        return;
    }
    CaeBPredClass *cc = CAE_BPRED_CLASS(object_get_class(obj));
    if (!cc || !cc->spec_restore) {
        return;
    }
    cc->spec_restore(obj, snap);
}

void cae_bpred_spec_snapshot_drop(Object *obj, CaeBPredSpecSnapshot *snap)
{
    if (!snap) {
        return;
    }
    /*
     * Drop routes through the vtable that created the snapshot.
     * `obj` may have been torn down between save and drop (unlikely
     * in practice — snapshots are short-lived, one per in-flight
     * speculation) but the vtable-through-obj pattern gives us a
     * clear ownership trail. If the object is NULL but a snapshot
     * exists, fall back to a bare free: this avoids leaking on an
     * unusual teardown order. Predictors whose snapshot owns nested
     * allocations (DecoupledBPU's FTQ copy, inner TAGE snapshot)
     * MUST implement ->spec_drop, since the bare free here would
     * leak the nested allocations.
     */
    if (obj) {
        CaeBPredClass *cc = CAE_BPRED_CLASS(object_get_class(obj));
        if (cc && cc->spec_drop) {
            cc->spec_drop(snap);
            return;
        }
    }
    g_free(snap);
}

static const TypeInfo cae_bpred_interface_type = {
    .name = TYPE_CAE_BPRED,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(CaeBPredClass),
};

static void cae_bpred_register_types(void)
{
    type_register_static(&cae_bpred_interface_type);
}

type_init(cae_bpred_register_types)
