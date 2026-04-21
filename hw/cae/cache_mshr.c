/*
 * CAE cache + MSHR wrapper.
 *
 * Implements TYPE_CAE_MEM (CaeMemClass) by composing on top of a
 * downstream CaeCache + DRAM chain, tracking the parallel-outstanding-
 * miss count and applying an overlap discount when mshr_size > 1.
 *
 * Round 19 t-mem-async-iface: completion tracking is now keyed by
 * absolute global cycles sourced from CaeMemReq.now_cycle. The
 * outstanding-miss table records `req->now_cycle + downstream
 * latency`; can_accept() and the outstanding-misses QOM property
 * expire against cae_engine_current_cycle() so observers (LSQ /
 * engine / QMP) see the true in-flight state instead of an
 * access-internal local clock.
 *
 * AC-K-3.2 surface: setting mshr_size=1 reduces this to sync cache
 * behaviour (no overlap); mshr_size>=2 credits concurrent outstanding
 * misses against the returned latency in proportion to the observed
 * parallel-count. Bank-conflict modeling (AC-K-5) lands later.
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
#include "cae/mem.h"
#include "cae/cache_mshr.h"
#include "cae/engine.h"

OBJECT_DECLARE_SIMPLE_TYPE(CaeCacheMshr, CAE_CACHE_MSHR)

/*
 * Max MSHR entries tracked by the sync-mode accounting window
 * (round 4). Sized so even an overprovisioned xs-1c-functional
 * mshr_size (up to 64 on kmhv3.py reference) fits without dynamic
 * allocation. The real async contract (t-mem-async-iface, round 5)
 * replaces this static table with an event-queue-backed entry pool.
 */
#define CAE_MSHR_MAX_ENTRIES 64u

/*
 * Round 49 AC-K-5: simple L1D bank-conflict model. The kmhv3.py
 * reference splits L1D into N banks (default 8 on KMH-V3);
 * concurrent accesses whose line-aligned addresses map to the
 * same bank at the same cycle incur a bank_conflict_stall_cycles
 * penalty. We implement this by tracking the last-observed cycle
 * per bank slot; if a new access at the same bank hits within
 * (last_cycle + bank_conflict_stall_cycles), the access's
 * effective latency is bumped by the remaining stall and the
 * bank_conflict_events counter is incremented.
 */
#define CAE_MSHR_BANK_COUNT_MAX 16u

struct CaeCacheMshr {
    Object parent;

    /* User-tunable parameters. */
    uint32_t mshr_size;
    uint32_t fill_queue_size;
    uint32_t writeback_queue_size;
    uint32_t bank_count;                 /* 0 or 1 disables banking */
    uint32_t bank_conflict_stall_cycles; /* 0 disables stalling */

    /* Downstream: the existing cae-cache object, which already owns
     * the tag/data array + DRAM chain. We simply credit its latency
     * when parallel outstanding accesses overlap. */
    Object *downstream;
    CaeMemClass *downstream_mc;

    /*
     * Round 19 t-mem-async-iface: absolute-cycle outstanding-miss
     * table. Each entry is the global-clock cycle at which that
     * miss's data becomes available (req->now_cycle + downstream
     * latency). On every access we expire entries whose completion
     * cycle has passed the caller's now_cycle; can_accept() and the
     * outstanding-misses QOM property expire against
     * cae_engine_current_cycle() so LSQ / QMP can observe real
     * outstanding state instead of the old local_cycle
     * approximation.
     */
    uint64_t completion_cycles[CAE_MSHR_MAX_ENTRIES];
    uint32_t n_entries;

    /*
     * Round 49 AC-K-5 bank-conflict state: last access cycle per
     * bank slot plus a first-touch bitmap. An access at same_bank
     * with now_cycle < bank_last_cycle + bank_conflict_stall_cycles
     * pays a stall once `bank_seen[bank]` is set (the first
     * access at each bank is by definition not a conflict even if
     * now_cycle == 0 happens to equal the default-initialised
     * bank_last_cycle).
     */
    uint64_t bank_last_cycle[CAE_MSHR_BANK_COUNT_MAX];
    bool     bank_seen[CAE_MSHR_BANK_COUNT_MAX];

    /* Stats. */
    uint64_t accesses;
    uint64_t parallel_events;  /* accesses that overlapped another */
    uint64_t overlap_cycles_saved;
    uint64_t bank_conflict_events;
    uint64_t bank_conflict_cycles_added;

