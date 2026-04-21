/*
 * CAE Out-of-Order timing-model internal API.
 *
 * Arch-neutral types for the M3' OoO functional oracle. The top-level
 * CaeCpuOoo in hw/cae/cpu_ooo.c composes these sub-structures to
 * charge per-retired-uop cycles against a CaeCpu under
 * `cpu-model=ooo-kmhv3`.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_OOO_H
#define CAE_OOO_H

#include <stdint.h>
#include <stdbool.h>
#include "cae/uop.h"

/* Default OoO knobs matching the kmhv3.py baseline. */
#define CAE_OOO_DEFAULT_ROB_SIZE            352u
#define CAE_OOO_DEFAULT_LQ_SIZE             72u
#define CAE_OOO_DEFAULT_SQ_SIZE             56u
#define CAE_OOO_DEFAULT_ISSUE_WIDTH         6u
#define CAE_OOO_DEFAULT_COMMIT_WIDTH        8u
#define CAE_OOO_DEFAULT_RENAME_WIDTH        8u
#define CAE_OOO_DEFAULT_NUM_PHYS_INT_REGS   224u
#define CAE_OOO_DEFAULT_NUM_PHYS_FLOAT_REGS 256u

/* Round 48: upper bound on phys-reg-pool sizes the RAT pre-allocates
 * for its free-list ring. Matches the plan-declared kmhv3.py defaults
 * with headroom; sizing the ring at compile-time keeps the snapshot
 * layout deterministic (no runtime re-allocation on restore) and
 * matches the existing "fixed-size slot arrays" pattern used by the
 * ROB. Architectural reg indices 0..31 occupy phys ids 1..32; the
 * allocator hands out ids 33..num_phys_int_regs from the free list. */
#define CAE_OOO_RAT_ARCH_REGS               32u
#define CAE_OOO_RAT_MAX_INT_PHYS            256u
#define CAE_OOO_RAT_MAX_FP_PHYS             288u

/*
 * One in-flight uop in the OoO core's bookkeeping. Round-12
 * BS-30 adds per-entry metadata so the commit walk can release
 * the exact LSQ / RAT / sbuffer slots the entry reserved at
 * dispatch time — before this, the commit loop mirrored the
 * current dispatch's `uop` instead of each committed entry's
 * own state, producing incorrect sbuffer drains on out-of-
 * order retire.
 *
 * Full kmhv3.py-style fields (port mapping, FU id, wakeup chain)
 * land alongside the scheduler refactor in M5'
 * (t-scheduler-kmhv3). For M3'/M4' this slim record is enough
 * to reach dispatch-commit coherence.
 */
typedef struct CaeOooEntry {
    uint64_t pc;
    uint8_t  fu_type;          /* CaeFuType */
    uint8_t  is_branch;
    uint8_t  is_mem;           /* any memory op (for scheduler hints) */
    uint8_t  is_load;          /* BS-30: so commit releases the LQ */
    uint8_t  is_store;         /* BS-30: so commit releases the SQ */
    uint8_t  num_dst_int;      /* BS-30: integer phys regs to free */
    uint8_t  num_dst_float;    /* BS-30: float phys regs to free */
    uint8_t  valid;
    uint32_t issue_cycle;      /* cycle the uop was issued to a FU */
    uint32_t ready_cycle;      /* cycle the uop's result is ready */
    /*
     * BS-30: sqn of the sbuffer entry this dispatched store
     * reserved, or 0 when no store was allocated (either because
     * the entry is not a store, or because sbuffer alloc failed
     * at dispatch). Commit walks use this sqn to call
     * cae_sbuffer_drain_head() on the correct entry, so an
     * older committed store drains its own payload instead of
     * the most recently dispatched store's.
     */
    uint64_t sbuffer_sqn;
    /*
     * Round 48 (AC-K-4 runtime-state closure): concrete RAT and
     * LSQ handles recorded at dispatch time so commit / squash /
     * live_restore can release the EXACT phys regs and LSQ entries
     * the dispatch reserved — not just matching counts.
     *
     *   dst_arch_{int,fp}: architectural reg id this destination
     *                      writes (0..31); CAE_OOO_INVALID_REG (0xff)
     *                      when the class has no destination.
     *   new_phys_{int,fp}: phys id the allocator handed out; 0
     *                      when no destination of that class.
     *   prev_phys_{int,fp}: phys id the arch reg was mapped to BEFORE
     *                       the allocation (squash rewinds map to
     *                       this; commit pushes this onto the free
     *                       list).
     *   lq_handle / sq_handle: ring index the LSQ allocator returned
     *                          at dispatch; CAE_OOO_INVALID_HANDLE
     *                          (0xffff) when no load/store.
     */
    uint8_t  dst_arch_int;
    uint8_t  dst_arch_fp;
    uint16_t new_phys_int;
    uint16_t new_phys_fp;
    uint16_t prev_phys_int;
    uint16_t prev_phys_fp;
    uint16_t lq_handle;
    uint16_t sq_handle;
} CaeOooEntry;

