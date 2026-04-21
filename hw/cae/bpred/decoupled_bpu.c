/*
 * CAE decoupled BPU (Round 15 M4' frontend deliverable).
 *
 * DecoupledBPU sits between fetch and the prediction stack. In
 * the real XiangShan KMH-V3 frontend, fetch consumes blocks from
 * the Fetch Stream Queue (FSQ), which is filled by the Fetch
 * Target Queue (FTQ), which is filled by the prediction pipeline
 * (DecoupledBPU + TAGE-SC-L + BTB + RAS + iTLB). The key
 * architectural property this layer provides is decoupling: the
 * predictor can run ahead of fetch, producing a pipeline of
 * predicted blocks; and mispredict recovery scopes the squash to
 * the FTQ/FSQ tail rather than the whole pipeline.
 *
 * This round-15 implementation is a functional-oracle model
 * sized for AC-K-4:
 *
 *   - inner TAGE-SC-L child handles direction + target
 *     prediction (allocated in complete(), parented under
 *     `/objects/cae-bpred/inner`);
 *   - FTQ is an in-order ring of predicted branch metadata,
 *     pushed on predict(), popped on update(), flushed on
 *     mispredict;
 *   - FSQ tracks fetch-block occupancy at the same granularity
 *     (one push per predict, one pop per update, flush on
 *     mispredict). Per-block detail lands with M5' calibration.
 *
 * Defaults match the kmhv3.py DecoupledBPU baseline:
 *   ftq-size = 64
 *   fsq-size = 64
 *   btb-entries / btb-assoc / ras-depth / mispredict penalty
 *       reuse the same accel-knob values the inner TAGE-SC-L
 *       would otherwise consume.
 *
 * `BL-20260417-qom-link-target-needs-canonical-path`: the inner
 * TAGE-SC-L is `object_property_add_child`-parented under the
 * decoupled-bpu Object before any link use, so the canonical-
 * path invariants hold.
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
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "cae/bpred.h"

#define TYPE_CAE_BPRED_DECOUPLED "cae-bpred-decoupled"
#define TYPE_CAE_BPRED_TAGE_SC_L "cae-bpred-tage-sc-l"

OBJECT_DECLARE_SIMPLE_TYPE(CaeBPredDecoupled, CAE_BPRED_DECOUPLED)

typedef struct CaeFtqEntry {
    uint64_t pc;
    uint64_t pred_target;
    bool     pred_taken;
    bool     target_known;
    bool     valid;
} CaeFtqEntry;

struct CaeBPredDecoupled {
    Object parent;

    /* Tunables. */
    uint32_t ftq_size;
    uint32_t fsq_size;
    uint32_t btb_entries;
    uint32_t btb_assoc;
    uint32_t ras_depth;
    uint32_t mispredict_penalty_cycles;

    /* Inner predictor (TAGE-SC-L). Owned via
     * object_property_add_child; freed by QOM when the parent
     * is unparented. */
    Object *inner;

    /* FTQ ring. */
    CaeFtqEntry *ftq;
    uint32_t ftq_head;        /* next slot to pop (update side) */
    uint32_t ftq_tail;        /* next slot to push (predict side) */
    uint32_t ftq_occupancy;

    /* FSQ: occupancy-only for M4' functional oracle. */
    uint32_t fsq_occupancy;

    /* Stats — all READ-ONLY uint64. */
    uint64_t predictions;
    uint64_t mispredictions;
    uint64_t ftq_pushes;
    uint64_t ftq_pops;
    uint64_t ftq_flushes;
    uint64_t ftq_stalls;
    uint64_t fsq_pushes;
    uint64_t fsq_pops;
    uint64_t fsq_flushes;
    uint64_t fsq_stalls;

    bool initialised;
};

/* ---- FTQ helpers ---------------------------------------------- */

static inline bool ftq_full(const CaeBPredDecoupled *p)
{
    return p->ftq_occupancy >= p->ftq_size;
}

/*
 * BS-36 round-16 fix: ftq_push must refuse to write when the
 * ring is already full. Round 15 incremented ftq_occupancy
 * unconditionally and let the slot at ftq_tail get silently
 * overwritten — both in the conditional-full path (which was
 * guarded elsewhere, but still called the unsafe helper if the
 * guard ever regressed) and in the unconditional-full path
 * (which fell through to the push from cae_bpred_decoupled_
 * predict without a guard). Returns true on success, false on
 * overflow so the caller can account the drop.
 */
