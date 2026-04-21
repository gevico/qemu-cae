/*
 * CAE (Cycle Approximate Engine) - Memory Access QOM Interface
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_MEM_H
#define CAE_MEM_H

#include <stdbool.h>
#include <stdint.h>
#include "qom/object.h"

#define TYPE_CAE_MEM "cae-mem"

typedef struct CaeMemClass CaeMemClass;
DECLARE_CLASS_CHECKERS(CaeMemClass, CAE_MEM, TYPE_CAE_MEM)

typedef enum CaeMemOp {
    CAE_MEM_READ,
    CAE_MEM_WRITE,
    CAE_MEM_FETCH,
} CaeMemOp;

typedef enum CaeMemResult {
    CAE_MEM_HIT,
    CAE_MEM_MISS,
    CAE_MEM_ERROR,
} CaeMemResult;

typedef struct CaeMemReq {
    uint64_t addr;
    uint32_t size;
    CaeMemOp op;
    uint32_t src_id;
    void *opaque;
    /*
     * Round 19 t-mem-async-iface: absolute engine cycle at
     * which this access is dispatched. Caller fills this so
     * asynchronous backends (cache_mshr) can track
     * outstanding completions against the global clock. A
     * value of 0 is accepted by legacy callers that never
     * set it; the MSHR then falls back to the engine's
     * current cycle on lookup.
     */
    uint64_t now_cycle;
    /*
     * Round 34 AC-K-4: set to true by cae_mem_access_notify
     * when the issuing CPU is inside a live speculation window
     * (`cpu->spec_snap_valid == true` — a checkpoint snapshot is
     * in flight between HELPER(lookup_tb_ptr) save and the
     * matching retire-side resolve). Downstream cache / MSHR
     * layers MUST NOT install the missed line into any cache
     * array when this flag is set, per plan.md:87 "Speculative
     * loads must not refill L1 data array". The miss still
     * charges full latency (downstream DRAM / NoC is exercised
     * and the CPU consumes the value during speculative
     * execution). The squash / restore path unwinds CPU-side
     * speculative state separately via the live_restore chain.
     * Default false preserves legacy callers that pre-date
     * this field.
     */
    bool speculative;
} CaeMemReq;

typedef struct CaeMemResp {
    uint64_t latency;
    CaeMemResult result;
    void *opaque;
    /*
     * Round 19 t-mem-async-iface: absolute engine cycle at
     * which this access's data becomes available. Backends
     * fill it as `req->now_cycle + returned_latency`; LSQ /
     * engine code can observe outstanding completions by
     * comparing against the current global cycle instead of
     * relying on a cache-internal monotonic counter.
     */
    uint64_t completion_cycle;
} CaeMemResp;

typedef void (*CaeMemRespCb)(CaeMemResp *resp, void *opaque);

struct CaeMemClass {
    InterfaceClass parent;

    CaeMemResp (*access)(Object *dev, CaeMemReq *req);
    bool (*access_async)(Object *dev, CaeMemReq *req,
                         CaeMemRespCb cb, void *cb_opaque);
    bool (*can_accept)(Object *dev);
};

#endif /* CAE_MEM_H */
