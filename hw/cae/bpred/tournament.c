/*
 * CAE tournament branch predictor.
 *
 * Composite predictor layered on top of the existing bpred interface:
 *   - local:  per-PC 2-bit saturating counter table (same as
 *             2bit-local, sized via local_history_bits);
 *   - gshare: global-history XOR PC into a 2-bit counter table
 *             (sized via global_history_bits);
 *   - chooser: 2-bit counter per-PC selecting between local and
 *             gshare on each prediction.
 *
 * Target prediction reuses the internal BTB + RAS from hw/cae/bpred/
 * to stay consistent with the in-order track's mispredict semantics.
 * The CaeBPredClass interface is unchanged, so the charge path in
 * cae_charge_executed_tb treats this predictor identically to the
 * 2-bit-local one (BL-20260418-bpred-cold-btb-taken-is-miss applies).
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
#include "hw/cae/bpred/btb.h"
#include "hw/cae/bpred/ras.h"

#define TYPE_CAE_BPRED_TOURNAMENT "cae-bpred-tournament"

OBJECT_DECLARE_SIMPLE_TYPE(CaeBPredTournament, CAE_BPRED_TOURNAMENT)

/* 2-bit saturating-counter encoding — same as 2bit-local. */
#define CAE_TOURN_COUNTER_INIT 1   /* weak not-taken */
#define CAE_TOURN_CHOOSER_INIT 1   /* weak-prefer-local */

struct CaeBPredTournament {
    Object parent;

    /* Config. */
    uint32_t local_history_bits;
    uint32_t global_history_bits;
    uint32_t btb_entries;
    uint32_t btb_assoc;
    uint32_t ras_depth;
    uint32_t mispredict_penalty_cycles;

    /* Derived. */
    uint32_t num_local_counters;
    uint32_t num_gshare_counters;
    uint32_t num_chooser_counters;
    uint32_t gshare_history_mask;

    uint8_t *local_counters;
    uint8_t *gshare_counters;
    uint8_t *chooser_counters;

    /* Global branch-direction history, least-significant bit = most
     * recent outcome. */
    uint64_t ghr;

    CaeBtb btb;
    CaeRas ras;

    /* Stats. */
    uint64_t predictions;
    uint64_t mispredictions;
    uint64_t chose_local;
    uint64_t chose_gshare;

    bool initialised;
};

static inline uint32_t local_index(const CaeBPredTournament *p, uint64_t pc)
{
    return (uint32_t)((pc >> 2) & (p->num_local_counters - 1));
}

static inline uint32_t gshare_index(const CaeBPredTournament *p,
                                    uint64_t pc)
{
    uint32_t pc_bits = (uint32_t)((pc >> 2) & p->gshare_history_mask);
    uint32_t hist = (uint32_t)(p->ghr & p->gshare_history_mask);
    return pc_bits ^ hist;
}

static inline bool counter_predicts_taken(uint8_t counter)
{
    return counter >= 2;
}

static inline uint8_t counter_update(uint8_t counter, bool actual_taken)
{
    if (actual_taken) {
        return counter == 3 ? 3 : counter + 1;
    }
    return counter == 0 ? 0 : counter - 1;
}

