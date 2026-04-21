/*
 * CAE M4' speculative store buffer.
 *
 * Holds retired-but-uncommitted stores between the store's retirement
 * from the pipeline (where it leaves the LSQ — see hw/cae/ooo/lsq.c)
 * and its commit to the coherent memory model. Two operations beyond
 * standard commit distinguish this from the LSQ:
 *
 *   - sbuffer_squash_after(sqn): discard every entry whose sequence
 *     number is >= sqn. Called by the M4' TCG predicted-path
 *     squash handler when a mispredict or fault rolls back
 *     speculative state.
 *
 *   - sbuffer_alloc() returns false when full. The caller treats the
 *     shortage as frontend backpressure: retirement stalls until a
 *     drain (commit or squash) frees an entry. M4' calibration
 *     against kmhv3.py picks sbuffer_size from the paired YAML
 *     (`xs_gem5_only.memory.sbuffer.size`); default 16 matches the
 *     XS-GEM5 `L1_SBUFFER_SIZE` knob.
 *
 * Commit order is strict FIFO (the oldest entry commits first); squash
 * is the only path that can remove an entry out of order. Per-address
 * ordering violation detection (RARQ / RAWQ) lives separately in
 * hw/cae/ooo/violation.c (M5') because violations are detected at
 * LSQ allocation, not at sbuffer drain.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "cae/sbuffer.h"

#define CAE_SBUFFER_DEFAULT_SIZE    16u
#define CAE_SBUFFER_DEFAULT_BANKS    2u
#define CAE_SBUFFER_MAX_ENTRIES     64u

typedef struct CaeSbufferEntry {
    uint64_t sqn;
    uint64_t pc;
    uint64_t addr;
    uint64_t value;
    uint16_t size;
    bool     valid;
} CaeSbufferEntry;

typedef struct CaeSbuffer CaeSbuffer;

OBJECT_DECLARE_SIMPLE_TYPE(CaeSbuffer, CAE_SBUFFER)

struct CaeSbuffer {
    Object parent;

    /* User-tunable. */
    uint32_t sbuffer_size;
    uint32_t bank_count;
    /*
     * Round 49 AC-K-5: sbuffer evict-threshold. When occupancy
     * equals or exceeds this watermark, the sbuffer is considered
     * "pressure-high": each subsequent alloc bumps the
     * `evict_threshold_events` counter so callers / QMP observers
     * can see when back-pressure actually crosses into the
     * accelerated-drain regime. Zero disables the tracker (matches
     * pre-round-49 behaviour).
     */
    uint32_t evict_threshold;

    /*
     * Idle-timeout watermark for the periodic tick driver. Legal
     * values are 0 (disabled) or >= 2. Both the QOM setter and
     * user_creatable_complete clamp a requested value of 1 to 0
     * with a warn report: a threshold of 1 would degrade into
     * "evict head on every idle cycle", starving the Full /
     * SQFull causes by never letting occupancy climb long enough
     * for them to fire. Zero leaves the tracker disabled for
     * callers that have not opted in.
     */
    uint32_t inactive_threshold;

    /*
     * Commit-lag watermark for the SQFull cause. When
     * `store_sqn_next - sbuffer_commit_sqn` crosses this value
     * and the head entry is not commit-drainable, the tick
     * evicts the head as SQFull. Zero disables SQFull (legal
     * value); a non-zero value has no clamped lower bound —
     * values below the buffer capacity merely make SQFull fire
     * before Full, which is the intended priority. Exposed as
     * an RW QOM knob; because this is classified as
     * "configuration" (not snapshot), runtime writes are
     * intentionally NOT rewound by speculative restore.
     */
    uint32_t sqfull_commit_lag_threshold;

    /*
     * Entry pool. FIFO commit order means we track head / tail as
     * indices into a fixed-size ring. Static capping at
     * CAE_SBUFFER_MAX_ENTRIES keeps the struct a fixed size —
     * sbuffer_size clamps the effective occupancy below that cap.
     */
    CaeSbufferEntry entries[CAE_SBUFFER_MAX_ENTRIES];
    uint32_t head;        /* next slot to commit */
    uint32_t tail;        /* next free slot */
    uint32_t occupancy;   /* entries[head..tail) live count */

    /* Read-only stats. */
    uint64_t enqueued;
    uint64_t dequeued;
    uint64_t squashed;
    uint64_t alloc_stalls;
    uint64_t evict_threshold_events;
    /*
     * Per-cause eviction counters. All three are lifetime-only:
     * they survive speculative restore, matching the precedent
     * set by evict_threshold_events. They are mutually exclusive
     * within one tick (priority SQFull > Full > Timeout).
     */
    uint64_t timeout_evicts;
    uint64_t full_evicts;
    uint64_t sqfull_evicts;

    /*
     * Diagnostic telemetry for the tick pump. All lifetime-only
     * (never in spec snapshot). Exposed as RO QOM properties so
     * a live xs-suite CAE run can explain the shape of its
     * eviction signal quantitatively — which is exactly what
     * the Round-5 review required: measured evidence, not
     * speculation about "head always drainable" or "occupancy
     * never climbs". Semantics:
     *   tick_calls: every non-NULL invocation of cae_sbuffer_tick
     *               (the NULL-safe early-return does not count).
     *   tick_head_drainable_events: on entry occupancy > 0 AND
     *               head.sqn <= commit_sqn (head is immediately
     *               drainable by the limited-drain step).
     *   tick_head_non_drainable_events: on entry occupancy > 0
     *               AND head.sqn > commit_sqn (head is NOT
     *               commit-drainable — the condition that
     *               enables Full / SQFull eviction branches).
     *   tick_inactive_max: historical maximum observed value of
     *               inactive_cycles (captures how close the
     *               Timeout threshold was approached).
     *   tick_occupancy_max: historical maximum observed value of
     *               occupancy on entry to tick (captures the
     *               natural residency peak, useful against
     *               evict_threshold).
     */
    uint64_t tick_calls;
    uint64_t tick_head_drainable_events;
    uint64_t tick_head_non_drainable_events;
    uint64_t tick_inactive_max;
    uint64_t tick_occupancy_max;

    /*
     * Internal progress pointer for the periodic tick driver. The
     * tick pump (cae_sbuffer_tick, wired up in a later change)
     * increments this on each call when the buffer is non-empty
     * and clears it on alloc / drain, so "cycles since last
     * sbuffer activity" is observable without threading a
     * separate clock into the sbuffer. Participates in
     * speculative save/restore so mispredict rollback does not
     * falsely advance the timeout countdown.
     */
    uint64_t inactive_cycles;

    bool configured;
};