    bool configured;
};

/* Round 49 AC-K-5: hash an access addr to one of `bank_count`
 * banks. Simple line-aligned modulo mapping — matches kmhv3.py
 * default behaviour. `bank_count` 0 or 1 short-circuits to 0. */
static uint32_t cache_mshr_bank_for_addr(const CaeCacheMshr *m,
                                         uint64_t addr)
{
    if (m->bank_count <= 1u) {
        return 0u;
    }
    uint32_t banks = m->bank_count;
    if (banks > CAE_MSHR_BANK_COUNT_MAX) {
        banks = CAE_MSHR_BANK_COUNT_MAX;
    }
    /* Drop the cache-line low bits (assume 64-byte line). */
    return (uint32_t)((addr >> 6u) % banks);
}

/*
 * Round 19 t-mem-async-iface: expire completion entries whose
 * absolute completion cycle has passed the caller-supplied
 * observation cycle. Compacts the table in-place. Returns the count
 * of still-outstanding entries after expiry.
 *
 * Semantic: `completion_cycle` is "the cycle at which this miss's
 * data becomes available". At exactly that cycle the data is just
 * arriving - treated as still in-flight so I/O-thread observers
 * (QMP outstanding-misses) can catch the window between the
 * completion and the engine's next cycle advance. Entries are
 * dropped only when strictly older than now_cycle. This is
 * critical for live visibility in the current sync-advance engine:
 * a stricter `>` rule would make outstanding-misses collapse to 0
 * every time the engine absorbs an access's full latency, hiding
 * real in-flight state from QMP / LSQ observers.
 */
static uint32_t mshr_expire_at(CaeCacheMshr *m, uint64_t now_cycle)
{
    uint32_t write = 0;
    for (uint32_t i = 0; i < m->n_entries; i++) {
        if (m->completion_cycles[i] >= now_cycle) {
            if (write != i) {
                m->completion_cycles[write] = m->completion_cycles[i];
            }
            write++;
        }
    }
    m->n_entries = write;
    return write;
}

