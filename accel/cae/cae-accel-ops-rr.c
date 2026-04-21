/*
 * CAE (Cycle Approximate Engine) - RR Mode vCPU Thread
 *
 * Single-threaded round-robin vCPU execution with CAE timing.
 * Based on TCG RR mode but integrates cycle-approximate timing
 * into the execution loop.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/lockable.h"
#include "system/tcg.h"
#include "system/replay.h"
#include "system/cpu-timers.h"
#include "system/cpus.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/guest-random.h"
#include "exec/translation-block.h"
#include "tcg/startup.h"
#include "hw/core/cpu.h"
#include "system/runstate.h"
#include "cae/engine.h"
#include "cae/pipeline.h"
#include "cae/cae-mem-hook.h"
#include "cae/uop.h"

#include "cae-accel-ops.h"

/* From accel/tcg/ internal headers */
void tcg_cpu_init_cflags(CPUState *cpu, bool parallel);
#define CAE_KICK_PERIOD (NANOSECONDS_PER_SECOND / 10)

/* Kick all RR vCPUs */
void cae_kick_vcpu_thread(CPUState *unused)
{
    CPUState *cpu;
    CPU_FOREACH(cpu) {
        qatomic_set(&cpu->exit_request, true);
    }
}

static QEMUTimer *cae_rr_kick_timer;
static CPUState *cae_rr_current_cpu;

/*
 * Use QEMU_CLOCK_HOST (wall-clock) for RR preemption instead of
 * QEMU_CLOCK_VIRTUAL. The virtual clock only advances after cpu_exec()
 * returns, so a timer on QEMU_CLOCK_VIRTUAL cannot preempt a vCPU
 * that is busy inside cpu_exec(). Wall-clock-based preemption ensures
 * fair scheduling independent of virtual time progression.
 */
static inline int64_t cae_rr_next_kick_time(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_HOST) + CAE_KICK_PERIOD;
}

static void cae_rr_kick_next_cpu(void)
{
    CPUState *cpu;
    do {
        cpu = qatomic_read(&cae_rr_current_cpu);
        if (cpu) {
            cpu_exit(cpu);
        }
        smp_mb();
    } while (cpu != qatomic_read(&cae_rr_current_cpu));
}

static void cae_rr_kick_thread(void *opaque)
{
    timer_mod(cae_rr_kick_timer, cae_rr_next_kick_time());
    cae_rr_kick_next_cpu();
}

static void cae_rr_start_kick_timer(void)
{
    /*
     * Always create the kick timer, even for single-vCPU guests.
     * Unlike TCG RR which only needs preemption for multi-vCPU,
     * CAE needs periodic cpu_exec returns to advance current_cycle
     * and notify QEMU_CLOCK_VIRTUAL. Without this, pure ALU loops
     * on a single vCPU would never advance the virtual clock.
     */
    if (!cae_rr_kick_timer) {
        cae_rr_kick_timer = timer_new_ns(QEMU_CLOCK_HOST,
                                          cae_rr_kick_thread, NULL);
    }
    if (cae_rr_kick_timer && !timer_pending(cae_rr_kick_timer)) {
        timer_mod(cae_rr_kick_timer, cae_rr_next_kick_time());
    }
}

static void cae_rr_stop_kick_timer(void)
{
    if (cae_rr_kick_timer && timer_pending(cae_rr_kick_timer)) {
        timer_del(cae_rr_kick_timer);
    }
}

static void cae_rr_wait_io_event(void)
{
    CPUState *cpu;

    while (all_cpu_threads_idle()) {
        cae_rr_stop_kick_timer();

        /*
         * Warp the CAE virtual clock forward to the next pending
         * timer deadline. Without this, all vCPUs would sleep
         * forever because current_cycle (and thus QEMU_CLOCK_VIRTUAL)
         * only advances during instruction execution. Timer-based
         * wakeups (stimecmp, vstimecmp, PMU overflow) need the clock
         * to reach their deadline while CPUs are idle.
         */
        {
            CaeEngine *engine = cae_get_engine();
            if (engine && runstate_is_running()) {
                /*
                 * Mask out QEMU_TIMER_ATTR_EXTERNAL so host-facing
                 * virtual timers (slirp, UI input, qcow2 cache) do
                 * not pull the guest clock forward past the instant
                 * they were meant to fire. icount-common.c applies
                 * the same mask for the same reason.
                 */
                int64_t deadline = qemu_clock_deadline_ns_all(
                    QEMU_CLOCK_VIRTUAL, ~QEMU_TIMER_ATTR_EXTERNAL);
                if (deadline >= 0 && engine->base_freq_hz > 0) {
                    if (deadline > 0) {
                        int64_t now_ns = cae_cycles_to_ns(
                            qatomic_read(&engine->current_cycle),
                            engine->base_freq_hz);
                        int64_t target_ns = now_ns + deadline;
                        uint64_t target_cycle =
                            (uint64_t)(target_ns / 1000000000LL)
                                * engine->base_freq_hz
                            + (uint64_t)(target_ns % 1000000000LL)
                                * engine->base_freq_hz / 1000000000ULL;
                        cae_engine_warp_idle(engine, target_cycle);
                    }
                    /*
                     * Run expired main-loop timers inline and
                     * wake the main loop for AIO-context timers.
                     * qemu_clock_run_timers handles the main-loop
                     * timerlist; qemu_notify_event wakes the event
                     * loop so AIO timers get processed too.
                     */
                    qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
                    qemu_notify_event();
                    continue;
                }
            }
        }

        qemu_cond_wait_bql(first_cpu->halt_cond);
    }

    cae_rr_start_kick_timer();

    CPU_FOREACH(cpu) {
        qemu_process_cpu_events_common(cpu);
    }
}

