/*
 * CAE (Cycle Approximate Engine) - Micro-Operation Handling
 *
 * Guest-architecture-neutral CaeUop glue: default latency table,
 * classifier registry, and the generic classify-by-bytes dispatcher.
 * All guest ISA decoding lives in target/<arch>/cae/ and registers
 * itself through cae_uop_register_classifier().
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "cae/uop.h"

/*
 * Default latency table (CPI approximation).
 * Phase-1 uses fixed CPI=1 for most instructions; later milestones
 * override per-uop latency based on microarchitecture models.
 */
static const uint8_t default_latency_table[] = {
    [CAE_UOP_ALU]     = 1,
    [CAE_UOP_BRANCH]  = 1,
    [CAE_UOP_LOAD]    = 1,
    [CAE_UOP_STORE]   = 1,
    [CAE_UOP_MUL]     = 3,
    [CAE_UOP_DIV]     = 20,
    [CAE_UOP_FPU]     = 3,
    [CAE_UOP_SYSTEM]  = 1,
    [CAE_UOP_FENCE]   = 1,
    [CAE_UOP_ATOMIC]  = 1,
    [CAE_UOP_UNKNOWN] = 1,
};

uint8_t cae_uop_default_latency(CaeUopType type)
{
    if (type <= CAE_UOP_UNKNOWN) {
        return default_latency_table[type];
    }
    return 1;
}

/*
 * Registered guest classifier. Written once from the target's
 * type_init() constructor and read from the vCPU thread; no lock is
 * required because publication happens strictly before any dispatch.
 */
static const CaeUopClassifier *registered_classifier;

void cae_uop_register_classifier(const CaeUopClassifier *classifier)
{
    g_assert(classifier != NULL);
    g_assert(classifier->classify != NULL);

    if (registered_classifier == classifier) {
        /* Idempotent re-registration of the same descriptor is allowed. */
        return;
    }

    g_assert(registered_classifier == NULL);
    registered_classifier = classifier;
}

bool cae_uop_has_classifier(void)
{
    return registered_classifier != NULL;
}

static void cae_uop_reset_generic(CaeUop *uop, uint64_t pc)
{
    uop->pc = pc;
    uop->insn = 0;
    uop->type = CAE_UOP_UNKNOWN;
    uop->fu_type = CAE_FU_NONE;
    uop->latency = cae_uop_default_latency(CAE_UOP_UNKNOWN);
    uop->num_src = 0;
    uop->num_dst = 0;
    uop->mem_addr = 0;
    uop->mem_size = 0;
    uop->is_load = false;
    uop->is_store = false;
    uop->is_branch = false;
    uop->is_conditional = false;
    uop->is_call = false;
    uop->is_return = false;
    uop->is_indirect = false;
    uop->insn_bytes = 0;
    uop->branch_taken = false;
    uop->branch_target = 0;
    /*
     * Round 47: a fresh classification starts uncharged. Both
     * charge sites short-circuit when `charged` is true, so
     * leaving stale true here would silence the retire record.
     */
    uop->charged = false;
}

bool cae_uop_classify_bytes(CaeUop *uop, uint64_t pc,
                            const uint8_t *insn_buf, size_t insn_bytes)
{
    const CaeUopClassifier *cls = registered_classifier;

    cae_uop_reset_generic(uop, pc);

    if (!cls) {
        return false;
    }

    if (insn_bytes > cls->max_insn_bytes) {
        insn_bytes = cls->max_insn_bytes;
    }

    return cls->classify(uop, pc, insn_buf, insn_bytes);
}

void cae_uop_from_insn(CaeUop *uop, uint64_t pc, uint32_t insn_raw)
{
    uint8_t buf[4];

    buf[0] = (uint8_t)(insn_raw);
    buf[1] = (uint8_t)(insn_raw >> 8);
    buf[2] = (uint8_t)(insn_raw >> 16);
    buf[3] = (uint8_t)(insn_raw >> 24);

    (void)cae_uop_classify_bytes(uop, pc, buf, sizeof(buf));

    /* Always reflect the raw 32-bit encoding, even on classifier miss. */
    uop->insn = insn_raw;
}
