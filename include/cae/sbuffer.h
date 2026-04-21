/*
 * CAE M4' speculative store buffer — public API.
 *
 * See hw/cae/ooo/sbuffer.c for the full contract. Headers are split
 * so unit tests and future M4' checkpoint-restore code can reach the
 * alloc / commit / squash entry points without pulling in the
 * instance internals.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_SBUFFER_H
#define CAE_SBUFFER_H

#include <stdbool.h>
#include <stdint.h>
#include "qom/object.h"

typedef struct CaeSbuffer CaeSbuffer;

#define TYPE_CAE_SBUFFER "cae-sbuffer"

/*
 * Payload-preserving view of one sbuffer entry. Filled by the
 * BS-28 round-11 payload-drain API so callers (cpu_ooo.c, LSQ,
 * M4' tcg-spec-path) can observe the `{sqn, pc, addr, size,
 * value}` of each store crossing the commit boundary. Round-10's
 * count-only commit API could not support post-commit accounting
 * (per-address coherence, wrong-path leak checks, retire-time
 * memory hash updates); this view is the minimum surface needed
 * for those M4' callers.
 */
typedef struct CaeSbufferView {
    uint64_t sqn;
    uint64_t pc;
    uint64_t addr;
    uint16_t size;
    uint64_t value;
} CaeSbufferView;

/*
 * Non-destructive check: returns true when the sbuffer has room
 * for one more cae_sbuffer_alloc(). BS-30 round 12 introduced
 * this so the OoO dispatch path can pre-check all resources
 * before allocating any — previously the round-11 cpu_ooo would
 * allocate LSQ/sbuffer eagerly and leak when ROB dispatch
 * failed later in the same retire.
 */
bool cae_sbuffer_has_slot(const CaeSbuffer *sb);

/*
 * Enqueue one speculative store. Returns false when the buffer is
 * full (caller treats as backpressure). `sqn` is the pipeline's
 * monotonic retirement sequence number — required so squash_after
 * can target the right tail slice.
 */
bool cae_sbuffer_alloc(CaeSbuffer *sb, uint64_t sqn, uint64_t pc,
                       uint64_t addr, uint16_t size, uint64_t value);

/*
 * Read-only peek at the oldest live entry (commit-order head).
 * Returns true + fills `out` when there is an entry, false when
 * the buffer is empty. No state change. Useful for M4' tcg-spec-
 * path pre-commit inspection without paying the drain cost.
 */
bool cae_sbuffer_peek_head(const CaeSbuffer *sb, CaeSbufferView *out);

/*
 * Payload-preserving FIFO drain. Drains up to `max` oldest
 * entries whose `sqn <= commit_sqn` into `out[0..returned)` and
 * returns the count written. `out` may be NULL to preserve the
 * round-10 count-only behaviour (used by the old
 * cae_sbuffer_commit wrapper for backward-compat callers that do
 * not care about the payload). Stops at the first entry whose
 * sqn exceeds commit_sqn because out-of-order commit would
 * reorder externally-visible writes.
 */
uint32_t cae_sbuffer_drain_head(CaeSbuffer *sb, uint64_t commit_sqn,
                                CaeSbufferView *out, uint32_t max);

/*
 * Commit FIFO-ordered entries whose sqn is <= commit_sqn. Returns
 * the count drained. Kept as a backward-compat wrapper around
 * cae_sbuffer_drain_head(..., NULL, UINT32_MAX); new callers
 * should use the payload-aware entry point above.
 */
uint32_t cae_sbuffer_commit(CaeSbuffer *sb, uint64_t commit_sqn);

/*
 * LIFO-discard entries with sqn >= squash_sqn. Returns the count
 * removed. Called from the M4' squash handler; strict-younger
 * contract ensures committed-before-squash entries are preserved.
 */
uint32_t cae_sbuffer_squash_after(CaeSbuffer *sb, uint64_t squash_sqn);

/*
 * Round 52 AC-K-5: read the current live occupancy. Used by
 * `cae_cpu_ooo_charge` to drive the sbuffer-evict live cycle
 * charge when occupancy crosses the configured watermark.
 * Returns 0 on a NULL sbuffer.
 */
uint32_t cae_sbuffer_occupancy(const CaeSbuffer *sb);

/*
 * Eviction cause mirror of gem5 O3's StoreBufferEvictCause.
 * Exactly one cause fires per tick — the three causes are
 * mutually exclusive by construction (priority order enforced
 * inside cae_sbuffer_tick). NONE means the tick did not evict.
 */
typedef enum {
    CAE_SBUFFER_EVICT_NONE = 0,
    CAE_SBUFFER_EVICT_SQFULL,
    CAE_SBUFFER_EVICT_FULL,
    CAE_SBUFFER_EVICT_TIMEOUT,
} CaeSbufferEvictCause;

/*
 * Result of one cae_sbuffer_tick call. Two independent outcomes
 * per tick:
 *
 *   - Limited commit-drain: when the head entry's sqn is at or
 *     below `commit_sqn`, the head is drained into
 *     `drained_view`, `drained` is set to true, and downstream
 *     observers can consume the payload (sbuffer-commits /
 *     last-committed-store-*). At most one drain per tick.
 *
 *   - Eviction cause: when buffer pressure or idle timeout
 *     demands a non-commit release, one of SQFull / Full /
 *     Timeout fires and `evicted_view` holds the discarded
 *     entry's payload (so callers that wish to record
 *     speculation-leak or capacity-drop telemetry can).
 *
 * Callers are NOT required to pre-zero the struct; the tick
 * implementation zero-initializes every field before deciding
 * any outcome.
 */
