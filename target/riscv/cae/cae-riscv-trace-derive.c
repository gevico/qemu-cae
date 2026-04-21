/*
 * CAE retired-insn trace: RISC-V fast-path store field derivation.
 *
 * Kept in its own compilation unit so tests/unit/test-cae can link
 * the pure helper without pulling in the softmmu-only trace writer
 * (stdio FILE*, error_report, qemu_cpu linkage, etc). No globals,
 * no I/O — caller supplies the integer register file pointer.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "cae/uop.h"
#include "cae/riscv-trace-derive.h"

/*
 * Extract rs1 from an RV I/S-type 32-bit encoding.
 */
static inline uint8_t rv_rs1(uint32_t insn)
{
    return (uint8_t)((insn >> 15) & 0x1f);
}

/*
 * Extract rs2 from an RV R/S-type 32-bit encoding.
 */
static inline uint8_t rv_rs2(uint32_t insn)
{
    return (uint8_t)((insn >> 20) & 0x1f);
}

/*
 * S-type sign-extended 12-bit immediate:
 *   imm[11:5] = insn[31:25], imm[4:0] = insn[11:7].
 */
static inline int32_t rv_s_imm(uint32_t insn)
{
    int32_t imm = (int32_t)(((insn >> 25) << 5) | ((insn >> 7) & 0x1f));
    return (imm << 20) >> 20;
}

bool cae_riscv_trace_derive_store_fields(const CaeUop *uop,
                                         const uint64_t *gpr,
                                         CaeRiscvStoreFields *out)
{
    if (uop == NULL || gpr == NULL || out == NULL || !uop->is_store) {
        return false;
    }
    out->mem_size = 0;
    out->mem_addr = 0;
    out->mem_value = 0;

    if (uop->insn_bytes == 4) {
        uint32_t insn = uop->insn;
        uint32_t op7 = insn & 0x7f;
        uint32_t funct3 = (insn >> 12) & 0x7;
        uint8_t rs1 = rv_rs1(insn);
        uint8_t rs2 = rv_rs2(insn);
        int32_t imm = rv_s_imm(insn);

        if (op7 == 0x23) {
            /* OP_STORE: SB/SH/SW/SD */
            switch (funct3) {
            case 0: out->mem_size = 1; break;
            case 1: out->mem_size = 2; break;
            case 2: out->mem_size = 4; break;
            case 3: out->mem_size = 8; break;
            default: return false;
            }
        } else if (op7 == 0x27) {
            /*
             * OP_STORE_FP: FSW/FSD. FP register file lives in
             * env->fpr, not gpr; the caller cannot capture
             * mem_value via gpr[rs2]. Return false so the
             * classifier-populated zero fields stay in place
             * rather than emit a wrong architectural value.
             */
            return false;
        } else {
            return false;
        }

        if (rs1 >= 32) {
            return false;
        }
        out->mem_addr = gpr[rs1] + (int64_t)imm;

        if (rs2 >= 32) {
            return false;
        }
        uint64_t v = gpr[rs2];
        if (out->mem_size < 8) {
            v &= ((uint64_t)1 << (out->mem_size * 8)) - 1;
        }
        out->mem_value = v;
        return true;
    }

    if (uop->insn_bytes == 2) {
        uint16_t ci = (uint16_t)uop->insn;
        uint8_t quadrant = ci & 0x3;
        uint8_t funct3 = (ci >> 13) & 0x7;

        if (quadrant == 0 && funct3 == 6) {
            /*
             * C.SW rs2', uimm(rs1') — RVC Table.
             * uimm = {insn[5], insn[12:10], insn[6]} << 2
             */
            uint8_t rs1 = (uint8_t)(((ci >> 7) & 0x7) + 8);
            uint8_t rs2 = (uint8_t)(((ci >> 2) & 0x7) + 8);
            uint32_t uimm = (((ci >> 6) & 0x1) << 2) |
                            (((ci >> 10) & 0x7) << 3) |
                            (((ci >> 5) & 0x1) << 6);
            out->mem_size = 4;
            out->mem_addr = gpr[rs1] + uimm;
            out->mem_value = gpr[rs2] & 0xffffffffull;
            return true;
        }
        if (quadrant == 0 && funct3 == 7) {
            /*
             * C.SD rs2', uimm(rs1') — RV64 only.
             * uimm = {insn[6:5], insn[12:10]} << 3
             */
            uint8_t rs1 = (uint8_t)(((ci >> 7) & 0x7) + 8);
            uint8_t rs2 = (uint8_t)(((ci >> 2) & 0x7) + 8);
            uint32_t uimm = (((ci >> 10) & 0x7) << 3) |
                            (((ci >> 5) & 0x3) << 6);
            out->mem_size = 8;
            out->mem_addr = gpr[rs1] + uimm;
            out->mem_value = gpr[rs2];
            return true;
        }
        if (quadrant == 2 && funct3 == 6) {
            /*
             * C.SWSP rs2, uimm(sp).
             * uimm = {insn[8:7], insn[12:9]} << 2
             */
            uint8_t rs2 = (uint8_t)((ci >> 2) & 0x1f);
            uint32_t uimm = (((ci >> 9) & 0xf) << 2) |
                            (((ci >> 7) & 0x3) << 6);
            out->mem_size = 4;
            out->mem_addr = gpr[2] + uimm;
            out->mem_value = gpr[rs2] & 0xffffffffull;
            return true;
        }
        if (quadrant == 2 && funct3 == 7) {
            /*
             * C.SDSP rs2, uimm(sp) — RV64 only.
             * uimm = {insn[9:7], insn[12:10]} << 3
             */
            uint8_t rs2 = (uint8_t)((ci >> 2) & 0x1f);
            uint32_t uimm = (((ci >> 10) & 0x7) << 3) |
                            (((ci >> 7) & 0x7) << 6);
            out->mem_size = 8;
            out->mem_addr = gpr[2] + uimm;
            out->mem_value = gpr[rs2];
            return true;
        }
        /*
         * C.FSD / C.FSDSP: FP store, not covered for same reason
         * as FSD above.
         */
        return false;
    }

    return false;
}