/*
 * Non-destructive check matching the round-11 alloc failure
 * condition. Lets callers pre-gate a store's dispatch so the
 * sbuffer never mutates state when ROB / LSQ / RAT are about to
 * refuse the same dispatch (BS-30 round 12).
 */
bool cae_sbuffer_has_slot(const CaeSbuffer *sb)
{
    return sb->occupancy < sb->sbuffer_size;
}

/*
 * Allocate a new entry for a retiring speculative store. Returns
 * false when the buffer is full; caller stalls retirement until a
 * drain frees an entry.
 */
bool cae_sbuffer_alloc(CaeSbuffer *sb, uint64_t sqn, uint64_t pc,
                       uint64_t addr, uint16_t size, uint64_t value)
{
    if (sb->occupancy >= sb->sbuffer_size) {
        sb->alloc_stalls++;
        return false;
    }
    CaeSbufferEntry *e = &sb->entries[sb->tail];
    e->sqn = sqn;
    e->pc = pc;
    e->addr = addr;
    e->value = value;
    e->size = size;
    e->valid = true;
    sb->tail = (sb->tail + 1u) % CAE_SBUFFER_MAX_ENTRIES;
    sb->occupancy++;
    sb->enqueued++;
    /* Alloc is an activity signal: reset the idle countdown. */
    sb->inactive_cycles = 0u;
    /*
     * Round 49 AC-K-5: evict-threshold event. When the sbuffer's
     * occupancy crosses the configured watermark on this alloc,
     * bump the event counter. `evict_threshold == 0` leaves the
     * tracker disabled (pre-round-49 behaviour). `>=` so crossing
     * into or remaining at the threshold both count — this mirrors
     * the kmhv3.py hardware's continuous-pressure semantic where
     * every alloc above the watermark increases drain urgency.
     */
    if (sb->evict_threshold > 0u &&
        sb->occupancy >= sb->evict_threshold) {
        sb->evict_threshold_events++;
    }
    return true;
}

