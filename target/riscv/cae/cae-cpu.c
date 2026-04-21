/*
 * RISC-V CAE AccelCPUClass implementation
 *
 * Adapts the RISC-V CPU for CAE acceleration. Reuses TCG's
 * functional execution infrastructure (trans_xxx, helpers) and
 * adds CAE timing initialization.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "exec/translation-block.h"
#include "cpu.h"
#include "internals.h"
#include "pmu.h"
#include "time_helper.h"
#include "qemu/accel.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "accel/accel-cpu-target.h"
#include "accel/tcg/cpu-ops.h"
#include "tcg/tcg-cpu.h"
#include "cae/engine.h"
#include "cae/pipeline.h"
#include "cae/uop.h"
#ifndef CONFIG_USER_ONLY
#include "hw/core/boards.h"
#endif

/*
 * CAE reuses the same TCGCPUOps as TCG — the functional
 * execution path is identical. The only difference is timing.
 * riscv_tcg_ops is declared in tcg/tcg-cpu.h (included above).
 */

/*
 * Called via AccelCPUClass->cpu_target_realize during CPU realization.
 * Path: riscv_cpu_realize() -> cpu_exec_realizefn() -> accel_cpu_common_realize()
 */
static bool riscv_cae_cpu_realize(CPUState *cs, Error **errp)
{
    RISCVCPU *cpu = RISCV_CPU(cs);

    if (!riscv_cpu_tcg_compatible(cpu)) {
        g_autofree char *name = riscv_cpu_get_name(cpu);
        error_setg(errp, "'%s' CPU is not compatible with CAE acceleration",
                   name);
        return false;
    }

#ifndef CONFIG_USER_ONLY
    tcg_cflags_set(cs, CF_PCREL);

    if (cpu->cfg.ext_sstc) {
        riscv_timer_init(cpu);
    }

    if (cpu->cfg.pmu_mask) {
        Error *local_err = NULL;
        riscv_pmu_init(cpu, &local_err);
        if (local_err != NULL) {
            error_propagate(errp, local_err);
            return false;
        }

        if (cpu->cfg.ext_sscofpmf) {
            cpu->pmu_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                          riscv_pmu_timer_cb, cpu);
        }
    }

    if (riscv_has_ext(&cpu->env, RVH)) {
        cpu->env.mideleg = MIP_VSSIP | MIP_VSTIP | MIP_VSEIP | MIP_SGEIP;
    }
#endif

    /* Create and register CAE timing CPU with the engine */
    {
        CaeEngine *engine = cae_get_engine();
        if (engine) {
            CaeCpu *cae_cpu = CAE_CPU(object_new(TYPE_CAE_CPU));
            cae_cpu_init(cae_cpu, engine, cs, cs->cpu_index);
            cae_engine_register_cpu(engine, cae_cpu);
        }
    }

    return true;
}

static void riscv_cae_cpu_instance_init(CPUState *cs)
{
    /* Shared init: extension tables, user properties, max extensions */
    riscv_cpu_accel_instance_init(cs);
}

static void riscv_cae_cpu_init_ops(AccelCPUClass *accel_cpu, CPUClass *cc)
{
    cc->tcg_ops = &riscv_tcg_ops;
}

static void riscv_cae_cpu_class_init(CPUClass *cc)
{
    cc->init_accel_cpu = riscv_cae_cpu_init_ops;
}

static void riscv_cae_cpu_accel_class_init(ObjectClass *oc, const void *data)
{
    AccelCPUClass *acc = ACCEL_CPU_CLASS(oc);

    acc->cpu_class_init = riscv_cae_cpu_class_init;
    acc->cpu_instance_init = riscv_cae_cpu_instance_init;
    acc->cpu_target_realize = riscv_cae_cpu_realize;
}

static const TypeInfo riscv_cae_cpu_accel_type_info = {
    .name = ACCEL_CPU_NAME("cae"),
    .parent = TYPE_ACCEL_CPU,
    .class_init = riscv_cae_cpu_accel_class_init,
    .abstract = true,
};

/*
 * AC-K-10 pre-exec hook. Before the FIRST cpu_exec slice retires
 * any TB, cae_cpu_exec calls this helper (via the arch-neutral
 * cae_cpu_prep_for_exec dispatcher registered at type_init). We
 * stamp active_uop->pc with the CPU's current architectural PC
 * so the first HELPER(lookup_tb_ptr) call charges the retired
 * TB against its real PC (typically 0x80000000 on RV virt
 * tier-1 binaries) instead of the zero left by cae_cpu_init.
 *
 * The guard is one-shot per CaeCpu lifetime: after the first
 * slice fills active_uop->pc, subsequent slices skip this hook
 * because active_uop persists across slices per
 * BL-20260418-active-uop-persist-across-slices, so pc is already
 * non-zero.
 */
static void riscv_cae_prep_for_exec(CaeCpu *cpu)
{
    if (!cpu || !cpu->qemu_cpu || !cpu->active_uop) {
        return;
    }
    if (cpu->active_uop->pc != 0) {
        return;
    }
    CPUState *cs = cpu->qemu_cpu;
    CPURISCVState *env = cpu_env(cs);
    cpu->active_uop->pc = env->pc;
}

static void riscv_cae_cpu_accel_register_types(void)
{
    type_register_static(&riscv_cae_cpu_accel_type_info);
    cae_accel_cpu_mark_registered();
    cae_cpu_register_prep_for_exec(riscv_cae_prep_for_exec);
}
type_init(riscv_cae_cpu_accel_register_types);
