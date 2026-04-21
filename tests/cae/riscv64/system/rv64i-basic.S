/*
 * Minimal bare-metal RV64I test for CAE vs TCG comparison.
 *
 * Tests: addi, add, lw, sw, beq, jal, and a 10-iteration loop.
 * Final register values written to memory for comparison.
 * Ends with EBREAK to signal completion (causes debug exception).
 *
 * Entry at 0x80000000 (RISC-V virt machine default reset vector
 * jumps here). Data area at 0x80000200 (within first page of RAM).
 *
 * Expected final register values:
 *   a0 = 42, a1 = 100, a2 = 142, a3 = data_base,
 *   a4 = 142, a5 = 10, t0 = 10, a6 = 99, a7 = 0xCAE0CAFE
 */

    .section .text
    .globl _start
_start:
    /* Test addi */
    li      a0, 0               /* a0 = 0 */
    addi    a0, a0, 42          /* a0 = 42 */
    addi    a1, a0, 58          /* a1 = 100 */

    /* Test add */
    add     a2, a0, a1          /* a2 = 142 */

    /* Set up data pointer via auipc (PC-relative, stays in first page) */
    auipc   a3, 0               /* a3 = current PC */
    addi    a3, a3, 512         /* a3 = PC + 512 (data area at _start+0x200+offset) */
    /* Align a3 to 0x80000200 range */
    li      a3, 0x80000200      /* Use absolute addr in first page */

    /* Test sw */
    li      t2, 142
    sw      t2, 0(a3)           /* mem[data_base] = 142 */

    /* Test lw */
    lw      a4, 0(a3)           /* a4 = 142 (from memory) */

    /* Test 10-iteration loop with beq/bne */
    li      t0, 0               /* t0 = loop counter */
    li      t1, 10              /* t1 = loop limit */
    li      a5, 0               /* a5 = accumulator */
loop:
    addi    a5, a5, 1           /* a5++ */
    addi    t0, t0, 1           /* t0++ */
    bne     t0, t1, loop        /* if t0 != 10, loop */

    /* After loop: a5 = 10, t0 = 10 */

    /* Test jal */
    jal     ra, target          /* jump to target, ra = PC+4 */
    j       done

target:
    addi    a6, zero, 99        /* a6 = 99 (proves jal worked) */
    jr      ra                  /* return */

done:
    /* Completion magic value */
    li      a7, 0xCAE0CAFE

    /* Write final register values to data area for readback */
    sd      a0, 0(a3)           /* data[0] = a0 (42) */
    sd      a1, 8(a3)           /* data[8] = a1 (100) */
    sd      a2, 16(a3)          /* data[16] = a2 (142) */
    sd      a4, 24(a3)          /* data[24] = a4 (142) */
    sd      a5, 32(a3)          /* data[32] = a5 (10) */
    sd      t0, 40(a3)          /* data[40] = t0 (10) */
    sd      a6, 48(a3)          /* data[48] = a6 (99) */
    sd      a7, 56(a3)          /* data[56] = a7 (0xCAE0CAFE) */

    /* Signal completion via EBREAK */
    ebreak

    /* Halt: loop forever (fallback) */
halt:
    wfi
    j       halt

    /* Illegal instruction test: placed after halt, never reached
     * in normal flow. Test script can jump here to verify trap. */
    .balign 4
illegal:
    .word   0x00000000          /* All-zeros is an illegal RV instruction */
