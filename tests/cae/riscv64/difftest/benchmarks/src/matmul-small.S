/*
 * M2 microbenchmark: small integer matrix multiply (8x8 * 8x8 -> 8x8).
 *
 * 8x8 matrices keep the inner loop short enough that loop overhead
 * stays small, yet large enough (512 inner multiplications) to exercise
 * load/store + ALU interleaving. Matrices are stored as uint64_t. The
 * benchmark runs the outer matmul 1024 times for a steady ~1M-insn
 * workload, close in scale to alu.S / mem-stream.S / pointer-chase.S.
 *
 * Workload shape:
 *   - Each inner step: 2 loads, 1 mul, 1 add, loop control.
 *   - Inner loop: 8 iterations per (i, j) cell.
 *   - Outer loops: 8 * 8 cells = 64 per matmul; 1024 matmul
 *     invocations => 65536 cells => 524288 inner steps.
 *   - Total retired: ~3M insns (loop overhead dominates).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 */

    .section .text
    .globl workload
workload:
    /* Argument setup */
    la      a0, matA
    la      a1, matB
    la      a2, matC
    li      t6, 1024                 /* outer-repeat count */

repeat:
    li      a3, 0                    /* i = 0 */
row_loop:
    li      a4, 0                    /* j = 0 */
col_loop:
    li      a5, 0                    /* k = 0 */
    li      a6, 0                    /* sum = 0 */

inner_loop:
    /* A[i][k]: row i (stride 8 uint64 = 64 bytes) + k */
    slli    t0, a3, 6                /* i * 64 */
    slli    t1, a5, 3                /* k * 8  */
    add     t0, t0, t1
    add     t0, t0, a0
    ld      t2, 0(t0)

    /* B[k][j] */
    slli    t0, a5, 6                /* k * 64 */
    slli    t1, a4, 3                /* j * 8  */
    add     t0, t0, t1
    add     t0, t0, a1
    ld      t3, 0(t0)

    mul     t4, t2, t3
    add     a6, a6, t4

    addi    a5, a5, 1
    li      t0, 8
    bne     a5, t0, inner_loop

    /* C[i][j] = sum */
    slli    t0, a3, 6
    slli    t1, a4, 3
    add     t0, t0, t1
    add     t0, t0, a2
    sd      a6, 0(t0)

    addi    a4, a4, 1
    li      t0, 8
    bne     a4, t0, col_loop

    addi    a3, a3, 1
    li      t0, 8
    bne     a3, t0, row_loop

    addi    t6, t6, -1
    bnez    t6, repeat

    ret

    .section .data
    .balign 64
    .globl matA
matA:
    .dword 0x0001020304050607, 0x08090a0b0c0d0e0f
    .dword 0x1011121314151617, 0x18191a1b1c1d1e1f
    .dword 0x2021222324252627, 0x28292a2b2c2d2e2f
    .dword 0x3031323334353637, 0x38393a3b3c3d3e3f
    .dword 0x4041424344454647, 0x48494a4b4c4d4e4f
    .dword 0x5051525354555657, 0x58595a5b5c5d5e5f
    .dword 0x6061626364656667, 0x68696a6b6c6d6e6f
    .dword 0x7071727374757677, 0x78797a7b7c7d7e7f

    .balign 64
    .globl matB
matB:
    .dword 0x0706050403020100, 0x0f0e0d0c0b0a0908
    .dword 0x1716151413121110, 0x1f1e1d1c1b1a1918
    .dword 0x2726252423222120, 0x2f2e2d2c2b2a2928
    .dword 0x3736353433323130, 0x3f3e3d3c3b3a3938
    .dword 0x4746454443424140, 0x4f4e4d4c4b4a4948
    .dword 0x5756555453525150, 0x5f5e5d5c5b5a5958
    .dword 0x6766656463626160, 0x6f6e6d6c6b6a6968
    .dword 0x7776757473727170, 0x7f7e7d7c7b7a7978

    .balign 64
    .globl matC
matC:
    .skip 512