/* Sentinel values for the RAT/LSQ handles in CaeOooEntry. */
#define CAE_OOO_INVALID_REG     0xffu
#define CAE_OOO_INVALID_HANDLE  0xffffu

/* ROB: circular queue of in-flight retired uops. */
typedef struct CaeOooRob {
    CaeOooEntry *slots;
    uint32_t size;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} CaeOooRob;

/* IQ: flat unified issue queue for M3'; segmented into 3 x 2-port
 * slots in M5' via hw/cae/ooo/scheduler.c. */
typedef struct CaeOooIq {
    uint32_t size;
    uint32_t count;
    uint32_t issue_width;
} CaeOooIq;

/*
 * LSQ: load and store queues with per-entry ordering metadata.
 *
 * Round 48 (AC-K-4 runtime-state closure): each entry carries a
 * stable per-lifetime `seq` ordinal, architectural addr/size/value
 * (value for stores), and an `alloc_tick` timestamp so the live
 * speculation path can restore exact ring order across a mispredict
 * — not only queue occupancy. Round-46 review explicitly rejected
 * the prior count-only model as AC-K-4 closure; this structure
 * replaces it while keeping the count observables derived.
 *
 * Storage is a FIFO ring of CAE_OOO_LQ_CAPACITY / CAE_OOO_SQ_CAPACITY
 * entries (fixed at compile time to keep snapshot layouts
 * deterministic; the logical capacity `{lq,sq}_size` is a runtime
 * knob bounded by the ring size). Dispatch appends at `tail`,
 * commit drains from `head`; the `seq_next` counter assigns a
 * strictly monotonic handle to each allocation that survives
 * across squash/restore so tests can prove ordering identity.
 */
#define CAE_OOO_LQ_CAPACITY                 96u
#define CAE_OOO_SQ_CAPACITY                 72u

typedef struct CaeOooLqEntry {
    uint64_t seq;              /* monotonic handle, 0 = empty slot */
    uint64_t addr;
    uint64_t alloc_tick;       /* dispatch cycle — ordering proof */
    uint16_t size;
    uint8_t  committed;        /* stays set until ring slot is freed */
    uint8_t  reserved;
} CaeOooLqEntry;

typedef struct CaeOooSqEntry {
    uint64_t seq;
    uint64_t addr;
    uint64_t value;
    uint64_t alloc_tick;
    uint16_t size;
    uint8_t  committed;
    uint8_t  reserved;
} CaeOooSqEntry;

typedef struct CaeOooLsq {
    uint32_t lq_size;
    uint32_t lq_head;
    uint32_t lq_tail;
    uint32_t lq_count;
    uint64_t lq_seq_next;
    uint32_t sq_size;
    uint32_t sq_head;
    uint32_t sq_tail;
    uint32_t sq_count;
    uint64_t sq_seq_next;
    CaeOooLqEntry lq[CAE_OOO_LQ_CAPACITY];
    CaeOooSqEntry sq[CAE_OOO_SQ_CAPACITY];
} CaeOooLsq;

