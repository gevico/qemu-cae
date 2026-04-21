/*
 * CAE (Cycle Approximate Engine) - RISC-V retire-boundary trace writer
 *
 * Implements the per-target emitter vtable declared in
 * include/cae/trace-emit.h. Reads retiring GPR values from the
 * RISC-V CPUState and serialises them into the on-disk CaeTraceRecord
 * format defined in include/cae/trace.h (AC-K-2).
 *
 * The file is opened lazily on the first retired instruction so a
 * run that never reaches retire does not leave an empty header
 * behind. The 16-byte header is written before the first record.
 * All output passes through stdio's default buffering; explicit
 * flush happens at cae_trace_close() (atexit-installed the first
 * time a record is written).
 *
 * One record per retired architectural instruction; no merging or
 * speculation tracking in v1 (CAE is strictly in-order at the retire
 * interface even when the OoO scaffold is active). Split-store and
 * exception cases set the corresponding flag bits.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "cae/pipeline.h"
#include "cae/uop.h"
#include "cae/trace.h"
#include "cae/trace-emit.h"
#include "cae/riscv-trace-derive.h"

#include <stdio.h>

static FILE *g_trace_fp;
static bool g_trace_open_failed;
static bool g_close_registered;

static void riscv_trace_flush_and_close(void)
{
    if (g_trace_fp != NULL) {
        fflush(g_trace_fp);
        fclose(g_trace_fp);
        g_trace_fp = NULL;
    }
}

static bool riscv_trace_open_locked(void)
{
    const char *path = cae_trace_get_out_path();
    if (path == NULL) {
        return false;
    }
    if (g_trace_fp != NULL) {
        return true;
    }
    if (g_trace_open_failed) {
        return false;
    }

    g_trace_fp = fopen(path, "wb");
    if (g_trace_fp == NULL) {
        error_report("cae: trace-out: failed to open '%s' (%s)",
                     path, strerror(errno));
        g_trace_open_failed = true;
        return false;
    }

    /*
     * Self-describing header so the consumer (scripts/nemu-difftest.py)
     * can reject incompatible producers before any byte-diff starts.
     * All fields little-endian; reserved is explicit padding.
     */
    CaeTraceHeader hdr = {
        .magic        = CAE_TRACE_MAGIC,
        .version      = CAE_TRACE_VERSION,
        .endianness   = CAE_TRACE_ENDIAN_LITTLE,
        .mode         = CAE_TRACE_MODE_TRACE,
        .record_size  = (uint16_t)sizeof(CaeTraceRecord),
        .reserved     = 0,
        .isa_subset   = CAE_TRACE_ISA_RV64GC,
    };
    if (fwrite(&hdr, sizeof(hdr), 1, g_trace_fp) != 1) {
        error_report("cae: trace-out: short write for header to '%s'", path);
        riscv_trace_flush_and_close();
        g_trace_open_failed = true;
        return false;
    }

    if (!g_close_registered) {
        atexit(riscv_trace_flush_and_close);
        g_close_registered = true;
    }
    return true;
}

/*
 * Extract the destination GPR index from the retiring RISC-V uop.
 * The classifier populates `dst_regs[0]` when a GPR write-back is
 * architecturally visible; num_dst==0 means no write-back (branch,
 * store, fence, ecall/ebreak, etc.).
 *
 * Returns 0xFF when no GPR is written — matches the on-disk rd_idx
 * sentinel.
 */
static uint8_t riscv_trace_rd_idx(const CaeUop *uop)
{
    if (!uop || uop->num_dst == 0) {
        return 0xFFu;
    }
    uint8_t rd = uop->dst_regs[0];
    if (rd == 0 || rd >= 32) {
        /*
         * x0 writes are a no-op architecturally; reporting them as
         * rd_idx=0 would force the consumer to special-case the
         * always-zero read. Collapse to the "no write-back" sentinel.
         */
        return 0xFFu;
    }
    return rd;
}

