/*
 * CAE (Cycle Approximate Engine) - Parameterised Cache Timing Backend
 *
 * CaeMemClass implementer that sits between the softmmu hook path and
 * a downstream CaeMemClass backend (typically cae-dram). Hit/miss
 * decisions are driven by address alone; no line payload is kept.
 * Deterministic LRU per set with invalid-first fill keeps two
 * identical access streams bit-for-bit reproducible (AC-11).
 *
 * Scope for this M2 subset:
 *   - synchronous, blocking access() only
 *   - no MSHR (M3), no dirty tracking, no writeback traffic
 *   - write and read share the same lookup path; misses forward to
 *     downstream, hits pay only local latency
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "cae/mem.h"

#define TYPE_CAE_CACHE "cae-cache"

OBJECT_DECLARE_SIMPLE_TYPE(CaeCache, CAE_CACHE)

struct CaeCache {
    Object parent;

    /* User-tunable parameters (set before complete(), frozen after). */
    uint64_t size_bytes;
    uint32_t assoc;
    uint32_t line_size;
    uint32_t latency_hit_cycles;
    uint32_t latency_miss_cycles;

    /* Typed strong QOM link to the downstream CaeMemClass implementer.
     * NULL before complete(). */
    Object *downstream;
    CaeMemClass *downstream_mc;  /* cached at complete() time */

    /* Derived at complete() time, read-only thereafter. */
    uint32_t num_sets;
    uint32_t line_shift;
    uint32_t set_mask;
    uint32_t tag_shift;

    /* Storage: tags/valid/lru laid out as [num_sets * assoc] flat arrays.
     * tag[set * assoc + way] is the tag stored in that way (valid==0
     * lines carry stale tag data and must not be probed). lru_order
     * encodes MRU-to-LRU way indices: lru[set*assoc + 0] is MRU,
     * lru[set*assoc + assoc-1] is LRU. */
    uint64_t *tags;
    uint8_t *valid;
    uint16_t *lru_order;

    /* Read-only stats (qatomic'd for concurrent QMP readers). */
    uint64_t accesses;
    uint64_t hits;
    uint64_t misses;
    /*
     * Round 34 AC-K-4: counts non-speculative miss-then-install
     * operations. A speculative miss increments `misses` (the
     * access still happens and is charged latency) but NOT
     * `fills` — per plan.md:87, a speculative load must not
     * refill the L1 data array. The plan's negative test
     * (plan.md:96) reads this stat before and after a known-
     * mispredicted load and asserts no increment.
     */
    uint64_t fills;

    bool configured;
};

/* ------------------------------------------------------------------ */
/*  Helpers (no allocations on hot path)                              */
/* ------------------------------------------------------------------ */