static CaeBPredPrediction cae_bpred_tournament_predict(
    Object *obj, const CaeBPredQuery *q)
{
    CaeBPredTournament *p = CAE_BPRED_TOURNAMENT(obj);
    CaeBPredPrediction resp = {
        .target_pc = q->fallthrough_pc,
        .taken = false,
        .target_known = true,
    };
    uint64_t target = 0;
    bool taken_local;
    bool taken_gshare;
    bool taken;
    uint32_t li;
    uint32_t gi;
    uint32_t ci;

    if (!p->initialised || !q->is_conditional) {
        /* Unconditional branches: defer target to BTB/RAS. */
        if (q->is_return && p->ras.depth > 0 &&
            cae_ras_peek(&p->ras, &target)) {
            resp.taken = true;
            resp.target_pc = target;
            resp.target_known = true;
            return resp;
        }
        if (cae_btb_lookup(&p->btb, q->pc, &target)) {
            resp.taken = true;
            resp.target_pc = target;
            resp.target_known = true;
            return resp;
        }
        resp.taken = true;
        resp.target_pc = q->fallthrough_pc;
        resp.target_known = false;
        return resp;
    }

    li = local_index(p, q->pc);
    gi = gshare_index(p, q->pc);
    ci = li % p->num_chooser_counters;

    taken_local = counter_predicts_taken(p->local_counters[li]);
    taken_gshare = counter_predicts_taken(p->gshare_counters[gi]);
    /* chooser >= 2 prefers gshare. */
    if (counter_predicts_taken(p->chooser_counters[ci])) {
        taken = taken_gshare;
    } else {
        taken = taken_local;
    }
    resp.taken = taken;
    if (taken) {
        if (cae_btb_lookup(&p->btb, q->pc, &target)) {
            resp.target_pc = target;
            resp.target_known = true;
        } else {
            resp.target_pc = q->fallthrough_pc;
            resp.target_known = false;
        }
    } else {
        resp.target_pc = q->fallthrough_pc;
        resp.target_known = true;
    }
    qatomic_set(&p->predictions,
                    qatomic_read(&p->predictions) + 1);
    return resp;
}

static void cae_bpred_tournament_update(Object *obj,
                                        const CaeBPredResolve *r)
{
    CaeBPredTournament *p = CAE_BPRED_TOURNAMENT(obj);
    uint32_t li;
    uint32_t gi;
    uint32_t ci;
    bool local_correct;
    bool gshare_correct;

    if (!p->initialised) {
        return;
    }
    if (r->is_conditional) {
        li = local_index(p, r->pc);
        gi = gshare_index(p, r->pc);
        ci = li % p->num_chooser_counters;

        local_correct = (counter_predicts_taken(p->local_counters[li])
                         == r->actual_taken);
        gshare_correct = (counter_predicts_taken(p->gshare_counters[gi])
                          == r->actual_taken);

        p->local_counters[li] = counter_update(p->local_counters[li],
                                               r->actual_taken);
        p->gshare_counters[gi] = counter_update(p->gshare_counters[gi],
                                                r->actual_taken);

        /* Chooser follows the winner (or stays put on tie). */
        if (local_correct != gshare_correct) {
            bool prefer_gshare = gshare_correct;
            p->chooser_counters[ci] = counter_update(
                p->chooser_counters[ci], prefer_gshare);
            if (prefer_gshare) {
                p->chose_gshare++;
            } else {
                p->chose_local++;
            }
        }

        /* Advance global history. */
        p->ghr = ((p->ghr << 1) | (r->actual_taken ? 1u : 0u));
    }

    /* Target learning via BTB + RAS. Mirror the 2bit-local update
     * rules so the in-order track's mispredict accounting stays
     * consistent. */
    if (r->actual_taken && r->actual_target != 0) {
        cae_btb_insert(&p->btb, r->pc, r->actual_target);
    }
    if (r->is_call) {
        cae_ras_push(&p->ras, r->pc + r->insn_bytes);
    }
    if (r->is_return) {
        uint64_t ret_target;
        (void)cae_ras_pop(&p->ras, &ret_target);
    }
}

/*
 * The charge-path mispredict-penalty reader is not part of the
 * CaeBPredClass interface in this tree; it is exposed as a QOM
 * integer property on the concrete predictor object and read by
 * cae_charge_executed_tb via object_property_get_uint. Nothing to
 * vtable-hook here.
 */