typedef struct CaeSbufferTickResult {
    bool                  drained;
    CaeSbufferView        drained_view;
    CaeSbufferEvictCause  cause;
    CaeSbufferView        evicted_view;
} CaeSbufferTickResult;

/*
 * Periodic tick for the idle-timeout + pressure-eviction pump.
 * NULL-safe; all fields of `out` are zeroed before any further
 * decision, so `out->drained == false` and
 * `out->cause == CAE_SBUFFER_EVICT_NONE` are the safe
 * no-op reading.
 *
 * Decision sequence:
 *   1. Limited commit-drain step: if the head entry's sqn is
 *      ≤ `commit_sqn`, drain the head into `out->drained_view`.
 *      Any drain resets `inactive_cycles` (activity signal).
 *   2. Eviction cause step (operates on the NEW head after step 1):
 *      - SQFull: `sqfull_commit_lag_threshold > 0` AND
 *                `store_sqn_next > sbuffer_commit_sqn` AND
 *                `store_sqn_next - sbuffer_commit_sqn ≥ lag_thresh`
 *                AND head is not commit-drainable.
 *        The `store_sqn_next > sbuffer_commit_sqn` pre-check is
 *        the underflow guard: subtraction never executes on a
 *        saturated or stale watermark.
 *      - Full:   `evict_threshold > 0` AND `occupancy ≥ threshold`
 *                AND head is not commit-drainable.
 *      - Timeout: `inactive_threshold > 0` AND non-empty buffer
 *                AND `inactive_cycles` exceeds the threshold.
 *                (Non-empty is guaranteed if step 1 did not
 *                drain the last entry; Timeout does not require
 *                `head.sqn > commit_sqn` since the point is
 *                idleness, not commit-lag.)
 *     Exactly one of the three causes fires per tick (priority
 *     SQFull > Full > Timeout). The chosen branch resets
 *     `inactive_cycles`.
 *
 * If step 2 did not evict and step 1 did not drain, and the
 * buffer is non-empty, `inactive_cycles` is incremented by one
 * (it has NOT been reset by activity).
 */
void cae_sbuffer_tick(CaeSbuffer *sb, uint64_t commit_sqn,
                      uint64_t store_sqn_next,
                      CaeSbufferTickResult *out);

/* ------------------------------------------------------------------ */
/*  Speculation save/restore                                          */
/* ------------------------------------------------------------------ */

/*
 * Test-only seed seam for the tick-driver's `inactive_cycles`
 * internal progress pointer. Lets unit tests inject a non-zero
 * save-time value so save/restore coverage can observe the
 * roundtrip. NULL obj or non-cae-sbuffer object is a safe no-op.
 * Production callers advance this counter through the sbuffer
 * tick entry point; they must not use this seam.
 */
void cae_sbuffer_test_seed_inactive_cycles(Object *obj, uint64_t value);

/*
 * Test-only observer for `inactive_cycles`. Returns 0 on NULL or
 * non-cae-sbuffer object. Exists so the tick-driver checkpoint
 * regression can assert the field's save/restore behaviour
 * without reaching into the private CaeSbuffer layout.
 */
uint64_t cae_sbuffer_inactive_cycles(const Object *obj);

/*
 * Test-only seed seam for the three lifetime-only eviction
 * counters (`timeout_evicts`, `full_evicts`, `sqfull_evicts`).
 * Lets a unit regression drive the lifetime invariant
 * explicitly: seed pre-save values, save a snapshot, mutate the
 * counters to different values, restore, and assert the MUTATED
 * (post-save) values survive instead of being rewound. NULL or
 * wrong-type object is a safe no-op. Production callers advance
 * these counters through the tick pump; they must not call this
 * seam.
 */
void cae_sbuffer_test_seed_lifetime_counters(Object *obj,
                                             uint64_t timeout_evicts,
                                             uint64_t full_evicts,
                                             uint64_t sqfull_evicts);

/*
 * Opaque speculation-snapshot of the sbuffer entry ring +
 * head/tail/occupancy + commit-/squash-/stall counters +
 * inactive-cycles progress pointer. Struct definition lives in
 * hw/cae/ooo/sbuffer.c so the internal layout stays encapsulated
 * (same pattern as CaeBPredSpecSnapshot / CaeMshrSpecSnapshot).
 *
 * save: obj may be NULL or a non-cae-sbuffer Object *; both return
 * NULL so the CaeCpuOoo composer can compose unconditionally.
 * restore: NULL obj or NULL snap is a safe no-op. Within-run
 * contract: sbuffer_size is set once at complete() time and never
 * changes, so restore does not write it back; entries[] copy is
 * sized at CAE_SBUFFER_MAX_ENTRIES (the compile-time ring cap).
 * drop: NULL-safe.
 */
typedef struct CaeSbufferSpecSnapshot CaeSbufferSpecSnapshot;

CaeSbufferSpecSnapshot *cae_sbuffer_spec_snapshot_save(Object *obj);
void cae_sbuffer_spec_snapshot_restore(Object *obj,
                                       const CaeSbufferSpecSnapshot *snap);
void cae_sbuffer_spec_snapshot_drop(CaeSbufferSpecSnapshot *snap);

#endif /* CAE_SBUFFER_H */
