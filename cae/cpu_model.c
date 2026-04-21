/*
 * CAE CPU timing model interface base.
 *
 * Registers TYPE_CAE_CPU_MODEL and provides the safe dispatcher used by
 * the engine on the hot path.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qom/object.h"
#include "cae/cpu_model.h"

uint32_t cae_cpu_model_charge(Object *obj, const CaeCpu *cpu,
                              const CaeUop *uop)
{
    CaeCpuModelClass *cc;

    if (!obj) {
        return 1;
    }
    cc = CAE_CPU_MODEL_CLASS(object_get_class(obj));
    if (!cc || !cc->charge) {
        return 1;
    }
    return cc->charge(obj, cpu, uop);
}

static const TypeInfo cae_cpu_model_interface_type = {
    .name = TYPE_CAE_CPU_MODEL,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(CaeCpuModelClass),
};

static void cae_cpu_model_register_types(void)
{
    type_register_static(&cae_cpu_model_interface_type);
}

type_init(cae_cpu_model_register_types)
