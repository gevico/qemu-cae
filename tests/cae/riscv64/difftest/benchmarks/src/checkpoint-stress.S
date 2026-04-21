/*
 * checkpoint-stress: high-density unpredictable branches, for
 * speculation checkpoint save / restore stress.
 *
 * Plan contract (AC-K-4 positive, plan.md:92):
 *   "tests/cae-difftest/benchmarks/checkpoint-stress.riscv
 *    (high-density unpredictable branches) survives 100 000+
 *    restores without crash and without state drift detected
 *    by NEMU difftest."
 *
 * Each iteration of the main loop executes four independent
 * PRNG-gated branches (each near-equiprobable) plus the
 * loop-control branch. Over ~25 000 iterations the workload
 * produces ~100 000 unpredictable high-density mispredictable
 * events.
 *
 * Under real-spec (future t-tcg-spec-path landing), each
 * mispredict drives one checkpoint save + restore cycle, so
 * the benchmark stresses the save/restore path 100 000+
 * times in a single run. Under the current DEC-4 functional-
 * oracle mode no restores happen; the benchmark still exits
 * cleanly and its committed state matches NEMU's byte-for-
 * byte, so the tier-1 byte-diff regression harness stays
 * useful as a regression baseline for the future engine.
 *
 * The four branches in each iteration accumulate into two
 * counters (taken_acc / nt_acc), which the benchmark stores
 * to well-known addresses (`ck_taken`, `ck_nt`) at the end
 * so the NEMU trace-diff consumer picks up the architected
 * per-retire state.
 *
 * Workload shape totals:
 *   - Main loop: 25 000 iterations × ~20 insns/iteration
 *     (xorshift 8 insns + 4 branches × 3 insns + loop ctl 3
 *     insns) ≈ 500 000 insns.
 *   - Prologue / final / epilogue: ~15 insns.
 *   Total ≈ 500 000 retired instructions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 */

    .section .text
    .globl workload
workload:
    li      t0, 0                       /* loop counter */
    li      t1, 25000                   /* iteration count */
    li      t2, 0x13579BDF              /* xorshift state */
    li      t3, 0                       /* taken_acc */
    li      t4, 0                       /* nt_acc */

1:
    /* xorshift64 step. */
    slli    t5, t2, 13
    xor     t2, t2, t5
    srli    t5, t2, 7
    xor     t2, t2, t5
    slli    t5, t2, 17
    xor     t2, t2, t5

    /* Branch 1: low bit of PRNG */
    andi    t5, t2, 1
    beqz    t5, 2f
    addi    t3, t3, 1
    j       3f
2:  addi    t4, t4, 1
3:

    /* Branch 2: bit 1 */
    andi    t5, t2, 2
    beqz    t5, 4f
    addi    t3, t3, 2
    j       5f
4:  addi    t4, t4, 2
5:

    /* Branch 3: bit 2 */
    andi    t5, t2, 4
    beqz    t5, 6f
    addi    t3, t3, 4
    j       7f
6:  addi    t4, t4, 4
7:

    /* Branch 4: bit 3 */
    andi    t5, t2, 8
    beqz    t5, 8f
    addi    t3, t3, 8
    j       9f
8:  addi    t4, t4, 8
9:

    addi    t0, t0, 1
    bne     t0, t1, 1b

    /* Publish architected state to well-known addresses so
     * the NEMU trace-diff consumer observes it. */
    la      t0, ck_taken
    sd      t3, 0(t0)
    la      t0, ck_nt
    sd      t4, 0(t0)
    ret

    .section .bss
    .balign 8
    .globl ck_taken
ck_taken:
    .skip 8

    .balign 8
    .globl ck_nt
ck_nt:
    .skip 8