/*
 * Round 52 AC-K-5: live-occupancy accessor for the cpu-model
 * retire path. Drives the sbuffer-evict-threshold live cycle
 * charge without routing callers through the QOM property
 * layer on every retire. NULL-safe.
 */
uint32_t cae_sbuffer_occupancy(const CaeSbuffer *sb)
{
    return sb ? sb->occupancy : 0u;
}

/*
 * Read-only view of the oldest live entry. Returns true + fills
 * `out` when the buffer has at least one live entry; false when
 * empty. No state change — callers can chain peek + drain to
 * implement bounded commit without losing the payload.
 */
bool cae_sbuffer_peek_head(const CaeSbuffer *sb, CaeSbufferView *out)
{
    if (sb->occupancy == 0u) {
        return false;
    }
    const CaeSbufferEntry *e = &sb->entries[sb->head];
    if (!e->valid) {
        return false;
    }
    if (out) {
        out->sqn   = e->sqn;
        out->pc    = e->pc;
        out->addr  = e->addr;
        out->size  = e->size;
        out->value = e->value;
    }
    return true;
}

/*
 * Payload-preserving FIFO drain. BS-28 round-11. Drains up to
 * `max` oldest entries whose sqn <= commit_sqn, writes their
 * payload into out[0..returned), and returns the count. `out`
 * may be NULL when the caller only needs the count (the
 * cae_sbuffer_commit wrapper uses this). Stops at the first
 * too-young entry because out-of-order commit would reorder
 * externally-visible writes.
 */
uint32_t cae_sbuffer_drain_head(CaeSbuffer *sb, uint64_t commit_sqn,
                                CaeSbufferView *out, uint32_t max)
{
    uint32_t drained = 0;
    while (drained < max && sb->occupancy > 0) {
        CaeSbufferEntry *e = &sb->entries[sb->head];
        if (!e->valid || e->sqn > commit_sqn) {
            break;
        }
        if (out) {
            out[drained].sqn   = e->sqn;
            out[drained].pc    = e->pc;
            out[drained].addr  = e->addr;
            out[drained].size  = e->size;
            out[drained].value = e->value;
        }
        e->valid = false;
        sb->head = (sb->head + 1u) % CAE_SBUFFER_MAX_ENTRIES;
        sb->occupancy--;
        sb->dequeued++;
        drained++;
    }
    if (drained > 0u) {
        /* Drain is an activity signal: reset the idle countdown. */
        sb->inactive_cycles = 0u;
    }
    return drained;
}

/*
 * Pop the head entry out as a non-commit eviction, copying its
 * payload into `out` when `out != NULL`. Returns true when an
 * entry was actually removed (occupancy drops by one). Private
 * helper shared by the SQFull / Full / Timeout branches of
 * cae_sbuffer_tick so the three eviction paths stay mechanically
 * identical apart from which counter they bump.
 */
static bool sbuffer_evict_head(CaeSbuffer *sb, CaeSbufferView *out)
{
    if (sb->occupancy == 0u) {
        return false;
    }
    CaeSbufferEntry *e = &sb->entries[sb->head];
    if (!e->valid) {
        return false;
    }
    if (out) {
        out->sqn   = e->sqn;
        out->pc    = e->pc;
        out->addr  = e->addr;
        out->size  = e->size;
        out->value = e->value;
    }
    e->valid = false;
    sb->head = (sb->head + 1u) % CAE_SBUFFER_MAX_ENTRIES;
    sb->occupancy--;
    sb->dequeued++;
    return true;
}