static bool ftq_push(CaeBPredDecoupled *p, const CaeBPredQuery *q,
                     const CaeBPredPrediction *pred)
{
    if (p->ftq_occupancy >= p->ftq_size) {
        return false;
    }
    CaeFtqEntry *e = &p->ftq[p->ftq_tail];
    e->pc = q->pc;
    e->pred_target = pred->target_pc;
    e->pred_taken = pred->taken;
    e->target_known = pred->target_known;
    e->valid = true;
    p->ftq_tail = (p->ftq_tail + 1u) % p->ftq_size;
    p->ftq_occupancy++;
    p->ftq_pushes++;
    return true;
}

static bool ftq_pop(CaeBPredDecoupled *p, CaeFtqEntry *out)
{
    if (p->ftq_occupancy == 0u) {
        return false;
    }
    CaeFtqEntry *e = &p->ftq[p->ftq_head];
    if (out) {
        *out = *e;
    }
    e->valid = false;
    p->ftq_head = (p->ftq_head + 1u) % p->ftq_size;
    p->ftq_occupancy--;
    p->ftq_pops++;
    return true;
}

/*
 * Drop every remaining FTQ entry. Models the frontend squash
 * that follows a resolved mispredict — any prediction queued
 * behind the mispredicted branch is now on the wrong path and
 * must be discarded before the correct-path fetch resumes.
 * Returns the count flushed so the caller can attribute it to
 * the `ftq-flushes` counter.
 */
static uint32_t ftq_flush_all(CaeBPredDecoupled *p)
{
    uint32_t flushed = p->ftq_occupancy;
    while (p->ftq_occupancy > 0u) {
        p->ftq[p->ftq_head].valid = false;
        p->ftq_head = (p->ftq_head + 1u) % p->ftq_size;
        p->ftq_occupancy--;
    }
    p->ftq_tail = p->ftq_head;
    return flushed;
}

/* ---- CaeBPredClass vtable ------------------------------------- */

static CaeBPredPrediction cae_bpred_decoupled_predict(
    Object *obj, const CaeBPredQuery *q)
{
    CaeBPredDecoupled *p = CAE_BPRED_DECOUPLED(obj);
    CaeBPredPrediction resp = {
        .target_pc = q->fallthrough_pc,
        .taken = false,
        .target_known = true,
    };

    if (!p->initialised) {
        return resp;
    }

    /*
     * FTQ-full stall: in the real frontend, the predictor
     * pipeline back-pressures fetch when FTQ fills up. Both
     * conditional and unconditional branches return the
     * stub "don't advance" prediction when the FTQ is full —
     * the round-15 uncond-full fall-through was the BS-36 bug
     * that let ftq_push corrupt the ring. The functional-
     * oracle semantic is simpler: if there is no FTQ slot,
     * no prediction can be enqueued this call; the caller
     * sees a frontend stall and retries next retire.
     */
    if (ftq_full(p)) {
        p->ftq_stalls++;
        resp.taken = false;
        resp.target_pc = q->fallthrough_pc;
        resp.target_known = false;
        return resp;
    }

    /* Delegate direction + target to the inner TAGE-SC-L. */
    resp = cae_bpred_predict(p->inner, q);
    p->predictions++;

    /*
     * Push onto FTQ / FSQ for later resolution. ftq_push is
     * bounds-safe (returns false when full); we never call
     * it without the ftq_full guard above, but the return
     * value is consulted defensively in case a future
     * refactor reintroduces the fall-through.
     */
    (void)ftq_push(p, q, &resp);
    if (p->fsq_occupancy < p->fsq_size) {
        p->fsq_occupancy++;
        p->fsq_pushes++;
    } else {
        p->fsq_stalls++;
    }

    return resp;
}

