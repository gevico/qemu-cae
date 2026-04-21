/*
 * CAE TAGE-SC-L branch predictor (Round 13 M4' deliverable).
 *
 * TAgged GEometric-history predictor with Statistical Corrector
 * and Loop predictor. The structural layout follows Seznec's
 * ISCA-2016 TAGE-SC-L description and what XS-GEM5's KMH-V3
 * calibrates against at configs/example/kmhv3.py — bimodal base
 * plus N tagged components, an SC corrector, and a short loop
 * predictor. The round-13 implementation is a functionally
 * correct skeleton wide enough for M5' calibration work to tune
 * via QOM properties without source edits:
 *
 *   - bimodal base: 2-bit saturating counters indexed by PC.
 *   - N tagged components: each a direct-mapped table of
 *     {tag, ctr, useful}; component i is indexed by
 *     PC XOR folded(GHR, L_i) and the tag is another fold of
 *     the same history. L_i grows geometrically so longer-
 *     history contexts override shorter ones on a tag hit.
 *   - Statistical Corrector: wide 5-bit signed counters
 *     indexed by PC + low GHR bits; inverts TAGE's direction
 *     when their weighted sum crosses a threshold.
 *   - Loop predictor: small PC-indexed table tracking
 *     backward-branch trip counts; overrides the rest of the
 *     stack when confidence is high.
 *   - BTB + RAS: shared mechanics with the tournament
 *     predictor (hw/cae/bpred/btb.h / ras.h) so the M4' track
 *     keeps the same target-prediction semantics as the
 *     in-order track. BL-20260418-bpred-cold-btb-taken-is-
 *     miss still applies: taken conditional with
 *     target_known=false is a mispredict.
 *
 * Defaults are sized for the kmhv3.py realspec config:
 *   bimodal-entries    = 4096
 *   num-tage-tables    = 4
 *   tage-entries       = 1024 per component
 *   tage-tag-bits      = 11
 *   sc-entries         = 512
 *   loop-entries       = 64
 *   ras-depth          = 16
 * Each is a READWRITE QOM property so per-benchmark paired
 * YAMLs (tests/cae-difftest/configs/xs-1c-realspec.yaml, M5'
 * kmhv3.yaml) can override without code changes.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "qemu/atomic.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "cae/bpred.h"
#include "hw/cae/bpred/btb.h"
#include "hw/cae/bpred/ras.h"

#define TYPE_CAE_BPRED_TAGE_SC_L "cae-bpred-tage-sc-l"

OBJECT_DECLARE_SIMPLE_TYPE(CaeBPredTageScL, CAE_BPRED_TAGE_SC_L)

/* 2-bit saturating counter, weak-not-taken init. */
#define CAE_TAGE_BASE_CTR_INIT      1u
/* 3-bit signed counter for tagged tables: [-4, 3]. */
#define CAE_TAGE_CTR_MIN            (-4)
#define CAE_TAGE_CTR_MAX            ( 3)
#define CAE_TAGE_CTR_INIT           0
/* 2-bit useful counter. */
#define CAE_TAGE_USEFUL_MAX         3u
/* 5-bit signed SC counter: [-16, 15]. */
#define CAE_TAGE_SC_MIN             (-16)
#define CAE_TAGE_SC_MAX             ( 15)
/* Loop predictor state machine. */
#define CAE_TAGE_LOOP_CONF_MAX      3u
/* Max tables the struct sizes for — static cap keeps struct
 * fixed-size, num_tage_tables clamps effective use below. */
#define CAE_TAGE_MAX_TABLES         8u

/*
 * Per-tagged-component entry. Kept small so M5' calibration can
 * push tage-entries into four-digit range without blowing RAM.
 */
typedef struct CaeTageEntry {
    uint16_t tag;
    int8_t   ctr;      /* 3-bit signed saturating */
    uint8_t  useful;   /* 2-bit saturating */
    uint8_t  valid;    /* distinguishes zero-init from a real 0 tag */
} CaeTageEntry;

typedef struct CaeTageLoop {
    uint32_t tag;
    uint16_t current_iter;
    uint16_t trip;
    uint8_t  confidence;
    uint8_t  age;
    bool     valid;
} CaeTageLoop;

struct CaeBPredTageScL {
    Object parent;