static CaeMemResp cae_cache_mshr_access(Object *obj, CaeMemReq *req)
{
    CaeCacheMshr *m = CAE_CACHE_MSHR(obj);
    CaeMemResp resp = { .latency = 0, .result = CAE_MEM_HIT,
                        .opaque = NULL,
                        .completion_cycle = req->now_cycle };
    CaeMemResp downstream_resp;

    if (!m->downstream_mc || !m->downstream_mc->access) {
        return resp;
    }

    /*
     * Round 19 t-mem-async-iface: expire completions against the
     * caller's global cycle, then measure concurrent peers. Peers
     * are misses whose completion cycle is strictly after
     * req->now_cycle — i.e. data not yet available when this access
     * arrives. Back-to-back misses at the same now_cycle see each
     * other via the table, so the Nth miss in a co-dispatched burst
     * sees (N-1) peers (capped at mshr_size-1) and shares the bus
     * accordingly.
     */
    uint32_t outstanding = mshr_expire_at(m, req->now_cycle);

    /*
     * Round 49 AC-K-5: L1D bank-conflict stall. When two accesses
     * hash to the same bank slot and the second arrives before
     * (last_bank_cycle + bank_conflict_stall_cycles), the second
     * pays a stall equal to the remaining window. Adds to the
     * effective latency reported to the caller and bumps the
     * dedicated counter. A configurable `bank_count` of 0 or 1
     * disables the model (round-2 backwards-compat default).
     */
    uint32_t bank_stall = 0u;
    if (m->bank_count > 1u && m->bank_conflict_stall_cycles > 0u) {
        uint32_t bank = cache_mshr_bank_for_addr(m, req->addr);
        if (!m->bank_seen[bank]) {
            /* First-ever touch at this bank: no prior conflict. */
            m->bank_seen[bank] = true;
            m->bank_last_cycle[bank] = req->now_cycle;
        } else {
            uint64_t busy_until = m->bank_last_cycle[bank] +
                                  m->bank_conflict_stall_cycles;
            if (req->now_cycle < busy_until) {
                bank_stall = (uint32_t)(busy_until - req->now_cycle);
                m->bank_conflict_events++;
                m->bank_conflict_cycles_added += bank_stall;
                m->bank_last_cycle[bank] = busy_until;
            } else {
                m->bank_last_cycle[bank] = req->now_cycle;
            }
        }
    }

    /* Dispatch the underlying access synchronously. The MSHR
     * models overlap/backpressure; the downstream cache + DRAM
     * chain still produces data on the functional path. */
    downstream_resp = m->downstream_mc->access(m->downstream, req);
    uint32_t original = downstream_resp.latency;

    /*
     * AC-K-3.2 overlap credit. The effective latency charged to the
     * caller is scaled down by the number of peer misses still in
     * flight, capped at (mshr_size - 1). mshr_size=1 collapses the
     * cap to zero so the wrapper serialises exactly like a
     * passthrough. Integer math keeps the cycle stream
     * deterministic.
     *
     * Round 20 fix: when the MSHR table is saturated (no capacity
     * for a new in-flight entry) the incoming request is
     * serialised — full downstream latency, no overlap discount,
     * no parallel-event increment. The round-19 implementation
     * computed `effective` unconditionally and only guarded entry
     * insertion, leaving a saturated-MSHR request with discounted
     * latency and an inflated parallel-events counter. Moving the
     * capacity check up front keeps the saturation contract
     * honest for both returned latency and stats.
     */
    bool have_slot = (original > 0 &&
                      m->n_entries < m->mshr_size &&
                      m->n_entries < CAE_MSHR_MAX_ENTRIES);

    uint32_t effective = original;
    if (have_slot) {
        uint32_t peers = outstanding;
        uint32_t cap = (m->mshr_size > 0) ? (m->mshr_size - 1u) : 0u;
        if (peers > cap) {
            peers = cap;
        }
        if (peers > 0 && original > 0) {
            effective = original / (1u + peers);
            if (effective < 1u) {
                effective = 1u;
            }
            m->overlap_cycles_saved += (original - effective);
            m->parallel_events++;
        }
        /*
         * Record the new outstanding entry. Completion is the
         * absolute cycle the downstream reported (original, not
         * effective — the miss physically completes when data
         * arrives, not at the discounted observation time).
         */
        uint64_t completion;
        if (downstream_resp.completion_cycle > req->now_cycle) {
            completion = downstream_resp.completion_cycle;
        } else {
            /* Legacy backend that didn't fill completion_cycle. */
            completion = req->now_cycle + original;
        }
        m->completion_cycles[m->n_entries++] = completion;
    }

    /* Round 49 AC-K-5: bank-conflict stall adds to effective latency. */
    effective += bank_stall;

    downstream_resp.latency = effective;
    /*
     * Completion_cycle the caller observes: the absolute cycle at
     * which this access's data is usable. We report the full
     * downstream completion regardless of overlap discount — two
     * misses dispatched at now_cycle=0 each return
     * completion_cycle = now_cycle + miss_latency even though their
     * effective latency is halved. The engine charges `latency`
     * cycles (discounted) but outstanding-misses observers see the
     * true in-flight window.
     */
    if (downstream_resp.completion_cycle <= req->now_cycle) {
        downstream_resp.completion_cycle = req->now_cycle + original;
    }
    m->accesses++;
    return downstream_resp;
}

/*
 * Round 20 t-mem-async-iface: deferred-completion event state.
 * Heap-allocated because the CaeEvent lives in the engine queue
 * until its cycle fires. The handler frees the struct on its way
 * out so callers do not need to track completions themselves.
 */
struct CaeMshrAsyncCompletion {
    CaeEvent ev;
    CaeMemResp resp;
    CaeMemRespCb cb;
    void *cb_opaque;
};

static void cae_cache_mshr_completion_handler(void *opaque)
{
    struct CaeMshrAsyncCompletion *ctx = opaque;
    if (ctx->cb) {
        ctx->cb(&ctx->resp, ctx->cb_opaque);
    }
    g_free(ctx);
}

/*
 * Round 20 t-mem-async-iface: real deferred-completion dispatch.
 * The MSHR's functional response is still computed synchronously
 * (the downstream cache + DRAM chain is the data oracle), but the
 * caller's callback is NOT fired immediately — it is scheduled as
 * a CaeEvent at the response's absolute `completion_cycle` so the
 * engine event queue drives the callback at the right global
 * cycle, matching the plan's "callback queue" contract. If the
 * global engine is absent (unit-test direct invocation) or the
 * scheduled cycle is in the past (0-latency stub fallthrough) the
 * callback fires immediately so existing consumers stay working.
 */