void cae_sbuffer_tick(CaeSbuffer *sb, uint64_t commit_sqn,
                      uint64_t store_sqn_next,
                      CaeSbufferTickResult *out)
{
    if (out) {
        out->drained = false;
        out->cause = CAE_SBUFFER_EVICT_NONE;
        memset(&out->drained_view, 0, sizeof(out->drained_view));
        memset(&out->evicted_view, 0, sizeof(out->evicted_view));
    }
    if (sb == NULL) {
        return;
    }

    /*
     * Diagnostic telemetry (lifetime-only): capture the tick's
     * input state BEFORE the drain/eviction branches mutate
     * occupancy. Helps a live xs-suite run explain the shape
     * of its eviction signal quantitatively — `tick_calls` is
     * the denominator; `*_drainable_events` partition non-empty
     * entries between "head immediately drainable" (Full /
     * SQFull branches CANNOT fire) and "head stuck by commit
     * lag" (they CAN fire). `tick_occupancy_max` and
     * `tick_inactive_max` respectively track the natural
     * residency peak and how close the Timeout idle-cycle
     * threshold was approached.
     */
    sb->tick_calls++;
    if (sb->occupancy > 0u) {
        CaeSbufferEntry *probe = &sb->entries[sb->head];
        if (probe->valid) {
            if (probe->sqn <= commit_sqn) {
                sb->tick_head_drainable_events++;
            } else {
                sb->tick_head_non_drainable_events++;
            }
        }
    }
    if (sb->occupancy > sb->tick_occupancy_max) {
        sb->tick_occupancy_max = sb->occupancy;
    }
    if (sb->inactive_cycles > sb->tick_inactive_max) {
        sb->tick_inactive_max = sb->inactive_cycles;
    }

    /*
     * Step 1: limited commit-drain. At most one head-of-FIFO
     * entry whose sqn is at or below commit_sqn drains per tick;
     * its payload is reported to the caller so downstream
     * telemetry (sbuffer-commits, last-committed-store-*) can
     * reflect the real drain point. The local flag decouples
     * "did a drain happen this tick" from the optional out
     * parameter — so the timeout branch below suppresses the
     * idle increment correctly even when callers pass a NULL
     * out, as the header contract promises.
     */
    bool drained_this_tick = false;
    if (sb->occupancy > 0u) {
        CaeSbufferEntry *head = &sb->entries[sb->head];
        if (head->valid && head->sqn <= commit_sqn) {
            CaeSbufferView v = { 0 };
            v.sqn   = head->sqn;
            v.pc    = head->pc;
            v.addr  = head->addr;
            v.size  = head->size;
            v.value = head->value;
            head->valid = false;
            sb->head = (sb->head + 1u) % CAE_SBUFFER_MAX_ENTRIES;
            sb->occupancy--;
            sb->dequeued++;
            sb->inactive_cycles = 0u;
            drained_this_tick = true;
            if (out) {
                out->drained = true;
                out->drained_view = v;
            }
        }
    }

    /*
     * Step 2: eviction cause — SQFull first, then Full, then
     * Timeout. Exactly one cause fires per tick. Each branch
     * requires the NEW head (post-drain) to meet its trigger;
     * SQFull and Full additionally require the head to be
     * non-commit-drainable (sqn > commit_sqn), otherwise the
     * commit-drain step would have handled it instead.
     */
    if (sb->occupancy == 0u) {
        /* Empty after drain: no idle increment, no eviction. */
        return;
    }
    CaeSbufferEntry *head = &sb->entries[sb->head];
    bool head_drainable = head->valid && head->sqn <= commit_sqn;

    /* SQFull: commit is too far behind. Underflow guard on the
     * subtraction — store_sqn_next must strictly exceed the
     * committed watermark. */
    if (sb->sqfull_commit_lag_threshold > 0u && !head_drainable &&
        store_sqn_next > commit_sqn) {
        uint64_t lag = store_sqn_next - commit_sqn;
        if (lag >= sb->sqfull_commit_lag_threshold) {
            CaeSbufferView v = { 0 };
            if (sbuffer_evict_head(sb, &v)) {
                sb->sqfull_evicts++;
                sb->inactive_cycles = 0u;
                if (out) {
                    out->cause = CAE_SBUFFER_EVICT_SQFULL;
                    out->evicted_view = v;
                }
                return;
            }
        }
    }

    /* Full: occupancy hit the configured watermark AND the head
     * is not commit-drainable. */
    if (sb->evict_threshold > 0u && !head_drainable &&
        sb->occupancy >= sb->evict_threshold) {
        CaeSbufferView v = { 0 };
        if (sbuffer_evict_head(sb, &v)) {
            sb->full_evicts++;
            sb->inactive_cycles = 0u;
            if (out) {
                out->cause = CAE_SBUFFER_EVICT_FULL;
                out->evicted_view = v;
            }
            return;
        }
    }

    /* Timeout: idle-count pump. Increments inactive_cycles ONLY
     * when no drain fired this tick (drain resets it above). If
     * the increment pushes the count past the threshold, evict
     * the head as Timeout. Unlike SQFull/Full, Timeout does not
     * require the head to be non-commit-drainable — the point of
     * Timeout is to flush idleness, not to respond to lag. */
    if (sb->inactive_threshold == 0u) {
        return;
    }
    if (drained_this_tick) {
        /* Step 1 already reset inactive_cycles via the drain; do
         * not increment. Also: when a drain fires, this tick is
         * not "idle" so Timeout must not fire. The check uses a
         * local flag, not out->drained, so this suppression is
         * correct even when `out` is NULL. */
        return;
    }
    sb->inactive_cycles++;
    if (sb->inactive_cycles <= sb->inactive_threshold) {
        return;
    }
    CaeSbufferView v = { 0 };
    if (sbuffer_evict_head(sb, &v)) {
        sb->timeout_evicts++;
        sb->inactive_cycles = 0u;
        if (out) {
            out->cause = CAE_SBUFFER_EVICT_TIMEOUT;
            out->evicted_view = v;
        }
    } else {
        /* Out-of-sync head (occupancy>0 but !valid): reset the
         * counter without charging an eviction that did not
         * really happen. */
        sb->inactive_cycles = 0u;
    }
}