/* RAT: rename map + free list. M3' models the free-list pressure
 * only (physical-register allocation stall); the map itself is not
 * simulated for the functional oracle.
 *
 * Round 37 adds `int_alloc_seq`: a monotonic counter bumped on
 * every successful int-destination allocate/free. It is the first
 * observable increment of richer rename-map state per the plan's
 * "rename map + free list" requirement (plan.md:89) — it tracks
 * the PROVENANCE of allocate/free traffic even while the map
 * itself is still count-only. Unlike `int_inflight`, which
 * squash-restore returns to the save-time level, `int_alloc_seq`
 * is snapshot-roundtrippable AND strictly monotonic within a
 * single timeline: a squash that rewinds `int_inflight` from 7
 * back to 3 also rewinds `int_alloc_seq` back to its save-time
 * value, so the two fields together let a regression prove both
 * "state rolled back" AND "the provenance chain restored, not
 * continued". `cae_ooo_rat_flush` leaves it unchanged because
 * flush is an architectural reset of inflight state, not a
 * squash-restore of provenance.
 */
typedef struct CaeOooRat {
    uint32_t num_phys_int_regs;
    uint32_t num_phys_float_regs;
    uint32_t int_inflight;
    uint32_t fp_inflight;
    uint64_t int_alloc_seq;
    /*
     * Round 48 (AC-K-4): concrete rename maps + free lists replace
     * the round-37 pressure-only accounting. `int_map[i]` / `fp_map[i]`
     * holds the phys-id currently bound to architectural reg `i`
     * (reserved phys-ids 1..32 mirror arch regs; the allocator pool
     * hands out 33..num_phys_{int,float}_regs from `int_free` /
     * `fp_free`). Free-list count tracks available entries. The
     * existing `int_inflight`, `fp_inflight`, and `int_alloc_seq`
     * fields stay observable as derived summaries: after any map
     * mutation they equal `num_phys_{int,float}_regs - 32 -
     * {int,fp}_free_count` — the tests that depend on them (round-37
     * provenance, round-34 live restore) keep passing unchanged.
     *
     * The live speculation path's save/restore round-trips the maps
     * AND the free lists byte-for-byte; a squash restores exact
     * bindings + free-list contents rather than rewinding a count.
     */
    uint16_t int_map[CAE_OOO_RAT_ARCH_REGS];
    uint16_t fp_map[CAE_OOO_RAT_ARCH_REGS];
    uint16_t int_free[CAE_OOO_RAT_MAX_INT_PHYS];
    uint16_t fp_free[CAE_OOO_RAT_MAX_FP_PHYS];
    uint32_t int_free_count;
    uint32_t fp_free_count;
} CaeOooRat;

/*
 * Construct/destroy helpers for the sub-structures. All allocation
 * is scoped to the owning CaeCpuOoo instance; the sub-structures
 * themselves are POD with explicit allocation of their dynamic
 * storage.
 */
void cae_ooo_rob_init(CaeOooRob *rob, uint32_t size);
void cae_ooo_rob_destroy(CaeOooRob *rob);
/*
 * Non-destructive head check: returns true if the ROB has space
 * for one more dispatch. Callers use this to pre-gate resource
 * allocation so LSQ / sbuffer reservations never leak when ROB
 * is full (BS-30 round 12).
 */
bool cae_ooo_rob_has_slot(const CaeOooRob *rob);
/*
 * Dispatch a uop with the sqn of its sbuffer reservation (0 if
 * not a store or sbuffer alloc failed). The caller is
 * responsible for all resource pre-checks; this helper just
 * writes the slot.
 *
 * Round 48 (AC-K-4): concrete RAT / LSQ handles captured at
 * dispatch so commit / squash / live_restore can release the
 * EXACT phys regs and ring entries the dispatch reserved, not
 * just matching counts. Sentinels (CAE_OOO_INVALID_REG /
 * CAE_OOO_INVALID_HANDLE / 0 for phys-ids) are accepted for
 * callers that have not yet allocated a given resource (test
 * seed helpers still pass sentinels for fields they do not
 * exercise).
 */
typedef struct CaeOooDispatchHandles {
    uint8_t  dst_arch_int;
    uint8_t  dst_arch_fp;
    uint16_t new_phys_int;
    uint16_t new_phys_fp;
    uint16_t prev_phys_int;
    uint16_t prev_phys_fp;
    uint16_t lq_handle;
    uint16_t sq_handle;
} CaeOooDispatchHandles;

bool cae_ooo_rob_dispatch(CaeOooRob *rob, const CaeUop *uop,
                          uint32_t dispatch_cycle,
                          uint64_t sbuffer_sqn);
