/*
 * M1 microbenchmark: pure ALU register-to-register loop.
 *
 * Runs a tight sequence of add/addi/xor/slli/srli/and instructions
 * for a fixed number of iterations. No memory accesses beyond the
 * final sentinel write handled by bench.S.
 *
 * Workload shape:
 *   - Each iteration executes 8 ALU ops (reg-reg / reg-imm).
 *   - 100000 iterations → 800000 ALU ops steady state.
 *   - Loop control adds 2 insns per iteration, giving 10 insns per
 *     iteration (1 000 000 instructions in the `workload` body).
 *
 * Both gem5 AtomicCPU (~CPI=1) and CAE CPI=1 should retire the same
 * number of cycles on this workload; IPC delta is the direct metric.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 */

    .section .text
    .globl workload
workload:
    li      t0, 0
    li      t1, 100000
    li      t2, 1
    li      t3, 2
    li      t4, 0x5a5a5a5a
    li      t5, 0
    li      t6, 0

1:  add     t5, t2, t3
    addi    t2, t2, 3
    xor     t6, t5, t4
    slli    t3, t3, 1
    srli    t3, t3, 1
    and     t3, t3, t4
    addi    t3, t3, 1
    add     t2, t2, t6

    addi    t0, t0, 1
    bne     t0, t1, 1b

    ret
