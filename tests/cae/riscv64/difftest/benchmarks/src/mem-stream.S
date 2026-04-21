/*
 * M1 microbenchmark: sequential memory stream.
 *
 * Writes and reads a 4 KiB scratch buffer in unit-stride fashion for
 * a fixed number of outer iterations. Exercises the load/store path
 * (softmmu hook on CAE; SimpleMemory on gem5 TimingSimpleCPU).
 *
 * Workload shape:
 *   - Inner loop: 32 iterations × (1 store + 1 load + 2 ALU ops + 2
 *     pointer bumps + 1 loop-control) = ~7 insns per inner iter; outer
 *     overhead is tiny. 1000 outer iterations → ~232 000 instructions.
 *   - Buffer on .bss, 64-byte aligned, 4 KiB = 64 cache-lines touched
 *     per pass.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 */

    .section .text
    .globl workload
workload:
    la      s0, buffer
    li      s1, 1000
    li      s2, 0

1:  mv      t0, s0
    li      t1, 32
    li      t2, 0
    mv      t3, s2

2:  sd      t3, 0(t0)
    ld      t4, 0(t0)
    addi    t3, t3, 1
    xor     t3, t3, t4
    addi    t0, t0, 64
    addi    t2, t2, 1
    bne     t2, t1, 2b

    addi    s2, s2, 1
    bne     s2, s1, 1b

    ret

    .section .bss
    .balign 64
    .globl buffer
buffer:
    .skip 4096