/*
 * Backward-compat count-only commit. Kept so callers that
 * predate the payload-drain API keep working; new callers should
 * use cae_sbuffer_drain_head(..., out, max).
 */
uint32_t cae_sbuffer_commit(CaeSbuffer *sb, uint64_t commit_sqn)
{
    return cae_sbuffer_drain_head(sb, commit_sqn, NULL, UINT32_MAX);
}

/*
 * Discard every entry with sqn >= squash_sqn. Called from the M4'
 * TCG predicted-path squash handler; entries committed before
 * squash_sqn stay, the rest are dropped in LIFO order.
 */
uint32_t cae_sbuffer_squash_after(CaeSbuffer *sb, uint64_t squash_sqn)
{
    uint32_t squashed = 0;
    while (sb->occupancy > 0) {
        uint32_t prev = (sb->tail + CAE_SBUFFER_MAX_ENTRIES - 1u)
                        % CAE_SBUFFER_MAX_ENTRIES;
        CaeSbufferEntry *e = &sb->entries[prev];
        if (!e->valid || e->sqn < squash_sqn) {
            break;
        }
        e->valid = false;
        sb->tail = prev;
        sb->occupancy--;
        sb->squashed++;
        squashed++;
    }
    return squashed;
}

/* ------------------------------------------------------------------ */
/*  Speculation save/restore                                          */
/* ------------------------------------------------------------------ */

struct CaeSbufferSpecSnapshot {
    CaeSbufferEntry entries[CAE_SBUFFER_MAX_ENTRIES];
    uint32_t head;
    uint32_t tail;
    uint32_t occupancy;
    uint64_t enqueued;
    uint64_t dequeued;
    uint64_t squashed;
    uint64_t alloc_stalls;
    /*
     * Tick-driver internal state. Must return to its save-time
     * value on spec-restore so a mispredicted speculative window
     * does not leak phantom idle progress into the committed
     * timeline. The paired lifetime counters
     * (evict_threshold_events, plus later-landing eviction-reason
     * counters) stay outside this struct by design — they are
     * telemetry and are never rewound.
     */
    uint64_t inactive_cycles;
};

CaeSbufferSpecSnapshot *cae_sbuffer_spec_snapshot_save(Object *obj)
{
    if (obj == NULL) {
        return NULL;
    }
    CaeSbuffer *sb = (CaeSbuffer *)object_dynamic_cast(obj, TYPE_CAE_SBUFFER);
    if (sb == NULL) {
        return NULL;
    }
    CaeSbufferSpecSnapshot *snap = g_new0(CaeSbufferSpecSnapshot, 1);
    memcpy(snap->entries, sb->entries, sizeof(snap->entries));
    snap->head            = sb->head;
    snap->tail            = sb->tail;
    snap->occupancy       = sb->occupancy;
    snap->enqueued        = sb->enqueued;
    snap->dequeued        = sb->dequeued;
    snap->squashed        = sb->squashed;
    snap->alloc_stalls    = sb->alloc_stalls;
    snap->inactive_cycles = sb->inactive_cycles;
    return snap;
}

