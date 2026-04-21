/*
 * CAE cache + MSHR wrapper (M3' scaffold).
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_CACHE_MSHR_H
#define CAE_CACHE_MSHR_H

#include <stdint.h>
#include <stdbool.h>
#include "qom/object.h"

/* QOM type name. */
#define TYPE_CAE_CACHE_MSHR "cae-cache-mshr"

#define CAE_MSHR_DEFAULT_SIZE     8u
#define CAE_MSHR_DEFAULT_FILL_Q   16u
#define CAE_MSHR_DEFAULT_WB_Q     16u

/*
 * Round 19 drive-by fix: icache geometry fallback helper. When any
 * of the per-I-cache knobs is left at 0, the accel layer substitutes
 * the equivalent L1D knob so the user-facing `icache-size=0 -> fall
 * back to L1D` contract actually holds instead of bottoming out on
 * cae-cache's "size must be a power of 2" check. Exposed via this
 * header so both accel/cae/cae-all.c and the regression test can
 * observe identical semantics.
 */
static inline uint64_t cae_icache_effective_size(uint64_t configured,
                                                 uint64_t fallback)
{
    return configured ? configured : fallback;
}

static inline uint32_t cae_icache_effective_u32(uint32_t configured,
                                                uint32_t fallback)
{
    return configured ? configured : fallback;
}

/* ------------------------------------------------------------------ */
/*  Speculation save/restore: MSHR outstanding ring                   */
/* ------------------------------------------------------------------ */

/*
 * Opaque snapshot of the MSHR's outstanding-miss lane — covers the
 * live `completion_cycles[0..n_entries-1]` ring and the
 * `n_entries` count. Used by the M4' speculation rollback path
 * alongside the CAE-side and RV architectural lanes. Save returns
 * NULL on NULL owner or on a non-mshr Object *. Restore is NULL-
 * safe on both owner and snapshot.
 */
typedef struct CaeMshrSpecSnapshot CaeMshrSpecSnapshot;

CaeMshrSpecSnapshot *cae_mshr_spec_snapshot_save(Object *obj);
void cae_mshr_spec_snapshot_restore(Object *obj,
                                    const CaeMshrSpecSnapshot *snap);
void cae_mshr_spec_snapshot_drop(CaeMshrSpecSnapshot *snap);

#endif /* CAE_CACHE_MSHR_H */
