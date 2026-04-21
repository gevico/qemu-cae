/*
 * CAE (Cycle Approximate Engine) - AccelOps Implementation
 *
 * Provides vCPU thread management and virtual clock interface.
 * RR (round-robin) single-thread mode only in phase-1.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "system/tcg.h"
#include "system/cpu-timers.h"
#include "system/cpus.h"
#include "accel/accel-ops.h"
#include "accel/accel-cpu-ops.h"
#include "qemu/main-loop.h"
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "exec/cputlb.h"
#include "exec/translation-block.h"
#include "exec/tb-flush.h"
#include "hw/core/cpu.h"
#include "cae/engine.h"

#include "cae-accel-ops.h"

/*
 * Virtual clock backed by the CAE engine cycle counter.
 * Converts cycles to nanoseconds using the engine's base frequency.
 *
 * The fallback path uses cpu_get_clock() rather than
 * qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL): the latter dispatches through
 * cpus_get_virtual_clock(), which calls cpus_accel->get_virtual_clock,
 * i.e. right back into this function. Any query that races with engine
 * teardown or runs before cae_init_machine finishes would recurse
 * without bound. cpu_get_clock() reads timers_state directly and never
 * re-enters the accelerator ops.
 */
static int64_t cae_get_virtual_clock(void)
{
    CaeEngine *engine = cae_get_engine();
    if (!engine || engine->base_freq_hz == 0) {
        return cpu_get_clock();
    }
    /*
     * current_cycle is mutated on the vCPU thread; on 32-bit hosts a
     * plain read from the I/O thread can tear. Use the atomic helper
     * to match the writers in cae/engine.c.
     */
    return cae_cycles_to_ns(qatomic_read(&engine->current_cycle),
                            engine->base_freq_hz);
}

static int64_t cae_get_elapsed_ticks(void)
{
    return cae_get_virtual_clock();
}

static void cae_handle_interrupt(CPUState *cpu, int mask)
{
    g_assert(bql_locked());
    cpu->interrupt_request |= mask;
    if (!qemu_cpu_is_self(cpu)) {
        qemu_cpu_kick(cpu);
    } else {
        qatomic_set(&cpu->neg.icount_decr.u16.high, -1);
    }
}

static void cae_set_virtual_clock(int64_t time)
{
    CaeEngine *engine = cae_get_engine();
    if (engine && engine->base_freq_hz > 0) {
        /* Convert ns to cycles: ns * freq / 1e9 (overflow-safe) */
        uint64_t sec = (uint64_t)(time / 1000000000LL);
        uint64_t rem_ns = (uint64_t)(time % 1000000000LL);
        uint64_t new_cycle = sec * engine->base_freq_hz
            + rem_ns * engine->base_freq_hz / 1000000000ULL;
        qatomic_set(&engine->current_cycle, new_cycle);
    }
}

static void cae_cpu_reset_hold(CPUState *cpu)
{
    tcg_flush_jmp_cache(cpu);
    tlb_flush(cpu);
}

static void cae_accel_ops_init(AccelClass *ac)
{
    AccelOpsClass *ops = ac->ops;

    /* RR mode only: delegate to TCG RR thread management */
    ops->create_vcpu_thread = cae_start_vcpu_thread;
    ops->kick_vcpu_thread = cae_kick_vcpu_thread;
    ops->handle_interrupt = cae_handle_interrupt;
    ops->cpu_reset_hold = cae_cpu_reset_hold;
    ops->get_virtual_clock = cae_get_virtual_clock;
    ops->set_virtual_clock = cae_set_virtual_clock;
    ops->get_elapsed_ticks = cae_get_elapsed_ticks;

    /* No guest debug support in phase-1 */
    ops->supports_guest_debug = NULL;
}

static void cae_accel_ops_class_init(ObjectClass *oc, const void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);
    ops->ops_init = cae_accel_ops_init;
}

static const TypeInfo cae_accel_ops_type = {
    .name = ACCEL_OPS_NAME("cae"),
    .parent = TYPE_ACCEL_OPS,
    .class_init = cae_accel_ops_class_init,
    .abstract = true,
};
module_obj(ACCEL_OPS_NAME("cae"));

static void cae_accel_ops_register_types(void)
{
    type_register_static(&cae_accel_ops_type);
}
type_init(cae_accel_ops_register_types);