void cae_sbuffer_spec_snapshot_restore(Object *obj,
                                       const CaeSbufferSpecSnapshot *snap)
{
    if (obj == NULL || snap == NULL) {
        return;
    }
    CaeSbuffer *sb = (CaeSbuffer *)object_dynamic_cast(obj, TYPE_CAE_SBUFFER);
    if (sb == NULL) {
        return;
    }
    memcpy(sb->entries, snap->entries, sizeof(sb->entries));
    sb->head            = snap->head;
    sb->tail            = snap->tail;
    sb->occupancy       = snap->occupancy;
    sb->enqueued        = snap->enqueued;
    sb->dequeued        = snap->dequeued;
    sb->squashed        = snap->squashed;
    sb->alloc_stalls    = snap->alloc_stalls;
    sb->inactive_cycles = snap->inactive_cycles;
}

void cae_sbuffer_spec_snapshot_drop(CaeSbufferSpecSnapshot *snap)
{
    g_free(snap);
}

void cae_sbuffer_test_seed_inactive_cycles(Object *obj, uint64_t value)
{
    if (obj == NULL) {
        return;
    }
    CaeSbuffer *sb = (CaeSbuffer *)object_dynamic_cast(obj, TYPE_CAE_SBUFFER);
    if (sb == NULL) {
        return;
    }
    sb->inactive_cycles = value;
}

uint64_t cae_sbuffer_inactive_cycles(const Object *obj)
{
    if (obj == NULL) {
        return 0;
    }
    CaeSbuffer *sb = (CaeSbuffer *)object_dynamic_cast((Object *)obj,
                                                       TYPE_CAE_SBUFFER);
    if (sb == NULL) {
        return 0;
    }
    return sb->inactive_cycles;
}

void cae_sbuffer_test_seed_lifetime_counters(Object *obj,
                                             uint64_t timeout_evicts,
                                             uint64_t full_evicts,
                                             uint64_t sqfull_evicts)
{
    if (obj == NULL) {
        return;
    }
    CaeSbuffer *sb = (CaeSbuffer *)object_dynamic_cast(obj, TYPE_CAE_SBUFFER);
    if (sb == NULL) {
        return;
    }
    sb->timeout_evicts = timeout_evicts;
    sb->full_evicts    = full_evicts;
    sb->sqfull_evicts  = sqfull_evicts;
}

static void cae_sbuffer_complete(UserCreatable *uc, Error **errp)
{
    CaeSbuffer *sb = CAE_SBUFFER(uc);
    if (sb->sbuffer_size == 0u) {
        sb->sbuffer_size = CAE_SBUFFER_DEFAULT_SIZE;
    }
    if (sb->sbuffer_size > CAE_SBUFFER_MAX_ENTRIES) {
        error_setg(errp, "cae-sbuffer: sbuffer-size=%u exceeds "
                         "compile-time cap %u",
                   sb->sbuffer_size, CAE_SBUFFER_MAX_ENTRIES);
        return;
    }
    if (sb->bank_count == 0u) {
        sb->bank_count = CAE_SBUFFER_DEFAULT_BANKS;
    }
    /*
     * Defensive re-clamp: complete() runs after every property
     * write in the -object construction path, so if the QOM
     * setter did not already clamp a transient "1" value, do it
     * here too. See the setter comment below for the full
     * rationale and the runtime-RW invariant.
     */
    if (sb->inactive_threshold == 1u) {
        warn_report("cae-sbuffer: inactive-threshold=1 is not "
                    "supported (would starve Full / SQFull "
                    "causes); clamping to 0 (disabled)");
        sb->inactive_threshold = 0u;
    }
    sb->head = 0;
    sb->tail = 0;
    sb->occupancy = 0;
    sb->inactive_cycles = 0;
    sb->configured = true;
}

/*
 * Custom setter for inactive-threshold. Clamps a requested value
 * of 1 down to 0 with a warn report — a one-cycle timeout would
 * degrade into "evict head on every idle cycle", starving the
 * Full / SQFull causes by never letting occupancy climb long
 * enough for them to fire. Legal values are 0 (disabled) or
 * >= 2. This setter runs on both construction-time property
 * writes and runtime QMP writes, closing the gap that a pure
 * pointer-binding plus complete()-time clamp would leave open.
 */