    /* User-tunable. */
    uint32_t bimodal_entries;
    uint32_t num_tage_tables;
    uint32_t tage_entries;        /* per component */
    uint32_t tage_tag_bits;
    uint32_t tage_min_hist;
    uint32_t tage_max_hist;
    uint32_t sc_entries;
    uint32_t loop_entries;
    uint32_t btb_entries;
    uint32_t btb_assoc;
    uint32_t ras_depth;
    uint32_t mispredict_penalty_cycles;

    /* Derived. */
    uint8_t *bimodal;               /* bimodal_entries */
    CaeTageEntry *tage[CAE_TAGE_MAX_TABLES];
    uint16_t hist_lengths[CAE_TAGE_MAX_TABLES];
    uint32_t tage_index_mask;       /* tage_entries - 1 */
    uint32_t tage_tag_mask;
    uint32_t bimodal_mask;
    int8_t *sc_table;               /* sc_entries, signed 5-bit */
    uint32_t sc_mask;
    CaeTageLoop *loop;              /* loop_entries */
    uint32_t loop_mask;

    /* History. */
    uint64_t ghr;                   /* 64-bit global history */
    uint64_t phr;                   /* path history */

    CaeBtb btb;
    CaeRas ras;

    /* Stats. */
    uint64_t predictions;
    uint64_t mispredictions;
    uint64_t tage_hits[CAE_TAGE_MAX_TABLES];
    uint64_t sc_inversions;
    uint64_t loop_overrides;
    uint64_t allocations;
    uint64_t alloc_stalls;

    bool initialised;
};

/* --- Utility: round up to power-of-two and fit a mask. ---------- */
static inline uint32_t round_up_pow2(uint32_t v)
{
    if (v <= 1u) {
        return 1u;
    }
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1u;
}

/*
 * Folded-history helper. Reduces a long GHR slice to the target
 * width by XOR-folding — classical TAGE index / tag derivation.
 * For the round-13 skeleton we keep it cheap: compute at
 * lookup/update time instead of maintaining incremental folded
 * histories. That's correct but slower than gem5's KMH-V3
 * production code; the M5' calibration path can swap in
 * incremental folds if calibration exposes a hot spot.
 */
static uint32_t fold_history(uint64_t ghr, uint32_t hist_len,
                             uint32_t target_width)
{
    if (target_width == 0u || hist_len == 0u) {
        return 0u;
    }
    uint64_t mask = (hist_len >= 64u) ? UINT64_MAX
                                      : ((1ULL << hist_len) - 1ULL);
    uint64_t slice = ghr & mask;
    uint32_t tgt_mask = (target_width >= 32u) ? UINT32_MAX
                                              : ((1u << target_width) - 1u);
    uint32_t fold = 0u;
    while (slice != 0u) {
        fold ^= (uint32_t)(slice & tgt_mask);
        slice >>= target_width;
    }
    return fold & tgt_mask;
}

static inline uint32_t bimodal_index(const CaeBPredTageScL *p, uint64_t pc)
{
    return (uint32_t)((pc >> 2) & p->bimodal_mask);
}

static inline uint32_t tage_index(const CaeBPredTageScL *p, uint32_t t,
                                  uint64_t pc)
{
    /*
     * BS-31 round-14 fix: fold width is `log2(tage_entries)`,
     * the number of index bits. Round 13 used
     * `ctz32(tage_entries | 1u)` which forces the low bit and
     * makes ctz32 return 0 for every power-of-two size — the
     * folded history was therefore always 0 and every tagged
     * component collapsed to pure PC indexing. After
     * round_up_pow2 in complete(), tage_entries is a nonzero
     * power of two, so ctz32(tage_entries) gives log2 directly.
     */
    uint32_t pc_bits = (uint32_t)((pc >> 2) & p->tage_index_mask);
    uint32_t hist = fold_history(p->ghr, p->hist_lengths[t],
                                 ctz32(p->tage_entries));
    return (pc_bits ^ hist) & p->tage_index_mask;
}

static inline uint16_t tage_tag(const CaeBPredTageScL *p, uint32_t t,
                                uint64_t pc)
{
    uint32_t pc_bits = (uint32_t)((pc >> 4) & p->tage_tag_mask);
    uint32_t hist = fold_history(p->ghr, p->hist_lengths[t],
                                 p->tage_tag_bits);
    return (uint16_t)((pc_bits ^ hist) & p->tage_tag_mask);
}