static bool is_power_of_two(uint64_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

static inline uint64_t cae_cache_tag_of(const CaeCache *c, uint64_t addr)
{
    return addr >> c->tag_shift;
}

static inline uint32_t cae_cache_set_of(const CaeCache *c, uint64_t addr)
{
    return (uint32_t)((addr >> c->line_shift) & c->set_mask);
}

static uint16_t *cae_cache_lru_set(const CaeCache *c, uint32_t set)
{
    return c->lru_order + (size_t)set * c->assoc;
}

/* Promote `way` to MRU position within its set's LRU order. The way
 * must already be present in lru_order[set][0..assoc-1]; on a fresh
 * fill the caller inserts the newly-occupied way first, then calls
 * this to update the order. */
static void cae_cache_lru_promote(const CaeCache *c, uint32_t set,
                                  uint16_t way)
{
    uint16_t *order = cae_cache_lru_set(c, set);
    uint32_t pos;

    for (pos = 0; pos < c->assoc; pos++) {
        if (order[pos] == way) {
            break;
        }
    }
    /* Already MRU or absent (shouldn't happen post-complete) - nothing
     * to do in the first case; in the second we'd corrupt state, so
     * bail. */
    if (pos == 0 || pos >= c->assoc) {
        return;
    }

    /* Shift order[0..pos-1] right by one, then drop `way` at the head. */
    for (uint32_t i = pos; i > 0; i--) {
        order[i] = order[i - 1];
    }
    order[0] = way;
}

/* Return the way to fill. Prefer the lowest-numbered invalid way for
 * deterministic cold-fill behaviour; fall back to the LRU way when the
 * set is fully populated. The chosen way is NOT yet moved to MRU -
 * callers call cae_cache_lru_promote() after the fill. */
static uint16_t cae_cache_pick_victim(CaeCache *c, uint32_t set)
{
    uint8_t *valid_row = c->valid + (size_t)set * c->assoc;
    uint16_t *order = cae_cache_lru_set(c, set);

    for (uint32_t w = 0; w < c->assoc; w++) {
        if (!valid_row[w]) {
            return (uint16_t)w;
        }
    }
    return order[c->assoc - 1];
}

/* ------------------------------------------------------------------ */
/*  Link property: always accept, defer type check to complete()      */
/* ------------------------------------------------------------------ */

/* QOM makes a link property read-only when its check callback is NULL
 * (the setter vtable entry is wired up only when check != NULL). So we
 * install a trivial always-accept check here; complete() does the real
 * TYPE_CAE_MEM + presence validation. */
static void cae_cache_check_downstream(const Object *obj, const char *name,
                                       Object *val, Error **errp)
{
}

/* ------------------------------------------------------------------ */
/*  CaeMemClass vtable                                                */
/* ------------------------------------------------------------------ */

static CaeMemResp cae_cache_access(Object *dev, CaeMemReq *req)
{
    CaeCache *c = CAE_CACHE(dev);
    uint64_t tag = cae_cache_tag_of(c, req->addr);
    uint32_t set = cae_cache_set_of(c, req->addr);
    uint64_t *tag_row = c->tags + (size_t)set * c->assoc;
    uint8_t *valid_row = c->valid + (size_t)set * c->assoc;
    uint32_t w;
    bool hit = false;

    qatomic_set(&c->accesses,
                    qatomic_read(&c->accesses) + 1);

    for (w = 0; w < c->assoc; w++) {
        if (valid_row[w] && tag_row[w] == tag) {
            hit = true;
            break;
        }
    }

    if (hit) {
        qatomic_set(&c->hits,
                        qatomic_read(&c->hits) + 1);
        /*
         * Round 35 AC-K-4 Option-X completion (plan.md:85-87):
         * speculative hits must NOT promote the LRU position of
         * the accessed way. The data + hit-latency are returned
         * (the speculating CPU observes them), but replacement
         * state stays unchanged so a wrong-path load leaves no
         * trace in the next eviction's victim choice.
         * LRU/replacement metadata is architecturally observable
         * through later eviction choice; round 34 gated the
         * miss-path install but left this hit-path mutation as
         * a contract leak that Codex's round-34 review flagged.
         */
        if (!req->speculative) {
            cae_cache_lru_promote(c, set, (uint16_t)w);
        }
        return (CaeMemResp){
            .latency = c->latency_hit_cycles,
            .result = CAE_MEM_HIT,
            .opaque = NULL,
            .completion_cycle = req->now_cycle
                                + c->latency_hit_cycles,
        };
    }

    /*
     * Miss: pay local miss penalty + downstream latency. Fill the
     * line into the cache ONLY when the access is not known to be
     * on a speculative path. Round 34 AC-K-4: a speculative
     * miss still charges latency but MUST NOT install the line
     * (per plan.md:87 "speculative loads must not refill L1 data
     * array"). The miss counter always advances; the `fills`
     * counter advances only when a line is actually installed.
     */
    qatomic_set(&c->misses,
                    qatomic_read(&c->misses) + 1);

    if (!req->speculative) {
        uint16_t victim = cae_cache_pick_victim(c, set);
        tag_row[victim] = tag;
        valid_row[victim] = 1;
        cae_cache_lru_promote(c, set, victim);
        qatomic_set(&c->fills,
                        qatomic_read(&c->fills) + 1);
    }

    CaeMemResp down_resp = { .latency = 0, .result = CAE_MEM_HIT };
    if (c->downstream_mc && c->downstream_mc->access) {
        down_resp = c->downstream_mc->access(c->downstream, req);
    }

    return (CaeMemResp){
        .latency = (uint64_t)c->latency_miss_cycles + down_resp.latency,
        .result = CAE_MEM_MISS,
        .opaque = NULL,
        .completion_cycle = req->now_cycle
                            + (uint64_t)c->latency_miss_cycles
                            + down_resp.latency,
    };
}

static bool cae_cache_access_async(Object *dev, CaeMemReq *req,
                                   CaeMemRespCb cb, void *cb_opaque)
{
    CaeMemResp resp = cae_cache_access(dev, req);
    if (cb) {
        cb(&resp, cb_opaque);
    }
    return true;
}

static bool cae_cache_can_accept(Object *dev)
{
    return true;
}

/* ------------------------------------------------------------------ */
/*  Read-only stats getters (atomic to survive concurrent QMP reads)  */
/* ------------------------------------------------------------------ */

#define DEFINE_CACHE_STAT_GETTER(fn, member)                              \
    static void fn(Object *obj, Visitor *v, const char *name,             \
                   void *opaque, Error **errp)                            \
    {                                                                     \
        CaeCache *c = CAE_CACHE(obj);                                     \
        uint64_t value = qatomic_read(&c->member);                    \
        visit_type_uint64(v, name, &value, errp);                         \
    }

DEFINE_CACHE_STAT_GETTER(cae_cache_get_accesses, accesses)
DEFINE_CACHE_STAT_GETTER(cae_cache_get_hits, hits)
DEFINE_CACHE_STAT_GETTER(cae_cache_get_misses, misses)
DEFINE_CACHE_STAT_GETTER(cae_cache_get_fills, fills)

/* ------------------------------------------------------------------ */
/*  UserCreatable.complete(): geometry + downstream validation        */
/* ------------------------------------------------------------------ */

static void cae_cache_complete(UserCreatable *uc, Error **errp)
{
    CaeCache *c = CAE_CACHE(uc);
    uint64_t entries;

    if (c->configured) {
        /* Object-add is a single-shot operation; guard against
         * re-entry just in case user_creatable_complete() ever fires
         * twice on the same object (e.g. migration-load someday). */
        return;
    }

    if (!is_power_of_two(c->line_size)) {
        error_setg(errp, "cae-cache: line-size (%u) must be a power of 2",
                   c->line_size);
        return;
    }
    if (!is_power_of_two(c->size_bytes)) {
        error_setg(errp, "cae-cache: size (%" PRIu64 ") must be a power of 2",
                   c->size_bytes);
        return;
    }
    if (c->assoc == 0) {
        error_setg(errp, "cae-cache: assoc must be non-zero");
        return;
    }
    if (c->size_bytes < (uint64_t)c->assoc * c->line_size) {
        error_setg(errp,
                   "cae-cache: size (%" PRIu64 ") < assoc*line-size (%u)",
                   c->size_bytes, c->assoc * c->line_size);
        return;
    }

    entries = c->size_bytes / c->line_size;
    if (entries % c->assoc != 0) {
        error_setg(errp, "cae-cache: size/line-size (%" PRIu64 ") is not a "
                         "multiple of assoc (%u)",
                   entries, c->assoc);
        return;
    }
    c->num_sets = (uint32_t)(entries / c->assoc);
    if (!is_power_of_two(c->num_sets)) {
        error_setg(errp,
                   "cae-cache: derived num-sets (%u) must be a power of 2; "
                   "pick a size/assoc/line-size combination whose quotient "
                   "is a power of 2",
                   c->num_sets);
        return;
    }

    c->line_shift = ctz32(c->line_size);
    c->set_mask = c->num_sets - 1;
    c->tag_shift = c->line_shift + ctz32(c->num_sets);

    if (c->assoc > UINT16_MAX) {
        error_setg(errp, "cae-cache: assoc (%u) exceeds %u-way ceiling",
                   c->assoc, UINT16_MAX);
        return;
    }

    if (!c->downstream) {
        error_setg(errp, "cae-cache: downstream link is unset; point it at "
                         "a cae-mem implementer (e.g. another cae-cache or "
                         "a cae-dram)");
        return;
    }

    c->downstream_mc = CAE_MEM_CLASS(
        object_class_dynamic_cast(object_get_class(c->downstream),
                                  TYPE_CAE_MEM));
    if (!c->downstream_mc || !c->downstream_mc->access) {
        error_setg(errp, "cae-cache: downstream does not implement "
                         "TYPE_CAE_MEM::access");
        return;
    }

    /* Single allocation per field keeps the hot path allocation-free. */
    size_t n = (size_t)c->num_sets * c->assoc;
    c->tags = g_new0(uint64_t, n);
    c->valid = g_new0(uint8_t, n);
    c->lru_order = g_new0(uint16_t, n);

    /* Seed LRU order so miss fills are deterministic on a cold cache:
     * the invalid-first victim picker returns way 0, 1, ... in order,
     * and only once all ways are valid does the tail of lru_order
     * become the eviction target. */
    for (uint32_t s = 0; s < c->num_sets; s++) {
        uint16_t *order = cae_cache_lru_set(c, s);
        for (uint32_t w = 0; w < c->assoc; w++) {
            order[w] = (uint16_t)w;
        }
    }

    c->configured = true;
}

/* ------------------------------------------------------------------ */
/*  QOM type wiring                                                   */
/* ------------------------------------------------------------------ */

static void cae_cache_instance_init(Object *obj)
{
    CaeCache *c = CAE_CACHE(obj);

    /* Sensible defaults for a small L1: 32 KB, 2-way, 64 B line,
     * 1-cycle hit, 10-cycle local miss penalty. Users override via
     * -object cae-cache,... on the command line. */
    c->size_bytes = 32 * 1024;
    c->assoc = 2;
    c->line_size = 64;
    c->latency_hit_cycles = 1;
    c->latency_miss_cycles = 10;
    c->downstream = NULL;
    c->downstream_mc = NULL;
    c->tags = NULL;
    c->valid = NULL;
    c->lru_order = NULL;
    c->accesses = 0;
    c->hits = 0;
    c->misses = 0;
    c->configured = false;

    object_property_add_uint64_ptr(obj, "size",
                                   &c->size_bytes,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "assoc",
                                   &c->assoc,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "line-size",
                                   &c->line_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-hit-cycles",
                                   &c->latency_hit_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-miss-cycles",
                                   &c->latency_miss_cycles,
                                   OBJ_PROP_FLAG_READWRITE);

    /* Strong typed link to the downstream CaeMemClass implementer.
     * QOM supports link<INTERFACE> (TYPE_STREAM_SINK uses the same
     * pattern). The link is populated via -object cae-cache,...,
     * downstream=<id> on the CLI or object-add over QMP. */
    /* The link check permits any set; complete() validates presence
     * and dynamic_cast. A non-NULL check is required for the link
     * property to be writable - QOM wires the setter only when a
     * check callback is present. */
    object_property_add_link(obj, "downstream", TYPE_CAE_MEM,
                             &c->downstream,
                             cae_cache_check_downstream,
                             OBJ_PROP_LINK_STRONG);

    object_property_add(obj, "accesses", "uint64",
                        cae_cache_get_accesses, NULL, NULL, NULL);
    object_property_add(obj, "hits", "uint64",
                        cae_cache_get_hits, NULL, NULL, NULL);
    object_property_add(obj, "misses", "uint64",
                        cae_cache_get_misses, NULL, NULL, NULL);
    object_property_add(obj, "fills", "uint64",
                        cae_cache_get_fills, NULL, NULL, NULL);
}

static void cae_cache_instance_finalize(Object *obj)
{
    CaeCache *c = CAE_CACHE(obj);

    g_free(c->tags);
    g_free(c->valid);
    g_free(c->lru_order);
}

static void cae_cache_class_init(ObjectClass *oc, const void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);
    CaeMemClass *mc = CAE_MEM_CLASS(
        object_class_dynamic_cast(oc, TYPE_CAE_MEM));

    ucc->complete = cae_cache_complete;

    if (mc) {
        mc->access = cae_cache_access;
        mc->access_async = cae_cache_access_async;
        mc->can_accept = cae_cache_can_accept;
    }
}

static const TypeInfo cae_cache_type = {
    .name = TYPE_CAE_CACHE,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeCache),
    .instance_init = cae_cache_instance_init,
    .instance_finalize = cae_cache_instance_finalize,
    .class_init = cae_cache_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_CAE_MEM },
        { TYPE_USER_CREATABLE },
        { },
    },
};

static void cae_cache_register_types(void)
{
    type_register_static(&cae_cache_type);
}
type_init(cae_cache_register_types);
