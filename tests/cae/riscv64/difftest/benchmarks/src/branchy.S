/*
 * M2 microbenchmark: high-density unpredictable branches.
 *
 * Generates a pseudo-random bit stream via a 64-bit xorshift PRNG and
 * branches on the low bit of each output. The direction is
 * near-equiprobable (~50% taken) and carries no exploitable pattern
 * beyond the xorshift state, which is what gives any learning
 * predictor a hard time. A simple 2-bit local predictor settles to
 * ~50% mispredict rate on this workload, which is exactly the signal
 * AC-4 uses to detect that bpred wiring is live.
 *
 * Workload shape:
 *   - 100000 iterations of (xorshift + branch-on-low-bit).
 *   - Each iteration: 1 xorshift chain (~6 insns) + 1 branch
 *     + 1 loop control branch + 2 adders that differentiate the two
 *     sides of the first branch. ~10 insns per iteration.
 *   - ~1M retired instructions overall; ~100k high-density branches.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 */

    .section .text
    .globl workload
workload:
    li      t0, 0                    /* loop counter */
    li      t1, 100000               /* iteration count */
    li      t2, 0x13579BDF           /* xorshift state (non-zero) */
    li      t3, 0                    /* taken-branch accumulator */
    li      t4, 0                    /* not-taken accumulator */

1:
    /* xorshift64 step: state ^= state << 13; state ^= state >> 7; state ^= state << 17 */
    slli    t5, t2, 13
    xor     t2, t2, t5
    srli    t5, t2, 7
    xor     t2, t2, t5
    slli    t5, t2, 17
    xor     t2, t2, t5

    /* Branch on the low bit of the PRNG output. Unpredictable. */
    andi    t5, t2, 1
    beq     t5, x0, 2f
    addi    t3, t3, 1                /* taken path: bump taken counter */
    j       3f
2:
    addi    t4, t4, 1                /* not-taken path: bump NT counter */
3:
    addi    t0, t0, 1
    bne     t0, t1, 1b

    ret
