/*
 * CAE (Cycle Approximate Engine) - Fixed-latency DRAM Timing Backend
 *
 * Terminal CaeMemClass implementer. Reports per-operation latency
 * measured in engine cycles; carries no backing store (QEMU's
 * MemoryRegion RAM continues to serve the functional read/write
 * path - this object only accounts for time).
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "cae/mem.h"

#define TYPE_CAE_DRAM "cae-dram"

OBJECT_DECLARE_SIMPLE_TYPE(CaeDram, CAE_DRAM)

struct CaeDram {
    Object parent;

    /* Per-operation latency in engine cycles. A 1 GHz engine (CAE default)
     * makes cycles == ns for users that think in nanoseconds. */
    uint64_t read_latency_cycles;
    uint64_t write_latency_cycles;
    /* When zero, fetch follows read (I-side traffic modelled as reads
     * unless the user wants to distinguish). */
    uint64_t fetch_latency_cycles;

    /* Read-only stats. Incremented from the vCPU thread via access();
     * sampled via QMP qom-get from the I/O thread, so updates go
     * through qatomic_set to keep 32-bit hosts coherent. */
    uint64_t accesses;
};

static uint64_t cae_dram_latency_for(const CaeDram *dram, CaeMemOp op)
{
    switch (op) {
    case CAE_MEM_WRITE:
        return dram->write_latency_cycles;
    case CAE_MEM_FETCH:
        return dram->fetch_latency_cycles ? dram->fetch_latency_cycles
                                          : dram->read_latency_cycles;
    case CAE_MEM_READ:
    default:
        return dram->read_latency_cycles;
    }
}

static CaeMemResp cae_dram_access(Object *dev, CaeMemReq *req)
{
    CaeDram *dram = CAE_DRAM(dev);
    uint64_t latency = cae_dram_latency_for(dram, req->op);

    qatomic_set(&dram->accesses,
                    qatomic_read(&dram->accesses) + 1);

    return (CaeMemResp){
        .latency = latency,
        .result = CAE_MEM_HIT,
        .opaque = NULL,
        .completion_cycle = req->now_cycle + latency,
    };
}

static bool cae_dram_access_async(Object *dev, CaeMemReq *req,
                                  CaeMemRespCb cb, void *cb_opaque)
{
    CaeMemResp resp = cae_dram_access(dev, req);
    if (cb) {
        cb(&resp, cb_opaque);
    }
    return true;
}

static bool cae_dram_can_accept(Object *dev)
{
    return true;
}

static void cae_dram_get_accesses(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    CaeDram *dram = CAE_DRAM(obj);
    uint64_t value = qatomic_read(&dram->accesses);
    visit_type_uint64(v, name, &value, errp);
}

static void cae_dram_complete(UserCreatable *uc, Error **errp)
{
    CaeDram *dram = CAE_DRAM(uc);

    if (dram->read_latency_cycles == 0 && dram->write_latency_cycles == 0
            && dram->fetch_latency_cycles == 0) {
        error_setg(errp, "cae-dram: at least one of read-latency-cycles / "
                         "write-latency-cycles / fetch-latency-cycles "
                         "must be non-zero");
        return;
    }
}

static void cae_dram_instance_init(Object *obj)
{
    CaeDram *dram = CAE_DRAM(obj);

    /* 50-cycle default matches the Phase-2 plan's DRAM baseline
     * (dram_latency_ns: 50 at 1 GHz engine = 50 cycles). */
    dram->read_latency_cycles = 50;
    dram->write_latency_cycles = 50;
    dram->fetch_latency_cycles = 0;  /* 0 => follow read */
    dram->accesses = 0;

    object_property_add_uint64_ptr(obj, "read-latency-cycles",
                                   &dram->read_latency_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "write-latency-cycles",
                                   &dram->write_latency_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "fetch-latency-cycles",
                                   &dram->fetch_latency_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "accesses", "uint64",
                        cae_dram_get_accesses,
                        NULL, NULL, NULL);
}

static void cae_dram_class_init(ObjectClass *oc, const void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);
    CaeMemClass *mc = CAE_MEM_CLASS(
        object_class_dynamic_cast(oc, TYPE_CAE_MEM));

    ucc->complete = cae_dram_complete;

    if (mc) {
        mc->access = cae_dram_access;
        mc->access_async = cae_dram_access_async;
        mc->can_accept = cae_dram_can_accept;
    }
}

static const TypeInfo cae_dram_type = {
    .name = TYPE_CAE_DRAM,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeDram),
    .instance_init = cae_dram_instance_init,
    .class_init = cae_dram_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_CAE_MEM },
        { TYPE_USER_CREATABLE },
        { },
    },
};

static void cae_dram_register_types(void)
{
    type_register_static(&cae_dram_type);
}
type_init(cae_dram_register_types);