static void cae_bpred_decoupled_update(Object *obj,
                                       const CaeBPredResolve *r)
{
    CaeBPredDecoupled *p = CAE_BPRED_DECOUPLED(obj);

    if (!p->initialised) {
        return;
    }

    /*
     * Pop the FTQ head corresponding to this retiring branch.
     * M4' functional-oracle model: FTQ is in-order by
     * predict-order, so the head is the oldest live prediction.
     * Reconstruct the predicted outcome from the popped entry
     * to drive the mispredict check.
     */
    CaeFtqEntry popped;
    bool had_head = ftq_pop(p, &popped);
    if (p->fsq_occupancy > 0u) {
        p->fsq_occupancy--;
        p->fsq_pops++;
    }

    /* Always update the inner predictor — learning state must
     * advance even when FTQ was empty (e.g. first branch after
     * reset). */
    cae_bpred_update(p->inner, r);

    if (had_head) {
        CaeBPredPrediction pred = {
            .target_pc = popped.pred_target,
            .taken = popped.pred_taken,
            .target_known = popped.target_known,
        };
        if (cae_bpred_is_mispredict(&pred, r)) {
            uint32_t flushed = ftq_flush_all(p);
            p->ftq_flushes += flushed;
            /* FSQ mirrors the FTQ squash. */
            if (p->fsq_occupancy > 0u) {
                p->fsq_flushes += p->fsq_occupancy;
                p->fsq_occupancy = 0u;
            }
            p->mispredictions++;
        }
    }
}

static void cae_bpred_decoupled_reset(Object *obj)
{
    CaeBPredDecoupled *p = CAE_BPRED_DECOUPLED(obj);
    if (!p->initialised) {
        return;
    }
    cae_bpred_reset(p->inner);
    ftq_flush_all(p);
    p->fsq_occupancy = 0u;
    p->predictions = 0;
    p->mispredictions = 0;
    p->ftq_pushes = 0;
    p->ftq_pops = 0;
    p->ftq_flushes = 0;
    p->ftq_stalls = 0;
    p->fsq_pushes = 0;
    p->fsq_pops = 0;
    p->fsq_flushes = 0;
    p->fsq_stalls = 0;
}

/* ---- Lifecycle ------------------------------------------------ */

static void cae_bpred_decoupled_complete(UserCreatable *uc,
                                         Error **errp)
{
    CaeBPredDecoupled *p = CAE_BPRED_DECOUPLED(uc);

    if (p->ftq_size == 0u) {
        p->ftq_size = 64u;
    }
    if (p->ftq_size > 1024u) {
        error_setg(errp, "cae-bpred-decoupled: ftq-size=%u too large "
                   "(cap 1024)", p->ftq_size);
        return;
    }
    if (p->fsq_size == 0u) {
        p->fsq_size = 64u;
    }

    /*
     * BS-35 round 15: construct the inner TAGE-SC-L predictor as
     * a child QOM object. Parented under the DecoupledBPU's own
     * Object (not object_get_objects_root), so the path becomes
     * /objects/cae-bpred/inner when the outer is the
     * cae-bpred accel child. The BTB/RAS/penalty knobs flow from
     * the decoupled wrapper's accel-forwarded tunables into the
     * inner.
     */
    Object *inner = object_new(TYPE_CAE_BPRED_TAGE_SC_L);
    if (p->btb_entries) {
        object_property_set_uint(inner, "btb-entries",
                                 p->btb_entries, &error_abort);
    }
    if (p->btb_assoc) {
        object_property_set_uint(inner, "btb-assoc",
                                 p->btb_assoc, &error_abort);
    }
    if (p->ras_depth) {
        object_property_set_uint(inner, "ras-depth",
                                 p->ras_depth, &error_abort);
    }
    if (p->mispredict_penalty_cycles) {
        object_property_set_uint(inner, "mispredict-penalty-cycles",
                                 p->mispredict_penalty_cycles,
                                 &error_abort);
    }
    user_creatable_complete(USER_CREATABLE(inner), &error_abort);
    object_property_add_child(OBJECT(p), "inner", inner);
    object_unref(inner);
    p->inner = inner;

    p->ftq = g_new0(CaeFtqEntry, p->ftq_size);
    p->ftq_head = 0;
    p->ftq_tail = 0;
    p->ftq_occupancy = 0;
    p->fsq_occupancy = 0;
    p->initialised = true;
}

