/*
 * CAE (Cycle Approximate Engine) - Branch Predictor Interface
 *
 * Arch-neutral QOM interface for direction + target prediction.
 * Concrete implementations live in hw/cae/bpred/.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_BPRED_H
#define CAE_BPRED_H

#include "qom/object.h"
#include <stdint.h>
#include <stdbool.h>

#define TYPE_CAE_BPRED "cae-bpred"

typedef struct CaeBPredClass CaeBPredClass;
DECLARE_CLASS_CHECKERS(CaeBPredClass, CAE_BPRED, TYPE_CAE_BPRED)

/*
 * Branch query: describes the branch instruction about to retire.
 * The caller fills in everything; the predictor consumes it.
 */
typedef struct CaeBPredQuery {
    uint64_t pc;                /* branch PC                            */
    uint64_t fallthrough_pc;    /* sequential next PC (pc + insn bytes) */
    bool is_conditional;        /* direct/indirect unconditional: false */
    bool is_call;               /* JAL(R) with link (ra)                */
    bool is_return;             /* JALR rd=x0 rs1=ra (return)           */
    bool is_indirect;           /* target not known at decode           */
} CaeBPredQuery;

typedef struct CaeBPredPrediction {
    uint64_t target_pc;         /* predicted next PC                    */
    bool taken;                 /* predicted direction (unconditional=true) */
    bool target_known;          /* BTB/RAS produced a target            */
} CaeBPredPrediction;

typedef struct CaeBPredResolve {
    uint64_t pc;                /* branch PC (same as the query)        */
    uint64_t actual_target;     /* resolved target                      */
    uint8_t insn_bytes;         /* 2 (RVC) or 4 (standard) — must be   */
                                /* set by caller so RAS push of         */
                                /* pc + insn_bytes is correct           */
    bool actual_taken;          /* resolved direction                   */
    bool is_call;
    bool is_return;
    bool is_conditional;
    bool is_indirect;
} CaeBPredResolve;

/*
 * Opaque speculation snapshot owned by the concrete predictor
 * implementation. cae-bpred-decoupled chains through to its inner
 * predictor's snapshot so DecoupledBPU FTQ / FSQ state and the
 * inner TAGE-SC-L / tournament history can be rolled back
 * together on a wrong-path squash.
 */
typedef struct CaeBPredSpecSnapshot CaeBPredSpecSnapshot;

struct CaeBPredClass {
    InterfaceClass parent_class;

    /*
     * Query prediction for a branch about to retire. Must be pure /
     * side-effect-free with respect to learning state (no counter bump,
     * no BTB insertion) — state updates happen through ->update().
     */
    CaeBPredPrediction (*predict)(Object *obj, const CaeBPredQuery *q);

    /*
     * Update predictor learning state with the resolved branch outcome.
     * Concrete predictors should also allocate BTB / RAS entries here as
     * appropriate.
     */
    void (*update)(Object *obj, const CaeBPredResolve *r);

    /*
     * Optional: reset learning state to initial values. If NULL, the
     * interface helper treats reset as a no-op.
     */
    void (*reset)(Object *obj);

    /*
     * Optional speculation save/restore. Predictors that carry
     * hidden history state (global/path history registers, return
     * address stacks, FTQ/FSQ contents, etc.) implement these to
     * unwind cleanly on a wrong-path squash. Predictors with no
     * hidden history (e.g., 2bit-local or tournament that only
     * mutate on retire) leave these NULL — the arch-neutral
     * dispatch treats a NULL vtable entry as a safe no-op (save
     * returns NULL, restore skips, drop skips).
     */
    CaeBPredSpecSnapshot *(*spec_snapshot)(Object *obj);
    void (*spec_restore)(Object *obj, const CaeBPredSpecSnapshot *snap);
    void (*spec_drop)(CaeBPredSpecSnapshot *snap);
};

/* Convenience dispatchers. Return defaults if obj is NULL. */
CaeBPredPrediction cae_bpred_predict(Object *obj, const CaeBPredQuery *q);
void cae_bpred_update(Object *obj, const CaeBPredResolve *r);
void cae_bpred_reset(Object *obj);

/*
 * Return cycle penalty for a mispredict given the predicted vs actual
 * query/resolve pair. Simple model: taken-vs-not-taken mismatch or
 * target mismatch contribute `penalty_cycles`; unconditional hits that
 * the BTB missed also cost `penalty_cycles`. Implementations of
 * predictors do not need to override this; cpu models call it directly.
 */
bool cae_bpred_is_mispredict(const CaeBPredPrediction *p,
                             const CaeBPredResolve *r);

/*
 * Arch-neutral speculation save/restore dispatch. Each routes
 * through the registered predictor's vtable hook if set, else
 * degrades to a safe no-op (NULL on save; no-op on restore/drop).
 * Callers may compose this into a larger checkpoint snapshot
 * without caring whether the concrete predictor carries hidden
 * history.
 */
CaeBPredSpecSnapshot *cae_bpred_spec_snapshot_save(Object *obj);
void cae_bpred_spec_snapshot_restore(Object *obj,
                                     const CaeBPredSpecSnapshot *snap);
void cae_bpred_spec_snapshot_drop(Object *obj,
                                  CaeBPredSpecSnapshot *snap);

#endif /* CAE_BPRED_H */