static void cae_rr_deal_with_unplugged_cpus(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (cpu->unplug && !cpu_can_run(cpu)) {
            cae_cpu_destroy(cpu);
            break;
        }
    }
}

int cae_cpu_exec(CPUState *cpu)
{
    CaeEngine *engine = cae_get_engine();
    CaeCpu *cae_cpu = engine ? cae_engine_find_cpu(engine, cpu) : NULL;
    int ret;

    assert(tcg_enabled());

    /*
     * AC-11 determinism: once a benchmark has written its completion
     * sentinel, cae_mem_access_notify latches `counters_frozen` and
     * calls cpu_exit() to break the current TB. From that point on
     * every subsequent cae_cpu_exec MUST be a no-op — otherwise the
     * halt-loop (ebreak -> wfi -> j halt) dispatches through
     * HELPER(lookup_tb_ptr)'s branch-resolve + charge sequence, and
     * any timing between run-cae.py's QMP cont/stop slice and the
     * next TB boundary makes the sample non-deterministic. Return
     * EXCP_HALTED so the caller treats the CPU as idle; the
     * run-cae.py `stop` command then pauses the VM on a stable
     * stop boundary and counters are bit-for-bit identical across
     * runs.
     */
    if (engine && qatomic_read(&engine->counters_frozen)) {
        return EXCP_HALTED;
    }

    /* Set current CAE CPU context so softmmu hooks can find it */
    cae_set_current_cpu(cae_cpu);

    /*
     * active_uop points at the persistent per-CaeCpu storage set up
     * by cae_cpu_init. Do NOT reset it to a fresh blank uop on each
     * slice: the RR kick timer (QEMU_CLOCK_HOST-based) forces
     * cpu_exec to return at host-wall-clock boundaries, and if
     * active_uop's classification were blanked per slice, the FIRST
     * TB of every new slice would charge with blank metadata —
     * silently dropping its bpred attribution. See
     * BL-20260418-active-uop-persist-across-slices.
     */
    if (cae_cpu) {
        cae_cpu->active_uop = &cae_cpu->active_uop_pool[0];
        /*
         * AC-K-10: populate active_uop->pc with the CPU's current
         * architectural PC before the very first cpu_exec slice so
         * the first HELPER(lookup_tb_ptr) charge captures the real
         * first retired PC (not the zero left by cae_cpu_init). The
         * hook is a no-op on subsequent slices (active_uop->pc is
         * already non-zero per BL-20260418-active-uop-persist-across-
         * slices).
         */
        cae_cpu_prep_for_exec(cae_cpu);
    }

    cae_tbs_started = 0;
    cae_tbs_charged = 0;

    cpu_exec_start(cpu);
    ret = cpu_exec(cpu);
    cpu_exec_end(cpu);

    /*
     * Per-TB timing is charged inline by cae_charge_executed_tb()
     * in cpu-exec.c after each TB. Synchronous exceptions longjmp
     * past the inline charge, so started > charged means faulting
     * TBs were missed. Compensate with CPI=1 for each missed TB.
     */
    if (cae_cpu && engine && cae_tbs_started > cae_tbs_charged &&
        !qatomic_read(&engine->counters_frozen)) {
        /*
         * Once the sentinel-write freeze has fired, the post-
         * sentinel halt loop (ebreak + wfi + j halt) will keep
         * driving started/charged counters past parity, but the
         * gate's snapshot is already "done" at the sentinel
         * store. Skipping fault-compensation preserves the
         * frozen counter view — otherwise each cpu_exec slice
         * that includes one-insn-per-tb halt retirement would
         * compensate CPI=1 missed and re-introduce run-to-run
         * variance.
         */
        unsigned missed = cae_tbs_started - cae_tbs_charged;
        /*
         * Each missed TB retired one instruction before the
         * synchronous fault longjmped past cae_charge_executed_tb, so
         * the compensation must bump cycle_count and insn_count by
         * the full `missed` count (CPI=1, batched). cae_cpu_advance()
         * is intentionally "one insn that took N cycles" and would
         * undercount insn_count for back-to-back faults.
         */
        qatomic_set(&cae_cpu->cycle_count,
                        qatomic_read(&cae_cpu->cycle_count) + missed);
        qatomic_set(&cae_cpu->insn_count,
                        qatomic_read(&cae_cpu->insn_count) + missed);
        cae_engine_advance_cycle(engine, missed);
        /*
         * cae_charge_executed_tb() notifies QEMU_CLOCK_VIRTUAL on the
         * normal TB-retire path; the exception-longjmp path misses
         * that, leaving any stimecmp / vstimecmp / PMU deadline that
         * would have fired on the faulting instruction stuck until
         * some later access wakes the timer code. Notify here so
         * virtual-clock propagation matches the non-exception path.
         */
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }

    if (cae_cpu) {
        cae_cpu->active_uop = NULL;
    }
    cae_set_current_cpu(NULL);
    return ret;
}