static bool cae_cache_mshr_access_async(Object *obj, CaeMemReq *req,
                                        CaeMemRespCb cb, void *cb_opaque)
{
    CaeMemResp resp = cae_cache_mshr_access(obj, req);
    CaeEngine *engine = cae_get_engine();

    if (!engine) {
        if (cb) {
            cb(&resp, cb_opaque);
        }
        return true;
    }

    struct CaeMshrAsyncCompletion *ctx =
        g_new0(struct CaeMshrAsyncCompletion, 1);
    ctx->resp = resp;
    ctx->cb = cb;
    ctx->cb_opaque = cb_opaque;
    ctx->ev.cycle = resp.completion_cycle;
    ctx->ev.handler = cae_cache_mshr_completion_handler;
    ctx->ev.opaque = ctx;

    if (!cae_engine_schedule_event(engine, &ctx->ev)) {
        /*
         * schedule_event rejects entries in the past. This is the
         * zero-latency / same-cycle shortcut — fire the callback
         * synchronously so the caller still observes completion.
         */
        if (cb) {
            cb(&ctx->resp, ctx->cb_opaque);
        }
        g_free(ctx);
    }
    return true;
}

static bool cae_cache_mshr_can_accept(Object *obj)
{
    CaeCacheMshr *m = CAE_CACHE_MSHR(obj);
    uint32_t outstanding = mshr_expire_at(m, cae_engine_current_cycle());
    return outstanding < m->mshr_size;
}

static void cae_cache_mshr_get_outstanding(Object *obj, Visitor *v,
                                           const char *name,
                                           void *opaque, Error **errp)
{
    CaeCacheMshr *m = CAE_CACHE_MSHR(obj);
    uint32_t value = mshr_expire_at(m, cae_engine_current_cycle());
    visit_type_uint32(v, name, &value, errp);
}

static void cae_cache_mshr_complete(UserCreatable *uc, Error **errp)
{
    CaeCacheMshr *m = CAE_CACHE_MSHR(uc);

    if (!m->downstream) {
        error_setg(errp, "cae-cache-mshr: downstream link is unset");
        return;
    }
    m->downstream_mc = CAE_MEM_CLASS(object_class_dynamic_cast(
        object_get_class(m->downstream), TYPE_CAE_MEM));
    if (!m->downstream_mc) {
        error_setg(errp, "cae-cache-mshr: downstream does not implement "
                         "TYPE_CAE_MEM");
        return;
    }
    if (m->mshr_size == 0u) {
        m->mshr_size = CAE_MSHR_DEFAULT_SIZE;
    }
    if (m->fill_queue_size == 0u) {
        m->fill_queue_size = CAE_MSHR_DEFAULT_FILL_Q;
    }
    if (m->writeback_queue_size == 0u) {
        m->writeback_queue_size = CAE_MSHR_DEFAULT_WB_Q;
    }
    m->n_entries = 0;
    m->configured = true;
}

static void cae_cache_mshr_check_downstream(const Object *obj,
                                            const char *name,
                                            Object *val, Error **errp)
{
    (void)obj;
    (void)name;
    (void)val;
    (void)errp;
}

