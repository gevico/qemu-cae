/*
 * CAE (Cycle Approximate Engine) - Memory Interface Registration
 *
 * Registers the CaeMemClass QOM interface type and provides a stub
 * implementer for testing and default behavior.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qom/object.h"
#include "cae/mem.h"

/* CaeMemClass interface type */
static const TypeInfo cae_mem_type = {
    .name = TYPE_CAE_MEM,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(CaeMemClass),
};

/*
 * Stub CaeMemClass implementer.
 * Returns zero latency for all accesses (flat memory model).
 */
#define TYPE_CAE_MEM_STUB "cae-mem-stub"

typedef struct CaeMemStub {
    Object parent;
} CaeMemStub;

static CaeMemResp cae_mem_stub_access(Object *dev, CaeMemReq *req)
{
    return (CaeMemResp){
        .latency = 0,
        .result = CAE_MEM_HIT,
        .opaque = NULL,
        /*
         * Round 19 t-mem-async-iface: stub is instantaneous,
         * so data is available the same cycle it was
         * requested.
         */
        .completion_cycle = req->now_cycle,
    };
}

static bool cae_mem_stub_access_async(Object *dev, CaeMemReq *req,
                                      CaeMemRespCb cb, void *cb_opaque)
{
    CaeMemResp resp = cae_mem_stub_access(dev, req);
    if (cb) {
        cb(&resp, cb_opaque);
    }
    return true;
}

static bool cae_mem_stub_can_accept(Object *dev)
{
    return true;
}

static void cae_mem_stub_class_init(ObjectClass *oc, const void *data)
{
    CaeMemClass *mc = CAE_MEM_CLASS(
        object_class_dynamic_cast(oc, TYPE_CAE_MEM));
    if (mc) {
        mc->access = cae_mem_stub_access;
        mc->access_async = cae_mem_stub_access_async;
        mc->can_accept = cae_mem_stub_can_accept;
    }
}

static const TypeInfo cae_mem_stub_type = {
    .name = TYPE_CAE_MEM_STUB,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeMemStub),
    .class_init = cae_mem_stub_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_CAE_MEM },
        { },
    },
};

static void cae_mem_register_types(void)
{
    type_register_static(&cae_mem_type);
    type_register_static(&cae_mem_stub_type);
}
type_init(cae_mem_register_types);