static void cae_bpred_tournament_complete(UserCreatable *uc, Error **errp)
{
    CaeBPredTournament *p = CAE_BPRED_TOURNAMENT(uc);
    uint32_t lhb = p->local_history_bits ? p->local_history_bits : 10u;
    uint32_t ghb = p->global_history_bits ? p->global_history_bits : 12u;

    if (lhb > 20u || ghb > 20u) {
        error_setg(errp, "cae-bpred-tournament: history-bits must be <= 20 "
                   "(got local=%u, global=%u)", lhb, ghb);
        return;
    }
    p->local_history_bits = lhb;
    p->global_history_bits = ghb;
    p->num_local_counters = 1u << lhb;
    p->num_gshare_counters = 1u << ghb;
    p->num_chooser_counters = p->num_local_counters;
    p->gshare_history_mask = p->num_gshare_counters - 1u;

    p->local_counters = g_new0(uint8_t, p->num_local_counters);
    p->gshare_counters = g_new0(uint8_t, p->num_gshare_counters);
    p->chooser_counters = g_new0(uint8_t, p->num_chooser_counters);
    for (uint32_t i = 0; i < p->num_local_counters; i++) {
        p->local_counters[i] = CAE_TOURN_COUNTER_INIT;
    }
    for (uint32_t i = 0; i < p->num_gshare_counters; i++) {
        p->gshare_counters[i] = CAE_TOURN_COUNTER_INIT;
    }
    for (uint32_t i = 0; i < p->num_chooser_counters; i++) {
        p->chooser_counters[i] = CAE_TOURN_CHOOSER_INIT;
    }
    if (!cae_btb_init(&p->btb,
                      p->btb_entries ? p->btb_entries : 64u,
                      p->btb_assoc ? p->btb_assoc : 2u,
                      errp)) {
        return;
    }
    if (!cae_ras_init(&p->ras, p->ras_depth ? p->ras_depth : 16u, errp)) {
        return;
    }
    p->ghr = 0;
    p->initialised = true;
}

static void cae_bpred_tournament_finalize(Object *obj)
{
    CaeBPredTournament *p = CAE_BPRED_TOURNAMENT(obj);
    g_free(p->local_counters);
    g_free(p->gshare_counters);
    g_free(p->chooser_counters);
    cae_btb_release(&p->btb);
    cae_ras_release(&p->ras);
}

static void cae_bpred_tournament_stat_get(Object *obj, Visitor *v,
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

static void cae_bpred_tournament_instance_init(Object *obj)
{
    CaeBPredTournament *p = CAE_BPRED_TOURNAMENT(obj);
    p->local_history_bits = 10u;
    p->global_history_bits = 12u;
    p->btb_entries = 64u;
    p->btb_assoc = 2u;
    p->ras_depth = 16u;
    p->mispredict_penalty_cycles = 7u;
    object_property_add_uint32_ptr(obj, "local-history-bits",
                                   &p->local_history_bits,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "global-history-bits",
                                   &p->global_history_bits,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "btb-entries", &p->btb_entries,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "btb-assoc", &p->btb_assoc,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ras-depth", &p->ras_depth,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "mispredict-penalty-cycles",
                                   &p->mispredict_penalty_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "predictions", "uint64",
                        cae_bpred_tournament_stat_get, NULL, NULL,
                        &p->predictions);
    object_property_add(obj, "mispredictions", "uint64",
                        cae_bpred_tournament_stat_get, NULL, NULL,
                        &p->mispredictions);
    object_property_add(obj, "chose-local", "uint64",
                        cae_bpred_tournament_stat_get, NULL, NULL,
                        &p->chose_local);
    object_property_add(obj, "chose-gshare", "uint64",
                        cae_bpred_tournament_stat_get, NULL, NULL,
                        &p->chose_gshare);
}

static void cae_bpred_tournament_class_init(ObjectClass *klass,
                                            const void *data)
{
    UserCreatableClass *uc = USER_CREATABLE_CLASS(klass);
    CaeBPredClass *bc = CAE_BPRED_CLASS(klass);

    (void)data;
    uc->complete = cae_bpred_tournament_complete;
    bc->predict = cae_bpred_tournament_predict;
    bc->update = cae_bpred_tournament_update;
}

static const TypeInfo cae_bpred_tournament_type = {
    .name = TYPE_CAE_BPRED_TOURNAMENT,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeBPredTournament),
    .instance_init = cae_bpred_tournament_instance_init,
    .instance_finalize = cae_bpred_tournament_finalize,
    .class_init = cae_bpred_tournament_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { TYPE_CAE_BPRED },
        { }
    }
};

static void cae_bpred_tournament_register_types(void)
{
    type_register_static(&cae_bpred_tournament_type);
}

type_init(cae_bpred_tournament_register_types)