static void cae_cache_mshr_instance_init(Object *obj)
{
    CaeCacheMshr *m = CAE_CACHE_MSHR(obj);

    m->mshr_size = CAE_MSHR_DEFAULT_SIZE;
    m->fill_queue_size = CAE_MSHR_DEFAULT_FILL_Q;
    m->writeback_queue_size = CAE_MSHR_DEFAULT_WB_Q;
    /* Round 49 AC-K-5 bank-conflict defaults: disabled (bank_count <= 1). */
    m->bank_count = 0u;
    m->bank_conflict_stall_cycles = 0u;

    object_property_add_uint32_ptr(obj, "mshr-size", &m->mshr_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "fill-queue-size",
                                   &m->fill_queue_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "writeback-queue-size",
                                   &m->writeback_queue_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "bank-count",
                                   &m->bank_count,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "bank-conflict-stall-cycles",
                                   &m->bank_conflict_stall_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "bank-conflict-events",
                                   &m->bank_conflict_events,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "bank-conflict-cycles-added",
                                   &m->bank_conflict_cycles_added,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_link(obj, "downstream", TYPE_CAE_MEM,
                             &m->downstream,
                             cae_cache_mshr_check_downstream,
                             OBJ_PROP_LINK_STRONG);
    /*
     * Read-only stats surface (AC-K-3.2 regression test hook).
     * parallel-events increments each time an access saw at least
     * one peer in flight; overlap-cycles-saved tallies the latency
     * reduction delivered by the overlap credit.
     */
    object_property_add_uint64_ptr(obj, "accesses",
                                   &m->accesses, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "parallel-events",
                                   &m->parallel_events,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "overlap-cycles-saved",
                                   &m->overlap_cycles_saved,
                                   OBJ_PROP_FLAG_READ);
    /*
     * Round 19 t-mem-async-iface: read-only outstanding-misses
     * probe. Expires against the engine's current global cycle on
     * each read so LSQ / engine / QMP observers see the real
     * in-flight count instead of the old local_cycle approximation.
     */
    object_property_add(obj, "outstanding-misses", "uint32",
                        cae_cache_mshr_get_outstanding,
                        NULL, NULL, NULL);
}

static void cae_cache_mshr_class_init(ObjectClass *klass, const void *data)
{
    UserCreatableClass *uc = USER_CREATABLE_CLASS(klass);
    CaeMemClass *mc = CAE_MEM_CLASS(klass);

    (void)data;
    uc->complete = cae_cache_mshr_complete;
    mc->access = cae_cache_mshr_access;
    mc->access_async = cae_cache_mshr_access_async;
    mc->can_accept = cae_cache_mshr_can_accept;
}

static const TypeInfo cae_cache_mshr_type = {
    .name = TYPE_CAE_CACHE_MSHR,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeCacheMshr),
    .instance_init = cae_cache_mshr_instance_init,
    .class_init = cae_cache_mshr_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { TYPE_CAE_MEM },
        { }
    }
};

static void cae_cache_mshr_register_types(void)
{
    type_register_static(&cae_cache_mshr_type);
}

type_init(cae_cache_mshr_register_types)

/* ------------------------------------------------------------------ */
/*  Speculation save/restore: outstanding-miss ring + n_entries       */
/* ------------------------------------------------------------------ */

/*
 * Snapshot carries the live portion of the completion ring
 * (first n_entries slots) plus n_entries itself. Restore
 * overwrites both; a wrong-path speculation that issued MSHRs
 * must be able to erase them on squash so the post-restore
 * expiry math keys off the correct outstanding set.
 *
 * The ring is fixed-size (CAE_MSHR_MAX_ENTRIES) so we copy all
 * slots for determinism — slot contents past n_entries are
 * don't-care at expiry time but a byte-identical restore of the
 * full ring avoids any surprise if a future expiry bug reads
 * past n_entries.
 */
struct CaeMshrSpecSnapshot {
    uint32_t n_entries;
    uint64_t completion_cycles[CAE_MSHR_MAX_ENTRIES];
};

CaeMshrSpecSnapshot *cae_mshr_spec_snapshot_save(Object *obj)
{
    if (!obj) {
        return NULL;
    }
    CaeCacheMshr *m =
        (CaeCacheMshr *)object_dynamic_cast(obj, TYPE_CAE_CACHE_MSHR);
    if (!m) {
        return NULL;
    }
    CaeMshrSpecSnapshot *snap = g_new0(CaeMshrSpecSnapshot, 1);
    snap->n_entries = m->n_entries;
    memcpy(snap->completion_cycles, m->completion_cycles,
           sizeof(m->completion_cycles));
    return snap;
}

void cae_mshr_spec_snapshot_restore(Object *obj,
                                    const CaeMshrSpecSnapshot *snap)
{
    if (!obj || !snap) {
        return;
    }
    CaeCacheMshr *m =
        (CaeCacheMshr *)object_dynamic_cast(obj, TYPE_CAE_CACHE_MSHR);
    if (!m) {
        return;
    }
    m->n_entries = snap->n_entries;
    memcpy(m->completion_cycles, snap->completion_cycles,
           sizeof(m->completion_cycles));
}

void cae_mshr_spec_snapshot_drop(CaeMshrSpecSnapshot *snap)
{
    g_free(snap);
}