static void riscv_trace_on_retire(CaeCpu *cpu, const CaeUop *uop)
{
    if (cpu == NULL || uop == NULL) {
        return;
    }
    /*
     * Defensive skip: pc=0 is never a real retired RV PC, and the
     * AC-K-10 pre-exec hook (riscv_cae_prep_for_exec) guarantees
     * the first-slice first retire has a real PC before this point.
     * If pc is still 0 here, some upstream path (e.g. a unit test
     * driving the emitter directly) has bypassed the hook; drop
     * the record rather than poison the trace file.
     */
    if (uop->pc == 0) {
        return;
    }
    if (!riscv_trace_open_locked()) {
        return;
    }

    CPUState *cs = cpu->qemu_cpu;
    if (cs == NULL) {
        return;
    }
    CPURISCVState *env = cpu_env(cs);

    uint8_t rd_idx = riscv_trace_rd_idx(uop);
    uint64_t rd_value = 0;
    /*
     * Round 46 AC-K-2 byte-identity: NEMU's itrace_emit_commit
     * records the store-data register (rs2) in rd_idx / rd_value
     * even though stores write no architectural rd. Match that
     * convention so the binding diff aligns on store records.
     * rs2 sits at insn[24:20] for both the 32-bit SD encoding and
     * the 16-bit compressed stores we classify at the low half.
     * For compressed stores the spec uses rs2' (+8 offset) at
     * bits[4:2] of the 16-bit opcode instead; handle both widths.
     */
    if (rd_idx == 0xFFu && uop->is_store && uop->insn_bytes == 4) {
        uint8_t rs2 = (uop->insn >> 20) & 0x1f;
        if (rs2 != 0 && rs2 < 32) {
            rd_idx = rs2;
        }
    } else if (rd_idx == 0xFFu && uop->is_store &&
               uop->insn_bytes == 2) {
        /*
         * Compressed stores (c.sw / c.sd / c.swsp / c.sdsp /
         * c.fsw*/
        uint16_t ci = (uint16_t)uop->insn;
        uint8_t quadrant = ci & 0x3;
        uint8_t funct3 = (ci >> 13) & 0x7;
        if (quadrant == 0 && (funct3 == 6 || funct3 == 7)) {
            rd_idx = (uint8_t)(((ci >> 2) & 0x7) + 8);
        } else if (quadrant == 2 && (funct3 == 6 || funct3 == 7)) {
            rd_idx = (uint8_t)((ci >> 2) & 0x1f);
            if (rd_idx == 0) {
                rd_idx = 0xFFu;
            }
        }
    }
    if (rd_idx != 0xFFu && rd_idx < 32) {
        rd_value = env->gpr[rd_idx];
    }

    /*
     * Round 7 AC-K-2 fix: when a store retires via the TCG fast
     * path (no softmmu helper), cae_mem_access_notify never fires
     * and uop->mem_size / mem_addr / mem_value remain at 0. This
     * is the common case under `cae_tlb_force_slow_active=false`
     * (OoO mode, AC-K-13). Derive the three fields from the insn
     * encoding + live GPR state so the trace record byte-matches
     * NEMU's emit. Hook-populated values take precedence when the
     * slow path did fire.
     */
    uint16_t store_mem_size = uop->mem_size;
    uint64_t store_mem_addr = uop->mem_addr;
    uint64_t store_mem_value = uop->mem_value;
    if (uop->is_store && store_mem_size == 0) {
        CaeRiscvStoreFields derived;
        if (cae_riscv_trace_derive_store_fields(uop, env->gpr, &derived)) {
            store_mem_size = derived.mem_size;
            store_mem_addr = derived.mem_addr;
            store_mem_value = derived.mem_value;
        }
    }

    uint8_t flags = 0;
    if (uop->is_store && store_mem_size != 0) {
        flags |= CAE_TRACE_FLAG_MEM_WRITE;
    }

    /*
     * AC-K-2.4 — split-store flag. A store crosses a natural
     * alignment boundary when mem_addr is not naturally aligned to
     * mem_size. On RV64 misaligned stores that straddle a
     * page/cache-line boundary are architecturally legal but emit a
     * single retired-insn record on both CAE and NEMU; the flag
     * tells the consumer the mem_value covers the full architectural
     * width even though the underlying access happened in two
     * sub-operations.
     */
    if ((flags & CAE_TRACE_FLAG_MEM_WRITE) && store_mem_size > 1 &&
        (store_mem_addr & (store_mem_size - 1)) != 0) {
        flags |= CAE_TRACE_FLAG_SPLIT;
    }

    /*
     * AC-K-2.4 — SC-fail flag. RISC-V convention: SC writes rd = 0
     * on success, non-zero on failure. The classifier tags SC as
     * CAE_UOP_ATOMIC; rd_idx != 0xFF and the committed GPR value
     * contains the success/fail code. On failure the store bytes
     * do not commit — clear the MEM_WRITE flag so the consumer
     * doesn't byte-compare a non-existent write.
     */
    if (uop->type == CAE_UOP_ATOMIC && rd_idx != 0xFFu && rd_value != 0 &&
        uop->is_store) {
        flags |= CAE_TRACE_FLAG_SCFAIL;
        flags &= ~CAE_TRACE_FLAG_MEM_WRITE;
    }

    /*
     * AC-K-2.4 — architectural-exception flag. On a committed trap
     * the RISC-V CPU latches the cause in env->mcause (M-mode) or
     * env->scause/vscause and sets cs->exception_index to the
     * numeric cause code. We detect the trap-just-committed case
     * via cs->exception_index >= 0 at retire time — the classifier
     * marked the trapping insn CAE_UOP_SYSTEM only when it's a
     * synchronous cause (ecall / ebreak / illegal / access-fault).
     * External interrupts do not show up here because they preempt
     * between instructions; this flag is reserved for the trapping
     * insn itself. Consumers ignore rd_value / mem_value for these
     * records.
     */
    if (cs->exception_index >= 0) {
        flags |= CAE_TRACE_FLAG_EXCEPTION;
    }

    CaeTraceRecord rec = {
        .pc         = uop->pc,
        .opcode     = uop->insn,
        .rd_idx     = rd_idx,
        .flags      = flags,
        .mem_size   = (flags & CAE_TRACE_FLAG_MEM_WRITE) ? store_mem_size : 0,
        .rd_value   = rd_value,
        .mem_addr   = (flags & CAE_TRACE_FLAG_MEM_WRITE) ? store_mem_addr : 0,
        .mem_value  = (flags & CAE_TRACE_FLAG_MEM_WRITE) ? store_mem_value : 0,
    };

    /*
     * Single 40-byte record per retired insn. fwrite's default block
     * buffering gives us an effective ring buffer; a dropped record
     * would be a correctness bug rather than a tolerable data loss,
     * so we fail loudly on short write rather than swallow it.
     */
    if (fwrite(&rec, sizeof(rec), 1, g_trace_fp) != 1) {
        error_report("cae: trace-out: short write for retire record; "
                     "disabling further trace output");
        riscv_trace_flush_and_close();
        g_trace_open_failed = true;
    }
}

static void riscv_trace_on_close(void)
{
    riscv_trace_flush_and_close();
}

static const CaeTraceEmitterOps riscv_trace_emitter_ops = {
    .on_retire = riscv_trace_on_retire,
    .on_close  = riscv_trace_on_close,
};

static void riscv_trace_register_types(void)
{
    cae_trace_register_emitter(&riscv_trace_emitter_ops);
}
type_init(riscv_trace_register_types);
