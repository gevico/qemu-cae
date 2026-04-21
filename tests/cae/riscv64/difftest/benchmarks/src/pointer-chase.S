/*
 * M1 microbenchmark: pointer chase.
 *
 * Walks a pre-built singly-linked list (256 nodes × 64-byte stride,
 * 16 KiB working set) for 500 outer iterations. Every chase step is a
 * dependent load on the previous load's result.
 *
 * Workload shape:
 *   - Setup pass: 256 stores + 256 counter bumps to build the list.
 *   - Steady state: 500 × 256 dependent loads ≈ 128 000 loads, roughly
 *     384 000 instructions total including loop control.
 *
 * On CAE M1 every insn retires in 1 cycle (no memory model yet). On
 * gem5 AtomicCPU the same. On gem5 TimingSimpleCPU the dependent load
 * serialises on memory latency, so pointer-chase is the expected
 * worst-case alignment gap for the M1 self-check against
 * TimingSimpleCPU (≤ 5 % budget).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 */

    .section .text
    .globl workload
workload:
    /* ---- Setup: build the linked list ---- */
    la      s0, nodes
    li      s1, 256
    li      s2, 0
    mv      t0, s0

1:  addi    t1, t0, 64
    sd      t1, 0(t0)
    addi    t0, t0, 64
    addi    s2, s2, 1
    bne     s2, s1, 1b

    /* Close the list: last->next = head. */
    addi    t0, t0, -64
    sd      s0, 0(t0)

    /* ---- Steady state: chase ---- */
    li      s3, 500
    li      s4, 0

2:  li      s5, 256
    mv      t0, s0
    li      t2, 0

3:  ld      t0, 0(t0)
    addi    t2, t2, 1
    bne     t2, s5, 3b

    addi    s4, s4, 1
    bne     s4, s3, 2b

    ret

    .section .bss
    .balign 64
    .globl nodes
nodes:
    .skip 16384
