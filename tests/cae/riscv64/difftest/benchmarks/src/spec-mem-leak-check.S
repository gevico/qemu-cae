/*
 * spec-mem-leak-check: speculative-store-leak marker-address
 * negative regression.
 *
 * Plan contract (AC-K-4 negative, plan.md:93):
 *   "a wrong-path store to a marker address; after squash,
 *    the marker address is unmodified."
 *
 * Structure:
 *   1. `spec_marker` is initialized to a known sentinel
 *      (0xDEADBEEFCAFEBABE).
 *   2. The main loop is a committed-path stress workload:
 *      PRNG-driven unpredictable branches feed data-dependent
 *      stores into a rotating 1 KiB buffer (spec_buf). None of
 *      this code touches spec_marker.
 *   3. At the bottom of each iteration an architecturally
 *      never-taken branch guards a dead-code `trap_arm` that
 *      writes a corrupt value (0) to spec_marker. The gate
 *      condition is `bltz` on an always-non-negative register
 *      (PRNG low bits masked to fit in 5 bits), so the arm is
 *      unreachable on the correct path — but a real-spec
 *      engine that mispredicts this branch speculatively
 *      executes the trap store. If squash fails, spec_marker
 *      is corrupted.
 *   4. End-of-run writes spec_marker's current value to
 *      spec_result. In DEC-4 mode (current, no real spec),
 *      spec_marker always equals the sentinel and spec_result
 *      = 0xDEADBEEFCAFEBABE. Under real-spec with a leak bug,
 *      spec_result surfaces the corrupted value; NEMU's trace
 *      records the architectural sentinel and byte-diff
 *      catches the divergence via spec_result's
 *      architectural-path store record.
 *
 * Because the two branch arms of the stress loop both bump a
 * DIFFERENT buffer slot from any past iteration, no prior
 * architectural store can alias-overwrite a leaked wrong-path
 * store; the marker address is isolated to the never-taken
 * trap arm only.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 */

    .section .text
    .globl workload
workload:
    /* --- Initialise spec_marker with the sentinel. --- */
    la      t0, spec_marker
    li      t1, 0xDEADBEEFCAFEBABE
    sd      t1, 0(t0)

    /* --- Zero the stress buffer. --- */
    la      t0, spec_buf
    li      t1, 256
1:  sw      x0, 0(t0)
    addi    t0, t0, 4
    addi    t1, t1, -1
    bnez    t1, 1b

    /* --- Main stress loop. --- */
    li      t0, 0                   /* i */
    li      t1, 1024                /* iteration count */
    li      t2, 0x13579BDF          /* xorshift state */
    la      s1, spec_buf            /* buffer base */

2:
    /* xorshift64 step. */
    slli    t3, t2, 13
    xor     t2, t2, t3
    srli    t3, t2, 7
    xor     t2, t2, t3
    slli    t3, t2, 17
    xor     t2, t2, t3

    /*
     * Committed-path data-dependent store. One branch arm
     * stores to slot i&0xff, the other arm stores to slot
     * ((i+128)&0xff), so the two arms never alias on the
     * same buffer word within a single iteration. BPred sees
     * high branch-density unpredictable traffic.
     */
    andi    t4, t2, 1
    beqz    t4, 3f
    /* taken arm: slot = i & 0xff */
    andi    t5, t0, 0xff
    j       4f
3:
    /* not-taken arm: slot = (i+128) & 0xff */
    addi    t5, t0, 128
    andi    t5, t5, 0xff
4:
    slli    t5, t5, 2
    add     t6, s1, t5
    lw      t3, 0(t6)
    addi    t3, t3, 1
    sw      t3, 0(t6)

    /*
     * Architecturally-never-taken trap gate.
     *
     * `t2` is an unsigned 64-bit PRNG output. Masking off the
     * top bit with `andi t3, t2, 0x7f` yields t3 in [0, 127]
     * — always non-negative. `bltz t3, trap_arm` is therefore
     * ALWAYS not-taken on the architectural path.
     *
     * Under real-spec, if the BPred happens to mispredict
     * this branch `taken` (e.g., cold-entry default, pattern
     * aliasing), the speculative path enters `trap_arm` and
     * writes 0 to spec_marker. A squash bug lets that store
     * commit; the final `spec_result` store then carries 0
     * instead of the sentinel, and NEMU's byte-diff catches
     * the divergence.
     */
    andi    t3, t2, 0x7f
    bltz    t3, trap_arm

    /* Loop continuation. */
    addi    t0, t0, 1
    bne     t0, t1, 2b

    /* --- Final: publish spec_marker's value to spec_result. --- */
    la      t0, spec_marker
    ld      t1, 0(t0)
    la      t2, spec_result
    sd      t1, 0(t2)
    ret

trap_arm:
    /*
     * Dead code on the architectural path. If real-spec
     * speculation enters here without squash, spec_marker is
     * clobbered with 0 — trace-diff-visible. Jump back to
     * the loop continuation so the benchmark terminates
     * normally even if (hypothetically) this arm runs under
     * a buggy engine.
     */
    la      t3, spec_marker
    sd      x0, 0(t3)
    addi    t0, t0, 1
    bne     t0, t1, 2b
    la      t0, spec_marker
    ld      t1, 0(t0)
    la      t2, spec_result
    sd      t1, 0(t2)
    ret

    .section .bss
    .balign 8
    .globl spec_marker
spec_marker:
    .skip 8

    .balign 8
    .globl spec_buf
spec_buf:
    .skip 1024

    .balign 8
    .globl spec_result
spec_result:
    .skip 8
