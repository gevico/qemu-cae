/*
 * Minimal RISC-V bare-metal linker script for the M1 self-check
 * microbenchmarks.
 *
 * Layout:
 *   - .text.init at 0x80000000 (QEMU RISC-V virt machine reset vector)
 *   - .text / .rodata / .data / .bss grow up from there
 *   - A small stack at the top of the 16 MiB window we reserve
 *
 * Both QEMU (loaded via -bios) and gem5 SE mode accept this. In SE
 * mode gem5 honours the ELF entry point and ignores the 0x80000000
 * address; in bare-metal QEMU the machine's reset vector jumps to
 * that address directly.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    .text.init : { *(.text.init) }
    .text      : { *(.text .text.*) }
    .rodata    : { *(.rodata .rodata.*) }

    . = ALIGN(16);
    .data      : { *(.data .data.*) }

    . = ALIGN(16);
    .bss       : { *(.bss .bss.*) *(COMMON) }

    . = ALIGN(16);
    _stack_bottom = .;
    . += 0x10000;                       /* 64 KiB stack */
    _stack_top = .;
}
