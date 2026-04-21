/*
 * CAE retired-insn trace: RISC-V fast-path store field derivation.
 *
 * When a store retires through the TCG fast-path (no softmmu
 * helper call), the shared mem hook at
 * cae/engine.c:cae_mem_access_notify never fires and leaves
 * uop->mem_size / mem_addr / mem_value at their classifier
 * defaults (0). Under `cae_tlb_force_slow_active=false` (OoO
 * mode, AC-K-13) this is the common case, not the exception.
 *
 * The retire-side trace emitter needs those fields to
 * byte-match NEMU. This header declares a pure helper that
 * derives them from the instruction encoding + integer register
 * file so the per-target emitter can backfill the uop fields at
 * trace-emit time.
 *
 * Keep arch-neutral consumers out of this header: the helper
 * operates on raw RISC-V instruction bits. RISC-V is the only
 * target with a CAE emitter at the moment; other targets would
 * land their own parallel helpers under target/<arch>/cae/.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_RISCV_TRACE_DERIVE_H
#define CAE_RISCV_TRACE_DERIVE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct CaeUop CaeUop;

typedef struct CaeRiscvStoreFields {
    uint16_t mem_size;
    uint64_t mem_addr;
    uint64_t mem_value;
} CaeRiscvStoreFields;

/*
 * Derive (mem_size, mem_addr, mem_value) for a retiring RISC-V
 * store whose uop->mem_size is 0 because the softmmu helper
 * did not fire (TCG fast-path). `gpr` must point at a 32-entry
 * array of the integer register file (x0..x31) captured at
 * retire time — the caller reads env->gpr on the live CPUState.
 *
 * Returns true and populates *out on any recognised store
 * encoding (OP_STORE, OP_STORE_FP for FSW/FSD, C.SW / C.SD /
 * C.SWSP / C.SDSP). Returns false otherwise (e.g. C.FSD or an
 * instruction that is_store-classified but whose encoding is
 * not covered here); callers then leave trace fields untouched.
 *
 * Pure function: no globals, no I/O.
 */
bool cae_riscv_trace_derive_store_fields(const CaeUop *uop,
                                         const uint64_t *gpr,
                                         CaeRiscvStoreFields *out);

#endif /* CAE_RISCV_TRACE_DERIVE_H */
