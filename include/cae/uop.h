/*
 * CAE (Cycle Approximate Engine) - Micro-Operation Definition
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_UOP_H
#define CAE_UOP_H

#include "qemu/queue.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct TranslationBlock TranslationBlock;

/* Instruction type classification */
typedef enum CaeUopType {
    CAE_UOP_ALU,        /* Integer arithmetic/logic */
    CAE_UOP_BRANCH,     /* Branch/jump */
    CAE_UOP_LOAD,       /* Memory load */
    CAE_UOP_STORE,      /* Memory store */
    CAE_UOP_MUL,        /* Multiply */
    CAE_UOP_DIV,        /* Divide */
    CAE_UOP_FPU,        /* Floating point */
    CAE_UOP_SYSTEM,     /* System/CSR */
    CAE_UOP_FENCE,      /* Memory fence */
    CAE_UOP_ATOMIC,     /* Atomic (e.g. AMO/LR/SC) */
    CAE_UOP_UNKNOWN,
} CaeUopType;

/* Functional unit type */
typedef enum CaeFuType {
    CAE_FU_ALU,
    CAE_FU_BRANCH,
    CAE_FU_LOAD,
    CAE_FU_STORE,
    CAE_FU_MUL,
    CAE_FU_DIV,
    CAE_FU_FPU,
    CAE_FU_SYSTEM,
    CAE_FU_NONE,
} CaeFuType;

typedef struct CaeUop {
    uint64_t pc;
    uint32_t insn;
    CaeUopType type;
    CaeFuType fu_type;
    uint8_t latency;

    /* Register operands */
    uint8_t src_regs[4];
    uint8_t dst_regs[2];
    uint8_t num_src;
    uint8_t num_dst;

    /* Memory access info (filled during execution) */
    uint64_t mem_addr;
    /*
     * mem_value is populated by cae_mem_access_notify when the hook
     * carries a store value pointer (op=WRITE) and the access width
     * fits in 8 bytes. Consumers (the per-target retire-side CAE
     * trace writer; AC-K-2.4) read it post-helper return; it is
     * zero for loads, fetches, and split stores.
     */
    uint64_t mem_value;
    uint8_t mem_size;
    bool is_load;
    bool is_store;

    /*
     * Branch info — classifier-populated (decode-time) + hot-path-
     * populated (execution-time) fields.
     *
     * Decode-time (set by the CaeUopClassifier for recognised control
     * flow instructions):
     *   is_branch       true for any PC-altering insn (cond branch, JAL,
     *                   JALR, RVC branches/jumps/calls/returns)
     *   is_conditional  true for direction-predictable branches
     *                   (BEQ/BNE/... / C.BEQZ / C.BNEZ); false for
     *                   unconditional jumps/calls/returns/indirects
     *   is_call         true when the branch writes the return address
     *                   register (JAL with link / JALR with link /
     *                   C.JAL(R) / C.JALR)
     *   is_return       true for the ABI-defined function-return shape
     *                   (JALR rd=x0 rs1=ra / C.JR ra)
     *   is_indirect     true when the target is register-sourced
     *                   (JALR / C.JR / C.JALR)
     *   insn_bytes      2 for RVC, 4 for standard 32-bit encodings
     *
     * Execution-time (derived in HELPER(lookup_tb_ptr) from the
     * observed next PC):
     *   branch_taken    direction the branch actually resolved
     *   branch_target   PC the branch actually landed on
     */
    bool is_branch;
    bool is_conditional;
    bool is_call;
    bool is_return;
    bool is_indirect;
    uint8_t insn_bytes;
    bool branch_taken;
    uint64_t branch_target;

    /*
     * Round 17 drift-recovery: frontend-side prediction
     * cache. Populated by cae_engine_on_frontend_predict()
     * at TB entry (HELPER(lookup_tb_ptr), immediately after
     * classification). Consumed by cae_charge_executed_tb
     * at TB retire for mispredict detection, so predict()
     * fires at TB ENTRY and update() at TB RETIRE — the
     * real producer/consumer separation Codex flagged as
     * missing in rounds 13-16.
     *
     * Zeroed by the classifier reset at lookup_tb_ptr
     * (accel/tcg/cpu-exec.c) along with the other classifier
     * fields. pred_valid=false on the fallback path (no
     * frontend hook fired, e.g. first TB of a cpu_exec
     * slice or a non-DecoupledBPU predictor where the
     * round-17 hook short-circuits) triggers the legacy
     * round-16 retire-side predict path in
     * cae_charge_executed_tb.
     */
    bool pred_valid;
    bool pred_taken;
    bool pred_target_known;
    uint64_t pred_target;

    /*
     * Round 47 AC-K-2 byte-identity: per-uop "already charged"
     * sentinel. Under one_insn_per_tb + CF_NO_GOTO_TB, two
     * charge sites can observe the same persistent
     * cpu->active_uop — HELPER(lookup_tb_ptr) at TB entry and
     * cpu_exec_loop's post-TB site. Without a per-uop gate
     * both sites emit a retire record for the same
     * classification, duplicating the non-branch predecessor
     * and dropping the branch-target TB on every chained
     * backward branch. This flag is cleared inside
     * cae_uop_classify_bytes (via cae_uop_reset_generic) so
     * each fresh classify starts uncharged, and set inside
     * cae_charge_executed_tb after a successful emit. Both
     * charge sites short-circuit when the flag is already
     * true on their current active_uop.
     */
    bool charged;

    TranslationBlock *tb;

    QTAILQ_ENTRY(CaeUop) next;
} CaeUop;

/*
 * Guest-architecture classifier plug-in.
 *
 * Each QEMU system binary hosts at most one guest target, and therefore
 * at most one CAE classifier. Implementations live under target/<arch>/cae/
 * and register themselves via cae_uop_register_classifier() from their
 * type_init() constructor. CAE core code never contains guest ISA details;
 * it dispatches through this interface.
 */
typedef struct CaeUopClassifier {
    /* Largest raw instruction length produced by this target, in bytes. */
    size_t max_insn_bytes;
    /*
     * Fill in the type/fu_type/is_load/is_store/is_branch/src/dst fields
     * of uop based on the raw instruction bytes. Returns true when the
     * encoding was recognised; false leaves uop in the UNKNOWN default
     * populated by the caller.
     */
    bool (*classify)(CaeUop *uop, uint64_t pc,
                     const uint8_t *insn_buf, size_t insn_bytes);
} CaeUopClassifier;

/*
 * Register the guest classifier for the current build. Must be called
 * exactly once, from a target-specific type_init() constructor, before
 * any classification request. Duplicate registrations abort.
 */
void cae_uop_register_classifier(const CaeUopClassifier *classifier);

/* True if a classifier has been registered. */
bool cae_uop_has_classifier(void);

/*
 * Classify an instruction encoded as raw bytes. Zeros the generic uop
 * fields and sets default UNKNOWN / FU_NONE before invoking the
 * classifier. Returns the classifier result, or false when no
 * classifier is registered.
 */
bool cae_uop_classify_bytes(CaeUop *uop, uint64_t pc,
                            const uint8_t *insn_buf, size_t insn_bytes);

/*
 * Helper for fixed-length 32-bit ISAs (notably RV where compressed
 * encodings occupy the low 16 bits). Materialises insn_raw as
 * little-endian bytes and delegates to cae_uop_classify_bytes().
 */
void cae_uop_from_insn(CaeUop *uop, uint64_t pc, uint32_t insn_raw);

/* Default latency (in cycles) associated with a uop type. */
uint8_t cae_uop_default_latency(CaeUopType type);

#endif /* CAE_UOP_H */
