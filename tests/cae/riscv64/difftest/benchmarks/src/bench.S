/*
 * Shared prologue / epilogue for the M1 self-check microbenchmarks.
 *
 * Each workload is expected to define a `workload` symbol implementing
 * the benchmark body. This file provides `_start`, a stack frame, and
 * the two tail actions required by the difftest drivers:
 *
 *   1. Write the completion sentinel 0xCAE0CAFE to the global `sentinel`
 *      word. The CAE harness resolves &sentinel from the ELF symbol
 *      table and polls the guest via QMP xp until it matches.
 *   2. Execute ebreak. gem5 SE mode treats ebreak as the program
 *      terminator and dumps stats; QEMU bare-metal drops into a halt
 *      loop (the sentinel-polling harness uses `quit` afterwards).
 *
 * The same ELF is consumed by both backends. On QEMU it is loaded via
 * `-bios <elf>` and the reset vector dispatches to _start. On gem5 SE
 * the ELF entry point is used directly; the sentinel write is a benign
 * no-op for gem5's correctness model and contributes the same number
 * of instructions on both sides.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 */

    .section .text.init
    .globl _start
_start:
#ifndef GEM5_SE
    /*
     * Disable M-mode interrupts before the measured workload runs.
     * Without this, mtime / mtip firings depend on host wallclock
     * timing and surface as 1-2 insn variance between otherwise-
     * identical runs (the timer trap longjmps past
     * cae_charge_executed_tb, so the exception-compensation path
     * in cae_cpu_exec charges one extra missed TB). Clearing
     * mstatus.MIE prevents the trap delivery, making every
     * sampling window on this workload independent of the host's
     * clock and the QMP cont/stop slice timing.
     *
     * gem5 SE mode runs the binary in user mode where mie / mstatus
     * are not accessible — `csrw mie, zero` would panic with
     * "mie is not accessible in 0". SE mode also has no interrupt
     * controller, so we skip the CSR writes there entirely.
     */
    csrw    mie, zero
    csrci   mstatus, 0x8
#endif
    la      sp, _stack_top
    call    workload

    /* Completion sentinel: drivers poll sentinel == 0xCAE0CAFE. */
    la      t0, sentinel
    li      t1, 0xCAE0CAFE
    sd      t1, 0(t0)

#ifdef GEM5_SE
    /*
     * gem5 SE mode treats ecall with a7=93 (exit) as a process
     * termination and dumps stats.txt.
     */
    li      a7, 93
    li      a0, 0
    ecall
#else
    /*
     * QEMU bare-metal: ebreak raises a debug exception which the
     * accel layer converts to EXCP_DEBUG; under -accel cae the RR
     * loop breaks out and waits for the QMP driver to observe the
     * sentinel and issue `quit`. We deliberately do NOT ecall here:
     * M-mode ecall with mtvec=0 would redirect the CPU to address 0
     * and start a fault_fetch cascade that stalls QMP processing.
     *
     * wfi below parks the CPU in a halted state so the CAE warp-idle
     * path advances current_cycle without bumping insn_count. Using
     * `j halt` alone would let the halt loop retire one extra
     * instruction per iteration and break AC-11 serial determinism.
     */
    ebreak
halt:
    wfi
    j       halt
#endif

    .section .bss
    .balign 16
    .globl sentinel
sentinel:
    .skip 16