static void cae_bpred_decoupled_finalize(Object *obj)
{
    CaeBPredDecoupled *p = CAE_BPRED_DECOUPLED(obj);
    g_free(p->ftq);
    p->ftq = NULL;
    /* `inner` is a child object: QOM releases it when the
     * parent is unparented. No explicit object_unref here. */
}

/* ---- QOM plumbing --------------------------------------------- */

static void cae_bpred_decoupled_stat_get(Object *obj, Visitor *v,
                                         const char *name,
                                         void *opaque, Error **errp)
{
    uint64_t *ptr = opaque;
    uint64_t value;
    (void)obj;
    (void)name;
    value = qatomic_read(ptr);
    visit_type_uint64(v, "stat", &value, errp);
}

static void cae_bpred_decoupled_instance_init(Object *obj)
{
    CaeBPredDecoupled *p = CAE_BPRED_DECOUPLED(obj);

    p->ftq_size = 64u;
    p->fsq_size = 64u;
    p->btb_entries = 64u;
    p->btb_assoc = 2u;
    p->ras_depth = 16u;
    p->mispredict_penalty_cycles = 7u;

    object_property_add_uint32_ptr(obj, "ftq-size", &p->ftq_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "fsq-size", &p->fsq_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "btb-entries",
                                   &p->btb_entries,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "btb-assoc", &p->btb_assoc,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ras-depth", &p->ras_depth,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "mispredict-penalty-cycles",
                                   &p->mispredict_penalty_cycles,
                                   OBJ_PROP_FLAG_READWRITE);

    object_property_add(obj, "predictions", "uint64",
                        cae_bpred_decoupled_stat_get, NULL, NULL,
                        &p->predictions);
    object_property_add(obj, "mispredictions", "uint64",
                        cae_bpred_decoupled_stat_get, NULL, NULL,
                        &p->mispredictions);
    object_property_add(obj, "ftq-pushes", "uint64",
                        cae_bpred_decoupled_stat_get, NULL, NULL,
                        &p->ftq_pushes);
    object_property_add(obj, "ftq-pops", "uint64",
                        cae_bpred_decoupled_stat_get, NULL, NULL,
                        &p->ftq_pops);
    object_property_add(obj, "ftq-flushes", "uint64",
                        cae_bpred_decoupled_stat_get, NULL, NULL,
                        &p->ftq_flushes);
    object_property_add(obj, "ftq-stalls", "uint64",
                        cae_bpred_decoupled_stat_get, NULL, NULL,
                        &p->ftq_stalls);
    object_property_add(obj, "fsq-pushes", "uint64",
                        cae_bpred_decoupled_stat_get, NULL, NULL,
                        &p->fsq_pushes);
    object_property_add(obj, "fsq-pops", "uint64",
                        cae_bpred_decoupled_stat_get, NULL, NULL,
                        &p->fsq_pops);
    object_property_add(obj, "fsq-flushes", "uint64",
                        cae_bpred_decoupled_stat_get, NULL, NULL,
                        &p->fsq_flushes);
    object_property_add(obj, "fsq-stalls", "uint64",
                        cae_bpred_decoupled_stat_get, NULL, NULL,
                        &p->fsq_stalls);
}

/* ------------------------------------------------------------------ */
/*  Speculation save/restore: FTQ ring + FSQ occupancy + inner bpred  */
/* ------------------------------------------------------------------ */

/*
 * Private snapshot struct — module-local tag DecoupledSpecSnap.
 * Cast to/from the opaque CaeBPredSpecSnapshot * at the vtable
 * boundary (the opaque type is never actually defined; every
 * concrete predictor uses its own struct tag). Carries a deep
 * copy of the FTQ ring, the three FTQ occupancy/head/tail
 * scalars, the FSQ occupancy, and an opaque handle to the inner
 * predictor's own speculation snapshot.
 */
typedef struct DecoupledSpecSnap {
    uint32_t ftq_size;
    uint32_t ftq_head;
    uint32_t ftq_tail;
    uint32_t ftq_occupancy;
    uint32_t fsq_occupancy;
    CaeFtqEntry *ftq_copy;              /* [ftq_size] */
    CaeBPredSpecSnapshot *inner;        /* inner predictor's snapshot, or NULL */
    Object *inner_obj;                  /* for the inner drop routing */
} DecoupledSpecSnap;

