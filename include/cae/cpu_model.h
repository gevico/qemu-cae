/*
 * CAE (Cycle Approximate Engine) - CPU timing model interface
 *
 * Plug-in point for per-instruction latency accounting. cae-engine's
 * cae_charge_executed_tb() defaults to CPI=1; installing a CaeCpuModel
 * lets an alternate latency policy (e.g. 5-stage in-order with branch
 * mispredict penalties) take over.
 *
 * The interface is arch-neutral. Concrete implementations live under
 * hw/cae/ (e.g. hw/cae/cpu_inorder.c).
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_CPU_MODEL_H
#define CAE_CPU_MODEL_H

#include "qom/object.h"
#include "cae/pipeline.h"
#include "cae/uop.h"

#define TYPE_CAE_CPU_MODEL "cae-cpu-model"

typedef struct CaeCpuModelClass CaeCpuModelClass;
DECLARE_CLASS_CHECKERS(CaeCpuModelClass, CAE_CPU_MODEL, TYPE_CAE_CPU_MODEL)

struct CaeCpuModelClass {
    InterfaceClass parent_class;

    /*
     * Compute the cycle cost of the retiring uop on the given CAE CPU.
     * Called from cae_charge_executed_tb() on the hot path — implementations
     * must be allocation-free and branch-light.
     *
     * When uop is NULL (e.g. chained TB whose classification wasn't set
     * ahead of time) implementations should return 1 (CPI=1 fallback) so
     * the engine's insn count still advances.
     */
    uint32_t (*charge)(Object *obj, const CaeCpu *cpu, const CaeUop *uop);
};

/*
 * Convenience dispatcher. Returns 1 if obj is NULL or the class does not
 * implement charge(), preserving Phase-1 CPI=1 behaviour.
 */
uint32_t cae_cpu_model_charge(Object *obj, const CaeCpu *cpu,
                              const CaeUop *uop);

#endif /* CAE_CPU_MODEL_H */