bool cae_ooo_rob_dispatch_ex(CaeOooRob *rob, const CaeUop *uop,
                             uint32_t dispatch_cycle,
                             uint64_t sbuffer_sqn,
                             const CaeOooDispatchHandles *handles);
/*
 * Commit one head entry if ready. Returns true and fills *out
 * with the retired entry's metadata (including sbuffer_sqn,
 * is_load/is_store, num_dst_*) when the head's ready_cycle has
 * passed; returns false when the head is not ready or the ROB
 * is empty. Callers loop up to commit_width to drain one
 * retire's worth.
 */
bool cae_ooo_rob_commit_one(CaeOooRob *rob, uint32_t now_cycle,
                            CaeOooEntry *out);
/*
 * Bulk-count commit wrapper for back-compat. Loops
 * cae_ooo_rob_commit_one up to commit_width and returns the
 * count committed. Kept for callers that do not need per-entry
 * metadata.
 */
uint32_t cae_ooo_rob_try_commit(CaeOooRob *rob, uint32_t now_cycle,
                                uint32_t commit_width);
void cae_ooo_rob_flush_after(CaeOooRob *rob);

void cae_ooo_iq_init(CaeOooIq *iq, uint32_t size, uint32_t issue_width);
bool cae_ooo_iq_has_slot(const CaeOooIq *iq);
uint32_t cae_ooo_iq_try_issue(CaeOooIq *iq, uint32_t ready_uops,
                              uint32_t now_cycle);
void cae_ooo_iq_enqueue(CaeOooIq *iq);
void cae_ooo_iq_flush(CaeOooIq *iq);

void cae_ooo_lsq_init(CaeOooLsq *lsq, uint32_t lq_size, uint32_t sq_size);
bool cae_ooo_lsq_has_load_slot(const CaeOooLsq *lsq);
bool cae_ooo_lsq_has_store_slot(const CaeOooLsq *lsq);
bool cae_ooo_lsq_allocate_load(CaeOooLsq *lsq);
bool cae_ooo_lsq_allocate_store(CaeOooLsq *lsq);
void cae_ooo_lsq_commit_load(CaeOooLsq *lsq);
void cae_ooo_lsq_commit_store(CaeOooLsq *lsq);
void cae_ooo_lsq_flush(CaeOooLsq *lsq);

/*
 * Round 48 (AC-K-4): per-entry allocate/commit helpers that return a
 * concrete ring-index handle instead of only incrementing counts.
 * The handle is CAE_OOO_INVALID_HANDLE on allocation failure; the
 * caller stores it in the ROB entry (lq_handle / sq_handle) so
 * commit releases the exact entry that was reserved at dispatch,
 * not just "the next entry at ring head". Commit asserts that the
 * handle corresponds to the current ring head — in-order
 * architectural retire requires FIFO release under the plan's
 * one-insn-per-tb execution contract.
 *
 * `alloc_tick` timestamps dispatch so restore-sensitive regressions
 * can prove ring ORDER survives squash, not just counts.
 */
uint16_t cae_ooo_lsq_allocate_load_entry(CaeOooLsq *lsq, uint64_t addr,
                                         uint16_t size,
                                         uint64_t alloc_tick);
uint16_t cae_ooo_lsq_allocate_store_entry(CaeOooLsq *lsq, uint64_t addr,
                                          uint16_t size, uint64_t value,
                                          uint64_t alloc_tick);
void cae_ooo_lsq_commit_load_handle(CaeOooLsq *lsq, uint16_t handle);
void cae_ooo_lsq_commit_store_handle(CaeOooLsq *lsq, uint16_t handle);
const CaeOooLqEntry *cae_ooo_lsq_peek_load(const CaeOooLsq *lsq,
                                           uint16_t handle);
const CaeOooSqEntry *cae_ooo_lsq_peek_store(const CaeOooLsq *lsq,
                                            uint16_t handle);