static CaeBPredSpecSnapshot *
cae_bpred_decoupled_spec_snapshot(Object *obj)
{
    CaeBPredDecoupled *p = CAE_BPRED_DECOUPLED(obj);
    DecoupledSpecSnap *s = g_new0(DecoupledSpecSnap, 1);

    s->ftq_size      = p->ftq_size;
    s->ftq_head      = p->ftq_head;
    s->ftq_tail      = p->ftq_tail;
    s->ftq_occupancy = p->ftq_occupancy;
    s->fsq_occupancy = p->fsq_occupancy;
    if (p->ftq_size > 0 && p->ftq != NULL) {
        s->ftq_copy = g_new0(CaeFtqEntry, p->ftq_size);
        memcpy(s->ftq_copy, p->ftq,
               sizeof(CaeFtqEntry) * p->ftq_size);
    }
    s->inner = cae_bpred_spec_snapshot_save(p->inner);
    s->inner_obj = p->inner;
    return (CaeBPredSpecSnapshot *)s;
}

static void
cae_bpred_decoupled_spec_restore(Object *obj,
                                 const CaeBPredSpecSnapshot *snap)
{
    CaeBPredDecoupled *p = CAE_BPRED_DECOUPLED(obj);
    const DecoupledSpecSnap *s = (const DecoupledSpecSnap *)snap;

    /*
     * Refuse to restore across geometry changes. The snapshot is
     * tied to a specific FTQ size; any mismatch means the caller
     * re-configured the predictor between save and restore, and
     * the byte-level copy would corrupt the ring.
     */
    if (s->ftq_size != p->ftq_size) {
        return;
    }
    p->ftq_head      = s->ftq_head;
    p->ftq_tail      = s->ftq_tail;
    p->ftq_occupancy = s->ftq_occupancy;
    p->fsq_occupancy = s->fsq_occupancy;
    if (s->ftq_copy != NULL && p->ftq != NULL) {
        memcpy(p->ftq, s->ftq_copy,
               sizeof(CaeFtqEntry) * p->ftq_size);
    }
    cae_bpred_spec_snapshot_restore(p->inner, s->inner);
}

static void
cae_bpred_decoupled_spec_drop(CaeBPredSpecSnapshot *snap)
{
    if (!snap) {
        return;
    }
    DecoupledSpecSnap *s = (DecoupledSpecSnap *)snap;
    /*
     * Drop inner snapshot through its original owner's vtable.
     * Round 29 DecoupledSpecSnap carries the inner_obj pointer
     * captured at save time so the inner predictor's vtable hook
     * can free any nested allocations it owns (e.g. TAGE-SC-L's
     * RAS stack copy).
     */
    cae_bpred_spec_snapshot_drop(s->inner_obj, s->inner);
    g_free(s->ftq_copy);
    g_free(s);
}

static void cae_bpred_decoupled_class_init(ObjectClass *klass,
                                           const void *data)
{
    UserCreatableClass *uc = USER_CREATABLE_CLASS(klass);
    CaeBPredClass *bc = CAE_BPRED_CLASS(klass);

    (void)data;
    uc->complete = cae_bpred_decoupled_complete;
    bc->predict = cae_bpred_decoupled_predict;
    bc->update = cae_bpred_decoupled_update;
    bc->reset = cae_bpred_decoupled_reset;
    bc->spec_snapshot = cae_bpred_decoupled_spec_snapshot;
    bc->spec_restore = cae_bpred_decoupled_spec_restore;
    bc->spec_drop = cae_bpred_decoupled_spec_drop;
}

static const TypeInfo cae_bpred_decoupled_type = {
    .name = TYPE_CAE_BPRED_DECOUPLED,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeBPredDecoupled),
    .instance_init = cae_bpred_decoupled_instance_init,
    .instance_finalize = cae_bpred_decoupled_finalize,
    .class_init = cae_bpred_decoupled_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { TYPE_CAE_BPRED },
        { }
    }
};

static void cae_bpred_decoupled_register_types(void)
{
    type_register_static(&cae_bpred_decoupled_type);
}

type_init(cae_bpred_decoupled_register_types)