void cae_cpu_destroy(CPUState *cpu)
{
    cpu_thread_signal_destroyed(cpu);
}

/*
 * CAE RR main thread: round-robin all vCPUs in a single host thread.
 * Each instruction executed advances the CAE cycle counter.
 */
static void cae_rr_force_rcu(Notifier *notify, void *data)
{
    cae_rr_kick_next_cpu();
}

static void *cae_rr_cpu_thread_fn(void *arg)
{
    Notifier force_rcu;
    CPUState *cpu = arg;

    assert(tcg_enabled());
    rcu_register_thread();
    force_rcu.notify = cae_rr_force_rcu;
    rcu_add_force_rcu_notifier(&force_rcu);
    tcg_register_thread();

    bql_lock();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->neg.can_do_io = true;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    /* Wait for initial kick-off after machine start */
    while (cpu_is_stopped(first_cpu)) {
        qemu_cond_wait_bql(first_cpu->halt_cond);
        CPU_FOREACH(cpu) {
            current_cpu = cpu;
            qemu_process_cpu_events_common(cpu);
        }
    }

    cae_rr_start_kick_timer();

    cpu = first_cpu;
    cpu->exit_request = 1;

    while (1) {
        bql_unlock();
        replay_mutex_lock();
        bql_lock();
        replay_mutex_unlock();

        if (!cpu) {
            cpu = first_cpu;
        }

        while (cpu && cpu_work_list_empty(cpu) && !cpu->exit_request) {
            qatomic_set_mb(&cae_rr_current_cpu, cpu);
            current_cpu = cpu;

            qemu_clock_enable(QEMU_CLOCK_VIRTUAL,
                              (cpu->singlestep_enabled & SSTEP_NOTIMER) == 0);

            if (cpu_can_run(cpu)) {
                int r;

                bql_unlock();
                r = cae_cpu_exec(cpu);
                bql_lock();

                if (r == EXCP_DEBUG) {
                    cpu_handle_guest_debug(cpu);
                    break;
                } else if (r == EXCP_ATOMIC) {
                    /*
                     * cpu_exec() returning EXCP_ATOMIC means the
                     * one-insn TB for the atomic was started but
                     * longjmp'd past the post-TB charge hook; the
                     * fault-TB compensation inside cae_cpu_exec()
                     * has already charged that single retired
                     * instruction against the CAE counters. So we
                     * only need to restore CAE context around the
                     * re-execution in cpu_exec_step_atomic() and
                     * kick the virtual clock — no additional
                     * counter bump.
                     */
                    CaeEngine *eng = cae_get_engine();
                    CaeCpu *ccpu = eng ?
                        cae_engine_find_cpu(eng, cpu) : NULL;
                    cae_set_current_cpu(ccpu);
                    bql_unlock();
                    cpu_exec_step_atomic(cpu);
                    bql_lock();
                    if (eng) {
                        cae_engine_sync_virtual_clock(eng);
                    }
                    cae_set_current_cpu(NULL);
                    break;
                }
            } else if (cpu->stop) {
                if (cpu->unplug) {
                    cpu = CPU_NEXT(cpu);
                }
                break;
            }

            cpu = CPU_NEXT(cpu);
        }

        qatomic_set(&cae_rr_current_cpu, NULL);

        if (cpu && cpu->exit_request) {
            qatomic_set_mb(&cpu->exit_request, 0);
        }

        cae_rr_wait_io_event();
        cae_rr_deal_with_unplugged_cpus();
    }

    g_assert_not_reached();
}

void cae_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];
    static QemuCond *single_cae_halt_cond;
    static QemuThread *single_cae_cpu_thread;

    g_assert(tcg_enabled());
    tcg_cpu_init_cflags(cpu, false);

    if (!single_cae_cpu_thread) {
        single_cae_halt_cond = cpu->halt_cond;
        single_cae_cpu_thread = cpu->thread;

        snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "ALL CPUs/CAE");
        qemu_thread_create(cpu->thread, thread_name,
                           cae_rr_cpu_thread_fn,
                           cpu, QEMU_THREAD_JOINABLE);
    } else {
        g_free(cpu->thread);
        qemu_cond_destroy(cpu->halt_cond);
        g_free(cpu->halt_cond);
        cpu->thread = single_cae_cpu_thread;
        cpu->halt_cond = single_cae_halt_cond;

        cpu->thread_id = first_cpu->thread_id;
        cpu->neg.can_do_io = 1;
        cpu->created = true;
    }
}
