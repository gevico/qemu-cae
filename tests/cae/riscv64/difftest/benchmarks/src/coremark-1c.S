/*
 * M2 microbenchmark: coremark-lite single-core (inline).
 *
 * A reduced, no-stdio version of the coremark core-loop. Unlike the
 * original split into cm_list_walk / cm_crc16 / cm_state_machine
 * helpers, this version inlines everything into `workload` to avoid
 * nested CALL / RET interactions with gem5 RiscvMinorCPU SE-mode
 * scheduling, which was observed to crash on multi-level calls during
 * M2 round-0 benchmark bring-up. Functional content is the same:
 *
 *   - Linked-list traversal (8 nodes, 4096 outer iterations).
 *   - CRC-16 over a 256-byte buffer, 256 iterations.
 *   - State-machine walk over a 512-byte stream, 256 iterations.
 *
 * Total retired: ~2M insns; mixed ALU / branch / load / store /
 * indirect-load / loop-heavy — the AC-4 mixed workload probe.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 */

    .section .text
    .globl workload
workload:
    /* ---------- Linked-list traversal ---------- */
    la      a0, list_head
    li      a1, 4096                 /* outer repeat */
    li      a2, 0                    /* acc */

walk_outer:
    mv      a3, a0                   /* node = head */
walk_inner:
    beqz    a3, walk_next
    ld      a4, 8(a3)
    add     a2, a2, a4
    slli    a2, a2, 48
    srli    a2, a2, 48               /* clamp to low 16 bits */
    ld      a3, 0(a3)
    j       walk_inner
walk_next:
    addi    a1, a1, -1
    bnez    a1, walk_outer

    la      a3, list_acc
    sd      a2, 0(a3)

    /* ---------- CRC-16 (CCITT-reversed) ---------- */
    li      t6, 256                  /* outer repeat */

crc_outer:
    li      t0, 0xFFFF               /* crc accumulator */
    la      t1, crc_buffer
    li      t2, 256                  /* buffer length */

crc_inner:
    lbu     t3, 0(t1)
    xor     t0, t0, t3
    li      t4, 8                    /* 8 bit-shift iterations */
crc_bit:
    andi    t5, t0, 1
    srli    t0, t0, 1
    beqz    t5, crc_skip
    li      a5, 0xA001
    xor     t0, t0, a5
crc_skip:
    addi    t4, t4, -1
    bnez    t4, crc_bit

    addi    t1, t1, 1
    addi    t2, t2, -1
    bnez    t2, crc_inner

    addi    t6, t6, -1
    bnez    t6, crc_outer

    la      t1, crc_result
    sd      t0, 0(t1)

    /* ---------- State-machine walk ---------- */
    li      a0, 256                  /* outer repeat */

sm_outer:
    li      a1, 0                    /* state */
    la      a2, sm_buffer
    li      a3, 512                  /* buffer length */

sm_inner:
    lbu     a4, 0(a2)
    srli    a4, a4, 6                /* bucket 0..3 */

    /* next_state = (state + bucket + 1) & 3 */
    add     a1, a1, a4
    addi    a1, a1, 1
    andi    a1, a1, 3

    addi    a2, a2, 1
    addi    a3, a3, -1
    bnez    a3, sm_inner

    addi    a0, a0, -1
    bnez    a0, sm_outer

    la      a2, sm_result
    sd      a1, 0(a2)

    ret

    .section .data
    .balign 64
    .globl list_head
list_head:
    .dword list_n1, 0x1234
list_n1:
    .dword list_n2, 0x2345
list_n2:
    .dword list_n3, 0x3456
list_n3:
    .dword list_n4, 0x4567
list_n4:
    .dword list_n5, 0x5678
list_n5:
    .dword list_n6, 0x6789
list_n6:
    .dword list_n7, 0x789A
list_n7:
    .dword 0, 0x89AB

    .balign 8
list_acc:
    .dword 0

    .balign 64
    .globl crc_buffer
crc_buffer:
    .rept 16
    .byte 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38
    .byte 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40
    .endr

    .balign 8
crc_result:
    .dword 0

    .balign 64
    .globl sm_buffer
sm_buffer:
    .rept 32
    .byte 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
    .byte 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00
    .endr

    .balign 8
sm_result:
    .dword 0