static void cae_sbuffer_set_inactive_threshold(Object *obj, Visitor *v,
                                               const char *name,
                                               void *opaque,
                                               Error **errp)
{
    CaeSbuffer *sb = CAE_SBUFFER(obj);
    uint32_t value;

    (void)opaque;
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value == 1u) {
        warn_report("cae-sbuffer: inactive-threshold=1 is not "
                    "supported (would starve Full / SQFull "
                    "causes); clamping to 0 (disabled)");
        value = 0u;
    }
    sb->inactive_threshold = value;
}

static void cae_sbuffer_get_inactive_threshold(Object *obj, Visitor *v,
                                               const char *name,
                                               void *opaque,
                                               Error **errp)
{
    CaeSbuffer *sb = CAE_SBUFFER(obj);

    (void)opaque;
    visit_type_uint32(v, name, &sb->inactive_threshold, errp);
}

static void cae_sbuffer_instance_init(Object *obj)
{
    CaeSbuffer *sb = CAE_SBUFFER(obj);

    sb->sbuffer_size = CAE_SBUFFER_DEFAULT_SIZE;
    sb->bank_count   = CAE_SBUFFER_DEFAULT_BANKS;
    sb->evict_threshold = 0u;  /* Round 49: default disabled. */
    sb->inactive_threshold = 0u;
    sb->sqfull_commit_lag_threshold = 0u;

    object_property_add_uint32_ptr(obj, "sbuffer-size", &sb->sbuffer_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "bank-count", &sb->bank_count,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "evict-threshold",
                                   &sb->evict_threshold,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "inactive-threshold", "uint32",
                        cae_sbuffer_get_inactive_threshold,
                        cae_sbuffer_set_inactive_threshold,
                        NULL, NULL);
    object_property_add_uint32_ptr(obj, "sqfull-commit-lag-threshold",
                                   &sb->sqfull_commit_lag_threshold,
                                   OBJ_PROP_FLAG_READWRITE);
    /* Read-only stats surface. */
    object_property_add_uint32_ptr(obj, "occupancy", &sb->occupancy,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "enqueued", &sb->enqueued,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "dequeued", &sb->dequeued,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "squashed", &sb->squashed,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "alloc-stalls", &sb->alloc_stalls,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "evict-threshold-events",
                                   &sb->evict_threshold_events,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "timeout-evicts",
                                   &sb->timeout_evicts,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "full-evicts",
                                   &sb->full_evicts,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "sqfull-evicts",
                                   &sb->sqfull_evicts,
                                   OBJ_PROP_FLAG_READ);
    /*
     * Diagnostic tick-path telemetry. All five are lifetime-
     * only RO QOM properties. They explain the shape of the
     * eviction signal quantitatively so a live xs-suite run
     * can say concretely whether the head is always
     * commit-drainable, whether occupancy never climbs, or
     * whether inactive_cycles never crosses threshold — rather
     * than relying on post-hoc speculation.
     */
    object_property_add_uint64_ptr(obj, "tick-calls",
                                   &sb->tick_calls,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "tick-head-drainable-events",
                                   &sb->tick_head_drainable_events,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "tick-head-non-drainable-events",
                                   &sb->tick_head_non_drainable_events,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "tick-inactive-max",
                                   &sb->tick_inactive_max,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "tick-occupancy-max",
                                   &sb->tick_occupancy_max,
                                   OBJ_PROP_FLAG_READ);
}

static void cae_sbuffer_class_init(ObjectClass *klass, const void *data)
{
    UserCreatableClass *uc = USER_CREATABLE_CLASS(klass);

    (void)data;
    uc->complete = cae_sbuffer_complete;
}

static const TypeInfo cae_sbuffer_type = {
    .name = TYPE_CAE_SBUFFER,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeSbuffer),
    .instance_init = cae_sbuffer_instance_init,
    .class_init = cae_sbuffer_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { }
    }
};

static void cae_sbuffer_register_types(void)
{
    type_register_static(&cae_sbuffer_type);
}

type_init(cae_sbuffer_register_types)