void cae_ooo_rat_init(CaeOooRat *rat, uint32_t num_int, uint32_t num_fp);
bool cae_ooo_rat_has_slot(const CaeOooRat *rat, const CaeUop *uop);
bool cae_ooo_rat_allocate(CaeOooRat *rat, const CaeUop *uop);
void cae_ooo_rat_free(CaeOooRat *rat, const CaeUop *uop);
/*
 * BS-30 round 12: free by explicit register-class counts taken
 * from the committed ROB entry. Callers that no longer have the
 * original CaeUop (because commit happens out-of-order) use this
 * form with the entry's num_dst_int / num_dst_float fields.
 */
void cae_ooo_rat_free_counts(CaeOooRat *rat, uint32_t int_count,
                             uint32_t fp_count);
void cae_ooo_rat_flush(CaeOooRat *rat);
uint64_t cae_ooo_rat_int_alloc_seq(const CaeOooRat *rat);

/*
 * Round 48 (AC-K-4): concrete rename-map allocators. Each
 * `allocate_dst_*` pops a phys id from the class's free list,
 * captures the previous mapping for `arch_dst` (so commit /
 * squash know what to restore), overwrites the map, and returns
 * the new phys id (or 0 on failure — arch regs occupy 1..32 so
 * 0 is a safe sentinel). `free_dst_*` pushes `prev_phys` back
 * onto the free list and rewrites the map so future reads of
 * `arch_dst` see `prev_phys`. These pair with the ROB entry's
 * dst_arch_/new_phys_/prev_phys_ fields.
 *
 * `prev_phys_out` returns the phys id that was mapped to
 * `arch_dst` before the allocation — the ROB entry captures
 * this so commit / squash can free or rewind cleanly.
 * Pass NULL if the caller does not need the value.
 *
 * arch_dst == 0 (the RISC-V zero reg) is a no-op: returns 0,
 * leaves the free list alone. This mirrors the architectural
 * "writes to x0 are dropped" contract and keeps test drivers
 * from accidentally consuming phys ids for non-writing uops.
 */
uint16_t cae_ooo_rat_allocate_dst_int(CaeOooRat *rat, uint8_t arch_dst,
                                      uint16_t *prev_phys_out);
uint16_t cae_ooo_rat_allocate_dst_fp(CaeOooRat *rat, uint8_t arch_dst,
                                     uint16_t *prev_phys_out);
void cae_ooo_rat_free_dst_int(CaeOooRat *rat, uint8_t arch_dst,
                              uint16_t new_phys, uint16_t prev_phys);
void cae_ooo_rat_free_dst_fp(CaeOooRat *rat, uint8_t arch_dst,
                             uint16_t new_phys, uint16_t prev_phys);
uint16_t cae_ooo_rat_map_int(const CaeOooRat *rat, uint8_t arch_reg);
uint16_t cae_ooo_rat_map_fp(const CaeOooRat *rat, uint8_t arch_reg);

/* ------------------------------------------------------------------ */
/*  Speculation save/restore: OoO container lanes                     */
/* ------------------------------------------------------------------ */

/*
 * Each OoO container below is a heap-allocated POD owned by the
 * composing CaeCpuOoo. The M4' speculation path needs to unwind
 * full container contents on a mispredict, not just the scalar
 * CaeCpuOoo clock / store_sqn_next lane. The round-28/29
 * "owning-module owns its snapshot" pattern applies even though
 * these containers are not QOM objects: the opaque snapshot struct
 * is defined in the owning .c, callers treat the returned pointer
 * as opaque, drop is the only owner-responsibility cleanup.
 *
 * save returns NULL when `src` is NULL (safe no-op); restore is a
 * no-op when either argument is NULL; drop is a no-op on NULL.
 * Within-run contract: the ROB slots[] array is allocated once at
 * init with a fixed size, so rob-snapshot restore requires the
 * caller's rob to have the same `size` as the snapshot. A
 * mismatched size is treated as a programming error (the restore
 * falls through without state change) — the single caller
 * (CaeCpuOoo composer) never changes rob->size across snapshots.
 */
typedef struct CaeOooRobSpecSnapshot CaeOooRobSpecSnapshot;
typedef struct CaeOooIqSpecSnapshot  CaeOooIqSpecSnapshot;
typedef struct CaeOooLsqSpecSnapshot CaeOooLsqSpecSnapshot;
typedef struct CaeOooRatSpecSnapshot CaeOooRatSpecSnapshot;

