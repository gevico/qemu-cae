/*
 * User-space link layout for gem5 SE mode.
 *
 * Same source as the bare-metal QEMU variant, but loaded at 0x00010000
 * (standard static RISC-V user-space entry) rather than at the
 * machine-mode reset vector. gem5's SE mode honours the ELF load
 * addresses; QEMU system emulation cannot use this layout because the
 * virt machine's RAM starts at 0x80000000.
 *
 * The instruction stream is bit-for-bit identical to the bare-metal
 * build (same .S, same flags, same section layout); only the VMA/LMA
 * and the sentinel placement change.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x00010000;

    .text.init : { *(.text.init) }
    .text      : { *(.text .text.*) }
    .rodata    : { *(.rodata .rodata.*) }

    . = ALIGN(16);
    .data      : { *(.data .data.*) }

    . = ALIGN(16);
    .bss       : {
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(16);
        _stack_bottom = .;
        . += 0x10000;                       /* 64 KiB stack */
        _stack_top = .;
    }
}