static inline uint32_t sc_index(const CaeBPredTageScL *p, uint64_t pc)
{
    uint32_t pc_bits = (uint32_t)((pc >> 3) & p->sc_mask);
    uint32_t hist = (uint32_t)(p->ghr & p->sc_mask);
    return (pc_bits ^ hist) & p->sc_mask;
}

static inline uint32_t loop_index(const CaeBPredTageScL *p, uint64_t pc)
{
    return (uint32_t)((pc >> 2) & p->loop_mask);
}

static inline bool base_predicts_taken(uint8_t ctr)
{
    return ctr >= 2;
}

static inline int8_t sat_incr_s3(int8_t v)
{
    return v < CAE_TAGE_CTR_MAX ? (int8_t)(v + 1) : (int8_t)CAE_TAGE_CTR_MAX;
}
static inline int8_t sat_decr_s3(int8_t v)
{
    return v > CAE_TAGE_CTR_MIN ? (int8_t)(v - 1) : (int8_t)CAE_TAGE_CTR_MIN;
}
static inline int8_t sat_incr_s5(int8_t v)
{
    return v < CAE_TAGE_SC_MAX ? (int8_t)(v + 1) : (int8_t)CAE_TAGE_SC_MAX;
}
static inline int8_t sat_decr_s5(int8_t v)
{
    return v > CAE_TAGE_SC_MIN ? (int8_t)(v - 1) : (int8_t)CAE_TAGE_SC_MIN;
}
static inline uint8_t sat_incr_u2(uint8_t v)
{
    return v < CAE_TAGE_USEFUL_MAX ? (uint8_t)(v + 1u)
                                   : (uint8_t)CAE_TAGE_USEFUL_MAX;
}
static inline uint8_t sat_decr_u2(uint8_t v)
{
    return v > 0u ? (uint8_t)(v - 1u) : 0u;
}

/*
 * TAGE lookup: returns the longest-history component whose tag
 * matches at this PC, with the predicted direction and the
 * component index. `out_component` is -1 when no tag hit (use
 * bimodal base).
 */
static bool tage_lookup(const CaeBPredTageScL *p, uint64_t pc,
                        int *out_component, bool *out_taken)
{
    for (int t = (int)p->num_tage_tables - 1; t >= 0; t--) {
        uint32_t idx = tage_index(p, (uint32_t)t, pc);
        uint16_t tag = tage_tag(p, (uint32_t)t, pc);
        if (p->tage[t][idx].valid && p->tage[t][idx].tag == tag) {
            *out_component = t;
            *out_taken = p->tage[t][idx].ctr >= 0;
            return true;
        }
    }
    *out_component = -1;
    *out_taken = base_predicts_taken(
        p->bimodal[bimodal_index(p, pc)]);
    return false;
}

/*
 * Statistical Corrector: reads one wide counter and flips the
 * TAGE verdict when the counter's sign contradicts. Round-13
 * uses a single-table SC (Seznec's full SC uses several); the
 * single-table skeleton is enough for AC-K-4 and the M5'
 * calibration path can grow it.
 */
static bool sc_correct(const CaeBPredTageScL *p, uint64_t pc,
                       bool tage_pred, bool *out_inverted)
{
    int8_t sc = p->sc_table[sc_index(p, pc)];
    /* Signal to invert when SC strongly disagrees. Threshold 4
     * keeps noise low on weakly-biased counters. */
    if (tage_pred && sc <= -4) {
        *out_inverted = true;
        return false;
    }
    if (!tage_pred && sc >= 4) {
        *out_inverted = true;
        return true;
    }
    *out_inverted = false;
    return tage_pred;
}

/*
 * Loop predictor: if the PC's entry has confidence == max and
 * we are within the tracked trip count, return the tracked
 * direction (taken until the last iteration). Else defer to
 * the TAGE/SC layers.
 */