CaeOooRobSpecSnapshot *cae_ooo_rob_spec_snapshot_save(const CaeOooRob *src);
void cae_ooo_rob_spec_snapshot_restore(CaeOooRob *dst,
                                       const CaeOooRobSpecSnapshot *snap);
void cae_ooo_rob_spec_snapshot_drop(CaeOooRobSpecSnapshot *snap);

CaeOooIqSpecSnapshot *cae_ooo_iq_spec_snapshot_save(const CaeOooIq *src);
void cae_ooo_iq_spec_snapshot_restore(CaeOooIq *dst,
                                      const CaeOooIqSpecSnapshot *snap);
void cae_ooo_iq_spec_snapshot_drop(CaeOooIqSpecSnapshot *snap);

CaeOooLsqSpecSnapshot *cae_ooo_lsq_spec_snapshot_save(const CaeOooLsq *src);
void cae_ooo_lsq_spec_snapshot_restore(CaeOooLsq *dst,
                                       const CaeOooLsqSpecSnapshot *snap);
void cae_ooo_lsq_spec_snapshot_drop(CaeOooLsqSpecSnapshot *snap);

CaeOooRatSpecSnapshot *cae_ooo_rat_spec_snapshot_save(const CaeOooRat *src);
void cae_ooo_rat_spec_snapshot_restore(CaeOooRat *dst,
                                       const CaeOooRatSpecSnapshot *snap);
void cae_ooo_rat_spec_snapshot_drop(CaeOooRatSpecSnapshot *snap);

/* ------------------------------------------------------------------ */
/*  AC-K-5: KMHV3Scheduler (3-segment IQ × 2 ports)                  */
/* ------------------------------------------------------------------ */

/*
 * M5' scheduler modeling the KMH-V3 3-segment issue queue: each
 * segment's ready-queue is a fixed-size ring; the scheduler issues
 * up to 2 entries per cycle by round-robin over the 3 segments.
 * Entries in the segmented ring carry a uop type hint so the
 * scheduler can bias the issue decision (ALU / MEM / FPU) toward
 * the correct port. Full per-FU latency / port contention lives in
 * cpu_ooo.c's charge loop; this structure is the segmented-issue
 * BOOKKEEPING the plan requires as a distinct OoO module.
 */
#define CAE_OOO_SCHED_SEGMENTS           3u
#define CAE_OOO_SCHED_PORTS              2u  /* default issue-port count */
#define CAE_OOO_SCHED_PORTS_MAX          8u  /* structural upper bound */
#define CAE_OOO_SCHED_SEGMENT_CAPACITY   64u

typedef struct CaeOooSchedEntry {
    uint64_t pc;
    uint8_t  fu_type;
    uint8_t  segment;
    uint8_t  valid;
    uint8_t  reserved;
} CaeOooSchedEntry;

typedef struct CaeOooSchedSegment {
    CaeOooSchedEntry slots[CAE_OOO_SCHED_SEGMENT_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t capacity;
} CaeOooSchedSegment;

typedef struct CaeOooScheduler {
    CaeOooSchedSegment segments[CAE_OOO_SCHED_SEGMENTS];
    uint32_t next_segment;      /* round-robin pointer for issue */
    uint64_t enqueued;          /* stat: total entries accepted */
    uint64_t issued;            /* stat: total entries issued */
    uint64_t backpressure;      /* stat: enqueue refused (full) */
} CaeOooScheduler;

void cae_ooo_scheduler_init(CaeOooScheduler *s);
void cae_ooo_scheduler_reset(CaeOooScheduler *s);
/*
 * Pick a segment based on the uop's FU type. Returns a value in
 * [0, CAE_OOO_SCHED_SEGMENTS). Encapsulated so callers and
 * regressions can use the same mapping.
 */
uint8_t cae_ooo_scheduler_segment_for(uint8_t fu_type);
bool cae_ooo_scheduler_enqueue(CaeOooScheduler *s, uint64_t pc,
                               uint8_t fu_type);
/*
 * Issue up to the configured port count entries in this cycle.
 * Returns the number of entries issued and fills out[] in the
 * order they were picked.  Caller provides an out[] buffer of
 * size CAE_OOO_SCHED_PORTS_MAX.
 */
/*
 * Round 50: `max_issue == 0` uses the structural default
 * (CAE_OOO_SCHED_PORTS). Non-zero caps issues to
 * `min(max_issue, CAE_OOO_SCHED_PORTS)` so live callers can
 * throttle the issue width from the cpu-model's issue_width
 * knob and observe the delta via `scheduler-issued`.
 */
uint8_t cae_ooo_scheduler_issue_cycle(CaeOooScheduler *s,
                                      CaeOooSchedEntry *out);
uint8_t cae_ooo_scheduler_issue_cycle_bounded(CaeOooScheduler *s,
                                              uint8_t max_issue,
                                              CaeOooSchedEntry *out);

/* ------------------------------------------------------------------ */
/*  AC-K-5: RARQ + RAWQ ordering violation + load-miss replay        */
/* ------------------------------------------------------------------ */

/*
 * M5' ordering-violation tracker. RARQ and RAWQ are both bounded
 * rings; load-miss-replay retains a small slot for the most
 * recent missed load that the scheduler must replay after fill.
 *
 * RARQ semantics (Read-After-Read): two loads to overlapping
 * addresses that committed out of architectural program order
 * flag a re-order-check event. The plan treats this as a
 * consistency invariant — RVWMO permits loads to reorder as
 * long as dependent data races are handled by fences; the
 * counter proves the machinery is in place.
 *
 * RAWQ semantics (Read-After-Write): a load that commits before
 * a store to the same address which was issued earlier in
 * program order flags a violation and forces replay.
 *
 * Both queues are FIFO rings; overflow drops the oldest entry
 * and bumps the drop counter. Check APIs return true on a
 * detected violation.
 */
#define CAE_OOO_VIOL_RARQ_CAPACITY   16u
#define CAE_OOO_VIOL_RAWQ_CAPACITY   16u

typedef struct CaeOooViolEntry {
    uint64_t seq;          /* monotonic ordering handle */
    uint64_t addr;
    uint16_t size;
    uint8_t  valid;
    uint8_t  reserved;
} CaeOooViolEntry;

typedef struct CaeOooViolReplaySlot {
    uint64_t seq;
    uint64_t addr;
    uint16_t size;
    uint8_t  valid;
    uint8_t  reserved;
} CaeOooViolReplaySlot;

typedef struct CaeOooViolation {
    CaeOooViolEntry rarq[CAE_OOO_VIOL_RARQ_CAPACITY];
    uint32_t rarq_head;
    uint32_t rarq_tail;
    uint32_t rarq_count;
    CaeOooViolEntry rawq[CAE_OOO_VIOL_RAWQ_CAPACITY];
    uint32_t rawq_head;
    uint32_t rawq_tail;
    uint32_t rawq_count;
    uint64_t seq_next;
    CaeOooViolReplaySlot replay;

    /* Stats. */
    uint64_t loads_observed;
    uint64_t stores_observed;
    uint64_t raw_violations;
    uint64_t rar_reorders;
    uint64_t replay_consumed;
    uint64_t drops;
} CaeOooViolation;

void cae_ooo_violation_init(CaeOooViolation *v);
void cae_ooo_violation_reset(CaeOooViolation *v);
void cae_ooo_violation_record_load(CaeOooViolation *v, uint64_t addr,
                                   uint16_t size);
void cae_ooo_violation_record_store(CaeOooViolation *v, uint64_t addr,
                                    uint16_t size);
/*
 * Check whether a new load at (addr, size) would observe an
 * architectural RAW violation against a live earlier store in the
 * RAWQ (overlapping address range). If it does, flag the load for
 * replay: the replay slot is populated and `raw_violations` is
 * bumped. Returns true when a violation was detected (caller
 * stalls / replays that load).
 */
bool cae_ooo_violation_check_raw(CaeOooViolation *v, uint64_t addr,
                                 uint16_t size);
/*
 * Consume the current replay slot, returning its contents in *out.
 * Returns true when a replay was pending (slot valid) and *out was
 * filled; false when the slot was empty.
 */
bool cae_ooo_violation_consume_replay(CaeOooViolation *v,
                                      CaeOooViolReplaySlot *out);

#endif /* CAE_OOO_H */