static bool loop_lookup(const CaeBPredTageScL *p, uint64_t pc,
                        bool *out_override, bool *out_taken)
{
    const CaeTageLoop *e = &p->loop[loop_index(p, pc)];
    uint32_t tag = (uint32_t)((pc >> 6) & 0xffffffffu);
    if (!e->valid || e->tag != tag
        || e->confidence < CAE_TAGE_LOOP_CONF_MAX) {
        *out_override = false;
        return false;
    }
    /* Last iteration — break the loop. */
    if (e->current_iter + 1u >= e->trip) {
        *out_override = true;
        *out_taken = false;
        return true;
    }
    *out_override = true;
    *out_taken = true;
    return true;
}

/* --- CaeBPredClass predict/update ------------------------------- */

static CaeBPredPrediction cae_bpred_tage_sc_l_predict(
    Object *obj, const CaeBPredQuery *q)
{
    CaeBPredTageScL *p = CAE_BPRED_TAGE_SC_L(obj);
    CaeBPredPrediction resp = {
        .target_pc = q->fallthrough_pc,
        .taken = false,
        .target_known = true,
    };
    uint64_t target = 0;

    if (!p->initialised || !q->is_conditional) {
        /* Target defers to RAS for returns, BTB for everything
         * else. Matches tournament.c / 2bit_local.c. */
        if (q->is_return && p->ras.depth > 0
            && cae_ras_peek(&p->ras, &target)) {
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

    /* Conditional branch — full TAGE stack. */
    int comp = -1;
    bool tage_taken = false;
    bool hit = tage_lookup(p, q->pc, &comp, &tage_taken);

    /* SC layer inverts TAGE when SC counter disagrees. */
    bool sc_inverted = false;
    bool sc_taken = sc_correct(p, q->pc, tage_taken, &sc_inverted);

    /* Loop predictor overrides when confident. */
    bool loop_override = false;
    bool loop_taken = false;
    bool loop_hit = loop_lookup(p, q->pc, &loop_override, &loop_taken);

    bool taken = loop_hit ? loop_taken : sc_taken;

    if (loop_override) {
        qatomic_set(&p->loop_overrides,
                        qatomic_read(&p->loop_overrides) + 1);
    } else if (sc_inverted) {
        qatomic_set(&p->sc_inversions,
                        qatomic_read(&p->sc_inversions) + 1);
    } else if (hit) {
        qatomic_set(&p->tage_hits[comp],
                        qatomic_read(&p->tage_hits[comp]) + 1);
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

static void tage_allocate_longer(CaeBPredTageScL *p, uint64_t pc,
                                 int prev_component, bool actual_taken)
{
    /*
     * Round-13 allocation policy: walk components strictly
     * longer than the component that produced the prediction,
     * allocate the first entry with useful=0. If every
     * candidate is "useful", decrement all of them by one
     * (age) and record an alloc_stall stat.
     */
    int start = prev_component + 1;
    if (start >= (int)p->num_tage_tables) {
        return;
    }
    bool allocated = false;
    for (int t = start; t < (int)p->num_tage_tables; t++) {
        uint32_t idx = tage_index(p, (uint32_t)t, pc);
        if (!p->tage[t][idx].valid || p->tage[t][idx].useful == 0u) {
            p->tage[t][idx].tag = tage_tag(p, (uint32_t)t, pc);
            p->tage[t][idx].ctr = actual_taken ? (int8_t)0 : (int8_t)-1;
            p->tage[t][idx].useful = 0u;
            p->tage[t][idx].valid = 1u;
            allocated = true;
            qatomic_set(&p->allocations,
                            qatomic_read(&p->allocations) + 1);
            break;
        }
    }
    if (!allocated) {
        for (int t = start; t < (int)p->num_tage_tables; t++) {
            uint32_t idx = tage_index(p, (uint32_t)t, pc);
            p->tage[t][idx].useful = sat_decr_u2(p->tage[t][idx].useful);
        }
        qatomic_set(&p->alloc_stalls,
                        qatomic_read(&p->alloc_stalls) + 1);
    }
}

static void loop_update(CaeBPredTageScL *p, uint64_t pc,
                        bool actual_taken)
{
    CaeTageLoop *e = &p->loop[loop_index(p, pc)];
    uint32_t tag = (uint32_t)((pc >> 6) & 0xffffffffu);
    if (!e->valid || e->tag != tag) {
        /* Tentative allocate on backward branches that are taken
         * (first iteration of a potential loop). */
        if (actual_taken) {
            e->valid = true;
            e->tag = tag;
            e->current_iter = 1u;
            e->trip = 0u;
            e->confidence = 0u;
            e->age = 0u;
        }
        return;
    }
    if (actual_taken) {
        e->current_iter++;
        if (e->current_iter > 4096u) {
            /* Drop clearly-not-a-loop entries. */
            e->valid = false;
        }
    } else {
        if (e->trip == 0u) {
            e->trip = e->current_iter;
        } else if (e->trip == e->current_iter) {
            if (e->confidence < CAE_TAGE_LOOP_CONF_MAX) {
                e->confidence++;
            }
        } else {
            /* Trip mismatch — drop confidence. */
            e->confidence = 0u;
            e->trip = e->current_iter;
        }
        e->current_iter = 0u;
    }
}

static void cae_bpred_tage_sc_l_update(Object *obj,
                                       const CaeBPredResolve *r)
{
    CaeBPredTageScL *p = CAE_BPRED_TAGE_SC_L(obj);

    if (!p->initialised) {
        return;
    }

    if (r->is_conditional) {
        int comp = -1;
        bool tage_taken = false;
        bool hit = tage_lookup(p, r->pc, &comp, &tage_taken);

        /* Update the winning component's counter — or the
         * bimodal base if no tag hit. */
        if (hit) {
            uint32_t idx = tage_index(p, (uint32_t)comp, r->pc);
            p->tage[comp][idx].ctr =
                r->actual_taken
                ? sat_incr_s3(p->tage[comp][idx].ctr)
                : sat_decr_s3(p->tage[comp][idx].ctr);
            if (tage_taken == r->actual_taken) {
                p->tage[comp][idx].useful =
                    sat_incr_u2(p->tage[comp][idx].useful);
            } else {
                p->tage[comp][idx].useful =
                    sat_decr_u2(p->tage[comp][idx].useful);
                tage_allocate_longer(p, r->pc, comp, r->actual_taken);
            }
        } else {
            uint32_t idx = bimodal_index(p, r->pc);
            uint8_t c = p->bimodal[idx];
            if (r->actual_taken) {
                p->bimodal[idx] = (c < 3u) ? (uint8_t)(c + 1u) : 3u;
            } else {
                p->bimodal[idx] = (c > 0u) ? (uint8_t)(c - 1u) : 0u;
            }
            if (tage_taken != r->actual_taken) {
                /* Allocate a tagged entry so longer-history
                 * contexts can catch up. */
                tage_allocate_longer(p, r->pc, -1, r->actual_taken);
            }
        }

        /* SC update: nudge toward correct. */
        int8_t *sc = &p->sc_table[sc_index(p, r->pc)];
        *sc = r->actual_taken ? sat_incr_s5(*sc) : sat_decr_s5(*sc);

        /* Loop predictor: track trip counts. */
        loop_update(p, r->pc, r->actual_taken);

        /* Advance global history. */
        p->ghr = (p->ghr << 1) | (r->actual_taken ? 1ull : 0ull);
        p->phr = (p->phr << 1) | ((r->pc >> 2) & 1ull);

        if (tage_taken != r->actual_taken) {
            qatomic_set(&p->mispredictions,
                            qatomic_read(&p->mispredictions) + 1);
        }
    }

    /* Target learning: BTB + RAS. Same rules as tournament.c so
     * the M4' track shares mispredict accounting with the rest
     * of the bpred family. */
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

static void cae_bpred_tage_sc_l_reset(Object *obj)
{
    CaeBPredTageScL *p = CAE_BPRED_TAGE_SC_L(obj);
    if (!p->initialised) {
        return;
    }
    for (uint32_t i = 0; i < p->bimodal_entries; i++) {
        p->bimodal[i] = CAE_TAGE_BASE_CTR_INIT;
    }
    for (uint32_t t = 0; t < p->num_tage_tables; t++) {
        memset(p->tage[t], 0, sizeof(CaeTageEntry) * p->tage_entries);
    }
    for (uint32_t i = 0; i < p->sc_entries; i++) {
        p->sc_table[i] = 0;
    }
    memset(p->loop, 0, sizeof(CaeTageLoop) * p->loop_entries);
    /*
     * BS-32 round-14 fix: reset the embedded BTB and RAS too.
     * Round 13 cleared the TAGE / SC / loop tables and the
     * history registers but left target-prediction state alive,
     * breaking `CaeBPredClass::reset`'s "reset learning state to
     * initial values" contract.
     */
    cae_btb_reset(&p->btb);
    cae_ras_reset(&p->ras);
    p->ghr = 0;
    p->phr = 0;
    p->predictions = 0;
    p->mispredictions = 0;
    for (uint32_t t = 0; t < CAE_TAGE_MAX_TABLES; t++) {
        p->tage_hits[t] = 0;
    }
    p->sc_inversions = 0;
    p->loop_overrides = 0;
    p->allocations = 0;
    p->alloc_stalls = 0;
}

static void cae_bpred_tage_sc_l_complete(UserCreatable *uc, Error **errp)
{
    CaeBPredTageScL *p = CAE_BPRED_TAGE_SC_L(uc);

    if (p->num_tage_tables == 0u) {
        p->num_tage_tables = 4u;
    }
    if (p->num_tage_tables > CAE_TAGE_MAX_TABLES) {
        error_setg(errp,
                   "cae-bpred-tage-sc-l: num-tage-tables=%u exceeds "
                   "compile-time cap %u",
                   p->num_tage_tables, CAE_TAGE_MAX_TABLES);
        return;
    }
    if (p->tage_min_hist == 0u) {
        p->tage_min_hist = 5u;
    }
    if (p->tage_max_hist == 0u) {
        p->tage_max_hist = 64u;
    }
    if (p->tage_max_hist < p->tage_min_hist
        || p->tage_max_hist > 64u) {
        error_setg(errp,
                   "cae-bpred-tage-sc-l: tage history range invalid "
                   "(min=%u max=%u)", p->tage_min_hist, p->tage_max_hist);
        return;
    }

    p->bimodal_entries = round_up_pow2(p->bimodal_entries
                                       ? p->bimodal_entries : 4096u);
    p->tage_entries    = round_up_pow2(p->tage_entries
                                       ? p->tage_entries : 1024u);
    p->sc_entries      = round_up_pow2(p->sc_entries
                                       ? p->sc_entries : 512u);
    p->loop_entries    = round_up_pow2(p->loop_entries
                                       ? p->loop_entries : 64u);
    if (p->tage_tag_bits == 0u) {
        p->tage_tag_bits = 11u;
    }
    if (p->tage_tag_bits > 16u) {
        error_setg(errp, "cae-bpred-tage-sc-l: tage-tag-bits>16");
        return;
    }

    p->bimodal_mask = p->bimodal_entries - 1u;
    p->tage_index_mask = p->tage_entries - 1u;
    p->tage_tag_mask = (1u << p->tage_tag_bits) - 1u;
    p->sc_mask = p->sc_entries - 1u;
    p->loop_mask = p->loop_entries - 1u;

    /*
     * History schedule across tagged components. Linear
     * interpolation between tage_min_hist and tage_max_hist —
     * monotone increasing, good enough for the round-13
     * skeleton. The M5' calibration path can override by
     * editing the `tage-min-hist` / `tage-max-hist` /
     * `num-tage-tables` QOM knobs from the paired YAML; if the
     * geometric shape matters, that's a round-14+ extension.
     */
    if (p->num_tage_tables == 1u) {
        p->hist_lengths[0] = (uint16_t)p->tage_min_hist;
    } else {
        uint32_t span = p->tage_max_hist - p->tage_min_hist;
        for (uint32_t t = 0; t < p->num_tage_tables; t++) {
            p->hist_lengths[t] = (uint16_t)(
                p->tage_min_hist
                + (span * t) / (p->num_tage_tables - 1u));
        }
    }

    p->bimodal = g_new0(uint8_t, p->bimodal_entries);
    for (uint32_t i = 0; i < p->bimodal_entries; i++) {
        p->bimodal[i] = CAE_TAGE_BASE_CTR_INIT;
    }
    for (uint32_t t = 0; t < p->num_tage_tables; t++) {
        p->tage[t] = g_new0(CaeTageEntry, p->tage_entries);
    }
    p->sc_table = g_new0(int8_t, p->sc_entries);
    p->loop = g_new0(CaeTageLoop, p->loop_entries);

    if (!cae_btb_init(&p->btb,
                      p->btb_entries ? p->btb_entries : 64u,
                      p->btb_assoc ? p->btb_assoc : 2u,
                      errp)) {
        return;
    }
    if (!cae_ras_init(&p->ras, p->ras_depth ? p->ras_depth : 16u,
                      errp)) {
        return;
    }

    p->ghr = 0;
    p->phr = 0;
    p->initialised = true;
}

static void cae_bpred_tage_sc_l_finalize(Object *obj)
{
    CaeBPredTageScL *p = CAE_BPRED_TAGE_SC_L(obj);
    g_free(p->bimodal);
    for (uint32_t t = 0; t < CAE_TAGE_MAX_TABLES; t++) {
        g_free(p->tage[t]);
        p->tage[t] = NULL;
    }
    g_free(p->sc_table);
    g_free(p->loop);
    cae_btb_release(&p->btb);
    cae_ras_release(&p->ras);
}

static void cae_bpred_tage_sc_l_stat_get(Object *obj, Visitor *v,
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

static void cae_bpred_tage_sc_l_instance_init(Object *obj)
{
    CaeBPredTageScL *p = CAE_BPRED_TAGE_SC_L(obj);

    p->bimodal_entries = 4096u;
    p->num_tage_tables = 4u;
    p->tage_entries = 1024u;
    p->tage_tag_bits = 11u;
    p->tage_min_hist = 5u;
    p->tage_max_hist = 64u;
    p->sc_entries = 512u;
    p->loop_entries = 64u;
    p->btb_entries = 64u;
    p->btb_assoc = 2u;
    p->ras_depth = 16u;
    p->mispredict_penalty_cycles = 7u;

    object_property_add_uint32_ptr(obj, "bimodal-entries",
                                   &p->bimodal_entries,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "num-tage-tables",
                                   &p->num_tage_tables,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "tage-entries",
                                   &p->tage_entries,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "tage-tag-bits",
                                   &p->tage_tag_bits,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "tage-min-hist",
                                   &p->tage_min_hist,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "tage-max-hist",
                                   &p->tage_max_hist,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "sc-entries",
                                   &p->sc_entries,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "loop-entries",
                                   &p->loop_entries,
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

    object_property_add(obj, "predictions", "uint64",
                        cae_bpred_tage_sc_l_stat_get, NULL, NULL,
                        &p->predictions);
    object_property_add(obj, "mispredictions", "uint64",
                        cae_bpred_tage_sc_l_stat_get, NULL, NULL,
                        &p->mispredictions);
    object_property_add(obj, "sc-inversions", "uint64",
                        cae_bpred_tage_sc_l_stat_get, NULL, NULL,
                        &p->sc_inversions);
    object_property_add(obj, "loop-overrides", "uint64",
                        cae_bpred_tage_sc_l_stat_get, NULL, NULL,
                        &p->loop_overrides);
    object_property_add(obj, "allocations", "uint64",
                        cae_bpred_tage_sc_l_stat_get, NULL, NULL,
                        &p->allocations);
    object_property_add(obj, "alloc-stalls", "uint64",
                        cae_bpred_tage_sc_l_stat_get, NULL, NULL,
                        &p->alloc_stalls);
    /*
     * BS-33 round-14 fix: per-component tag-hit counters are
     * surfaced as read-only QOM uint64 properties
     * `tage-hits-0 … tage-hits-(CAE_TAGE_MAX_TABLES-1)`. Round 13
     * tracked `tage_hits[t]` internally but never registered the
     * properties, so QMP introspection could not reach them.
     * All CAE_TAGE_MAX_TABLES slots are registered up-front even
     * though the active `num_tage_tables` may be smaller — the
     * unused-component counters stay at zero and the fixed
     * property surface means a calibration harness can walk a
     * stable schema regardless of the table count chosen via
     * the `num-tage-tables` knob.
     */
    for (uint32_t t = 0; t < CAE_TAGE_MAX_TABLES; t++) {
        char *prop_name = g_strdup_printf("tage-hits-%u", t);
        object_property_add(obj, prop_name, "uint64",
                            cae_bpred_tage_sc_l_stat_get, NULL,
                            NULL, &p->tage_hits[t]);
        g_free(prop_name);
    }
}

/* ------------------------------------------------------------------ */
/*  Speculation save/restore: GHR / PHR / RAS history lanes           */
/* ------------------------------------------------------------------ */

/*
 * Plan AC-K-4 calls out predictor history (GHR/PHR) and RAS state
 * as part of the checkpoint lane (plan.md:89). TAGE-SC-L owns all
 * three through the embedded fields below. This snapshot captures
 * the minimum set needed to replay prediction correctness on a
 * mispredict restore: the two history registers and the RAS stack
 * (top + entry values). Tagged-table / bimodal / SC counter state
 * is learning state, not history, and is intentionally NOT
 * snapshotted — the round-29 scope is wrong-path history replay,
 * not full learning-state rollback.
 */
typedef struct TageSpecSnap {
    uint64_t ghr;
    uint64_t phr;
    uint32_t ras_depth;
    uint32_t ras_top;
    uint64_t *ras_stack_copy;    /* [ras_depth] */
} TageSpecSnap;

static CaeBPredSpecSnapshot *
cae_bpred_tage_sc_l_spec_snapshot(Object *obj)
{
    CaeBPredTageScL *p = CAE_BPRED_TAGE_SC_L(obj);
    TageSpecSnap *s = g_new0(TageSpecSnap, 1);

    s->ghr = p->ghr;
    s->phr = p->phr;
    s->ras_depth = p->ras.depth;
    s->ras_top = p->ras.top;
    if (p->ras.depth > 0 && p->ras.stack != NULL) {
        s->ras_stack_copy = g_new0(uint64_t, p->ras.depth);
        memcpy(s->ras_stack_copy, p->ras.stack,
               sizeof(uint64_t) * p->ras.depth);
    }
    return (CaeBPredSpecSnapshot *)s;
}

static void
cae_bpred_tage_sc_l_spec_restore(Object *obj,
                                 const CaeBPredSpecSnapshot *snap)
{
    CaeBPredTageScL *p = CAE_BPRED_TAGE_SC_L(obj);
    const TageSpecSnap *s = (const TageSpecSnap *)snap;

    /*
     * Refuse to restore across a RAS depth change — same
     * reasoning as the decoupled FTQ-size guard. Any mismatch
     * means the predictor was reconfigured between save and
     * restore and the byte-copy would corrupt out-of-range
     * indices.
     */
    if (s->ras_depth != p->ras.depth) {
        return;
    }
    p->ghr = s->ghr;
    p->phr = s->phr;
    p->ras.top = s->ras_top;
    if (s->ras_stack_copy != NULL && p->ras.stack != NULL) {
        memcpy(p->ras.stack, s->ras_stack_copy,
               sizeof(uint64_t) * p->ras.depth);
    }
}

static void
cae_bpred_tage_sc_l_spec_drop(CaeBPredSpecSnapshot *snap)
{
    if (!snap) {
        return;
    }
    TageSpecSnap *s = (TageSpecSnap *)snap;
    g_free(s->ras_stack_copy);
    g_free(s);
}

static void cae_bpred_tage_sc_l_class_init(ObjectClass *klass, const void *data)
{
    UserCreatableClass *uc = USER_CREATABLE_CLASS(klass);
    CaeBPredClass *bc = CAE_BPRED_CLASS(klass);

    (void)data;
    uc->complete = cae_bpred_tage_sc_l_complete;
    bc->predict = cae_bpred_tage_sc_l_predict;
    bc->update = cae_bpred_tage_sc_l_update;
    bc->reset = cae_bpred_tage_sc_l_reset;
    bc->spec_snapshot = cae_bpred_tage_sc_l_spec_snapshot;
    bc->spec_restore = cae_bpred_tage_sc_l_spec_restore;
    bc->spec_drop = cae_bpred_tage_sc_l_spec_drop;
}

static const TypeInfo cae_bpred_tage_sc_l_type = {
    .name = TYPE_CAE_BPRED_TAGE_SC_L,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeBPredTageScL),
    .instance_init = cae_bpred_tage_sc_l_instance_init,
    .instance_finalize = cae_bpred_tage_sc_l_finalize,
    .class_init = cae_bpred_tage_sc_l_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { TYPE_CAE_BPRED },
        { }
    }
};

static void cae_bpred_tage_sc_l_register_types(void)
{
    type_register_static(&cae_bpred_tage_sc_l_type);
}

type_init(cae_bpred_tage_sc_l_register_types)
