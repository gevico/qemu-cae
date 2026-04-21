/*
 * CAE (Cycle Approximate Engine) - RISC-V tier-2 checkpoint writer
 *
 * Implements the per-target checkpoint emitter vtable declared in
 * include/cae/checkpoint.h. Fires every
 * CAE_TRACE_CHECKPOINT_INTERVAL_DEFAULT retired instructions
 * (1 000 000 by default) and snapshots the guest architectural
 * state into a CaeTraceCheckpointRecord matching
 * include/cae/trace.h.
 *
 * The file is opened lazily on the first interval trigger so a
 * short run that never reaches an interval boundary does not
 * leave an empty header behind. The 16-byte CaeTraceHeader is
 * written before the first record with `mode =
 * CAE_TRACE_MODE_CHECKPOINT`; consumers (nemu-difftest.py
 * --mode checkpoint) use the mode field to branch between
 * record schemas.
 *
 * Field population (RV64GC subset):
 *   - retire_index      — interval index (g_retire_count)
 *   - pc                — current env->pc (the retired-insn PC)
 *   - gpr[32]           — RV64 integer register file
 *   - fpr[32]           — RV64 FP register file (FLEN=64)
 *   - csrs[8]           — frm, fflags, fcsr, mstatus, mepc,
 *                         mcause, mie, priv
 *   - memory_hash       — 0 for v1 (placeholder until
 *                         --full-trace checkpoint lands)
 *   - flags / reserved  — 0 for v1
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
#include "exec/cpu-common.h"
#include "exec/cputlb.h"
#include "exec/translation-block.h"
#include "fpu/softfloat.h"
#include "internals.h"
#include "cae/pipeline.h"
#include "cae/uop.h"
#include "cae/trace.h"
#include "cae/checkpoint.h"
#include "cae/engine.h"
#include "cae/cpu_ooo.h"
#include "cae/bpred.h"
#include "cae/cache_mshr.h"

#include <stdio.h>

/*
 * Width of the guest-memory window hashed into the checkpoint
 * record's memory_hash field (AC-K-2.3). Starts at the tier-1
 * benchmark entry PC (MANIFEST.reset_pc, typically 0x80000000)
 * and spans 1 MiB — small enough to hash on every 1M-retire
 * interval without measurable overhead, large enough that a
 * divergent guest store into the text/rodata region shows up
 * on the very first post-divergence checkpoint.
 *
 * The NEMU-side hash (tools/nemu-itrace.patch:
 * itrace_memory_hash_fnv1a) uses the same start / width / FNV-1a
 * algorithm so the byte-diff is meaningful.
 */
#define CAE_CP_MEM_HASH_BYTES  0x100000ULL

static FILE *g_checkpoint_fp;
static bool g_checkpoint_open_failed;
static bool g_close_registered;

static void riscv_checkpoint_flush_and_close(void)
{
    if (g_checkpoint_fp != NULL) {
        fflush(g_checkpoint_fp);
        fclose(g_checkpoint_fp);
        g_checkpoint_fp = NULL;
    }
}

static bool riscv_checkpoint_open_locked(void)
{
    const char *path = cae_checkpoint_get_out_path();
    if (path == NULL) {
        return false;
    }
    if (g_checkpoint_fp != NULL) {
        return true;
    }
    if (g_checkpoint_open_failed) {
        return false;
    }

    g_checkpoint_fp = fopen(path, "wb");
    if (g_checkpoint_fp == NULL) {
        error_report("cae: checkpoint-out: failed to open '%s' (%s)",
                     path, strerror(errno));
        g_checkpoint_open_failed = true;
        return false;
    }

    CaeTraceHeader hdr = {
        .magic        = CAE_TRACE_MAGIC,
        .version      = CAE_TRACE_VERSION,
        .endianness   = CAE_TRACE_ENDIAN_LITTLE,
        .mode         = CAE_TRACE_MODE_CHECKPOINT,
        .record_size  = (uint16_t)sizeof(CaeTraceCheckpointRecord),
        .reserved     = 0,
        .isa_subset   = CAE_TRACE_ISA_RV64GC,
    };
    if (fwrite(&hdr, sizeof(hdr), 1, g_checkpoint_fp) != 1) {
        error_report("cae: checkpoint-out: short write for header to '%s'",
                     path);
        riscv_checkpoint_flush_and_close();
        g_checkpoint_open_failed = true;
        return false;
    }

    if (!g_close_registered) {
        atexit(riscv_checkpoint_flush_and_close);
        g_close_registered = true;
    }
    return true;
}

/*
 * RV64 CSR snapshot. The eight slots in CaeTraceCheckpointRecord
 * are fixed by the on-disk contract (see
 * CAE_TRACE_CHECKPOINT_CSR_COUNT = 8 in include/cae/trace.h); we
 * map them to the NEMU patch's checkpoint sampler: frm, fflags,
 * fcsr, mstatus, mepc, mcause, mie, priv.
 */
/*
 * FNV-1a 64-bit hash over a bounded guest-memory window. Matches
 * the NEMU-side implementation byte-for-byte on input bytes, so
 * the memory_hash column of the checkpoint byte-diff is
 * contract-equal on a deterministic workload. Reads through the
 * QEMU address-space interface so any guest-to-host mapping is
 * handled transparently (including MMIO regions, which read as
 * 0 via the memory API and therefore hash identically on both
 * sides).
 */
static uint64_t riscv_checkpoint_memory_hash_fnv1a(CPUState *cs,
                                                    uint64_t start,
                                                    uint64_t nbytes)
{
    const uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    const uint64_t FNV_PRIME  = 0x100000001b3ULL;
    uint64_t h = FNV_OFFSET;
    /*
     * Read in chunks to keep per-interval overhead bounded. 4 KiB
     * matches typical page granularity and avoids stack pressure.
     */
    enum { CHUNK = 4096 };
    uint8_t buf[CHUNK];
    for (uint64_t off = 0; off < nbytes; off += CHUNK) {
        uint64_t want = nbytes - off;
        if (want > CHUNK) {
            want = CHUNK;
        }
        if (cpu_memory_rw_debug(cs, start + off, buf, want, false) != 0) {
            /*
             * Address space window unmapped mid-sweep. Treat bytes
             * as zero (matches NEMU's "unmapped reads as 0"
             * convention) and continue so truncation still produces
             * a deterministic hash that NEMU can reproduce.
             */
            memset(buf, 0, want);
        }
        for (uint64_t i = 0; i < want; i++) {
            h ^= (uint64_t)buf[i];
            h *= FNV_PRIME;
        }
    }
    return h;
}

static void riscv_checkpoint_fill_csrs(CPURISCVState *env, uint64_t *csrs)
{
    /*
     * env->fflags is not a C struct member; it's synthesised from
     * float_status by riscv_cpu_get_fflags(). Call the accessor so
     * the checkpoint matches NEMU's fflags / fcsr snapshot.
     */
    target_ulong fflags = riscv_cpu_get_fflags(env);
    csrs[0] = env->frm;
    csrs[1] = fflags;
    csrs[2] = ((uint64_t)env->frm << 5) | fflags;  /* fcsr */
    csrs[3] = env->mstatus;
    csrs[4] = env->mepc;
    csrs[5] = env->mcause;
    csrs[6] = env->mie;
    csrs[7] = env->priv;
}

static void riscv_checkpoint_on_interval(CaeCpu *cpu,
                                          const CaeUop *uop,
                                          uint64_t retire_index)
{
    if (cpu == NULL || cpu->qemu_cpu == NULL) {
        return;
    }
    if (!riscv_checkpoint_open_locked()) {
        return;
    }

    CPUState *cs = cpu->qemu_cpu;
    CPURISCVState *env = cpu_env(cs);

    /*
     * Hash the benchmark's text + early data window so the
     * checkpoint carries a determinism signal beyond the
     * architectural register set. The start address is the RV
     * virt-machine MBASE (0x80000000), which matches
     * MANIFEST.reset_pc for every tier-1 micro and the XS raw-
     * binary link address. The NEMU patch computes the same
     * hash over the same window; a divergent guest store into
     * the hashed region shows up as a mismatch at the first
     * post-divergence checkpoint.
     */
    uint64_t memory_hash = riscv_checkpoint_memory_hash_fnv1a(
        cs, 0x80000000ULL, CAE_CP_MEM_HASH_BYTES);

    /*
     * Round 8 AC-K-2.4: checkpoint `pc` is the retiring
     * instruction's PC, not env->pc. env->pc at this point is
     * the NEXT PC to execute — using it made the checkpoint pc
     * lag one retirement behind the retire_index boundary,
     * which would fail byte-diff against NEMU (whose checkpoint
     * emitter is called at the retire-boundary and also
     * serializes the retiring pc). Falling back to env->pc
     * only when uop is missing preserves the old behaviour as
     * a safety net.
     */
    uint64_t cp_pc = (uop != NULL && uop->pc != 0)
                   ? uop->pc : env->pc;

    CaeTraceCheckpointRecord rec = {
        .retire_index = retire_index,
        .pc           = cp_pc,
        .memory_hash  = memory_hash,
        .flags        = 0,
        .reserved     = 0,
    };
    for (unsigned i = 0; i < CAE_TRACE_CHECKPOINT_GPR_COUNT; i++) {
        rec.gpr[i] = env->gpr[i];
    }
    for (unsigned i = 0; i < CAE_TRACE_CHECKPOINT_FPR_COUNT; i++) {
        rec.fpr[i] = env->fpr[i];
    }
    riscv_checkpoint_fill_csrs(env, rec.csrs);

    if (fwrite(&rec, sizeof(rec), 1, g_checkpoint_fp) != 1) {
        error_report("cae: checkpoint-out: short write for checkpoint "
                     "record; disabling further output");
        riscv_checkpoint_flush_and_close();
        g_checkpoint_open_failed = true;
    }
}

static void riscv_checkpoint_on_close(void)
{
    riscv_checkpoint_flush_and_close();
}

static const CaeCheckpointEmitterOps riscv_checkpoint_emitter_ops = {
    .on_interval = riscv_checkpoint_on_interval,
    .on_close    = riscv_checkpoint_on_close,
};

/* ------------------------------------------------------------------ */
/*  Speculation save/restore (in-memory snapshot path)                */
/* ------------------------------------------------------------------ */

/*
 * Architectural lane the snapshot captures. Plan AC-K-4
 * (plan.md:89) asks for GPR / FPR / PC / priv / key CSRs /
 * AMO load_res-load_val / predictor history / RAS / rename /
 * free-list / LSQ / MSHR. Round 25 landed GPR + PC + priv;
 * round 26 extends to the full RV architectural lane that lives
 * in CPURISCVState: FPR, FP CSRs (frm + fflags, from which
 * fcsr is rederivable), the m-mode CSRs the tier-2 emitter
 * already snapshots (mstatus / mie / mepc / mcause), and the
 * AMO reservation pair (load_res / load_val) so wrong-path
 * LR/SC rollback is byte-faithful. CAE-side engine / OoO state
 * (bpred_pending, ROB / IQ / LSQ / RAT / sbuffer / FTQ / FSQ /
 * predictor history / RAS / MSHR) is the round-27 extension;
 * this struct carries only RV architectural fields so the
 * arch-neutral layer can treat it as an opaque handle.
 */
struct CaeCheckpointSnapshot {
    /* RV architectural lane (CPURISCVState-resident). */
    uint64_t gpr[32];
    uint64_t fpr[32];
    uint64_t pc;
    int priv;
    /*
     * virt_enabled is part of the privilege-level lane: a
     * speculative misprediction that entered hypervisor-guest
     * mode must unwind it on squash. Restored via
     * riscv_cpu_set_mode() alongside priv.
     */
    bool virt_enabled;
    target_ulong frm;
    target_ulong fflags;
    uint64_t mstatus;
    uint64_t mie;
    target_ulong mepc;
    target_ulong mcause;
    target_ulong load_res;
    target_ulong load_val;

    /*
     * CAE-side sub-blobs. Each lives in its owning module's
     * public API (include/cae/pipeline.h for the CaeCpu lane,
     * include/cae/cpu_ooo.h for the OoO scalar lane). The
     * target-side snapshot composes them as opaque pointers so
     * the per-module internal representation stays
     * encapsulated. Future rounds will add more sub-blobs
     * (DecoupledBPU FTQ/FSQ state, MSHR outstanding ring, etc.)
     * alongside these two.
     */
    CaeCpuSpecSnapshot *cpu_spec;
    CaeOooSpecSnapshot *ooo_spec;
    /*
     * Round 29: two more opaque sub-blobs for the predictor
     * family (FTQ/FSQ + inner TAGE history/RAS via the shared
     * cae_bpred_spec_snapshot_* dispatch) and for the MSHR
     * outstanding-miss ring (completion_cycles + n_entries).
     * Each sub-blob is still managed by its owning module; the
     * target-side struct composes opaque pointers only.
     */
    CaeBPredSpecSnapshot *bpred_spec;
    Object *bpred_obj;    /* captured at save for drop routing */
    CaeMshrSpecSnapshot *mshr_spec;
};

static CaeCheckpointSnapshot *riscv_spec_snapshot(CaeCpu *cpu)
{
    if (cpu == NULL || cpu->qemu_cpu == NULL) {
        return NULL;
    }
    CPURISCVState *env = cpu_env(cpu->qemu_cpu);
    CaeCheckpointSnapshot *snap = g_new0(CaeCheckpointSnapshot, 1);
    for (unsigned i = 0; i < 32; i++) {
        snap->gpr[i] = env->gpr[i];
        snap->fpr[i] = env->fpr[i];
    }
    snap->pc = env->pc;
    snap->priv = env->priv;
    snap->virt_enabled = env->virt_enabled;
    snap->frm = env->frm;
    /*
     * env->fflags is synthesised from float_status by
     * riscv_cpu_get_fflags(). Matching the tier-2 emitter's
     * accessor keeps the save path consistent across both
     * checkpoint lanes.
     */
    snap->fflags = riscv_cpu_get_fflags(env);
    snap->mstatus = env->mstatus;
    snap->mie = env->mie;
    snap->mepc = env->mepc;
    snap->mcause = env->mcause;
    snap->load_res = env->load_res;
    snap->load_val = env->load_val;

    /*
     * Compose the CAE-side sub-blobs. Each save is NULL-safe and
     * owns its internal representation — the target-side struct
     * stores only opaque pointers. cpu_spec is populated when a
     * CaeCpu is attached; ooo_spec is populated only when the
     * engine's cpu_model is a cae-cpu-ooo instance (the
     * save helper returns NULL for in-order / cpi1 paths).
     */
    snap->cpu_spec = cae_cpu_spec_snapshot_save(cpu);
    CaeEngine *engine = cpu->engine;
    if (engine != NULL) {
        snap->ooo_spec =
            cae_cpu_ooo_spec_snapshot_save(engine->cpu_model);
        snap->bpred_spec = cae_bpred_spec_snapshot_save(engine->bpred);
        snap->bpred_obj = engine->bpred;
        /*
         * MSHR lives behind the mem_backend link — the engine
         * knows about the top-level backend Object, and the
         * mshr-spec save helper's dynamic-cast gate makes a safe
         * no-op return when the backend is not a cae-cache-mshr
         * (e.g., memory-model=stub or memory-model=l1-dram).
         */
        snap->mshr_spec =
            cae_mshr_spec_snapshot_save(engine->mem_backend);
    }
    return snap;
}

/*
 * Rebuild the softfloat rounding mode from env->frm after the
 * architectural frm field has been restored. Matches the mapping
 * in target/riscv/fpu_helper.c::helper_set_rounding_mode() so
 * any subsequent FP instruction on the restored snapshot
 * executes with the correct rounding. Invalid frm values on the
 * snapshot (shouldn't occur — sampled from a valid state) leave
 * the current fp_status rounding mode untouched.
 */
static void riscv_spec_restore_fp_rounding(CPURISCVState *env)
{
    int softrm;
    switch (env->frm) {
    case RISCV_FRM_RNE:
        softrm = float_round_nearest_even;
        break;
    case RISCV_FRM_RTZ:
        softrm = float_round_to_zero;
        break;
    case RISCV_FRM_RDN:
        softrm = float_round_down;
        break;
    case RISCV_FRM_RUP:
        softrm = float_round_up;
        break;
    case RISCV_FRM_RMM:
        softrm = float_round_ties_away;
        break;
    default:
        return;
    }
    set_float_rounding_mode(softrm, &env->fp_status);
}

static void riscv_spec_restore(CaeCpu *cpu,
                               const CaeCheckpointSnapshot *snap)
{
    if (cpu == NULL || cpu->qemu_cpu == NULL || snap == NULL) {
        return;
    }
    /*
     * Restore the CAE-side sub-blobs FIRST so the retire path
     * (which is what will run next after a live restore in the
     * future round-32 wiring) sees the unwound predict/OoO state
     * before the RV architectural lane is touched. Each helper is
     * NULL-safe; only the ones that the save-time path populated
     * will actually write anything back.
     */
    cae_cpu_spec_snapshot_restore(cpu, snap->cpu_spec);
    if (cpu->engine != NULL) {
        cae_cpu_ooo_spec_snapshot_restore(cpu->engine->cpu_model,
                                          snap->ooo_spec);
        cae_bpred_spec_snapshot_restore(cpu->engine->bpred,
                                        snap->bpred_spec);
        cae_mshr_spec_snapshot_restore(cpu->engine->mem_backend,
                                       snap->mshr_spec);
    }

    CPURISCVState *env = cpu_env(cpu->qemu_cpu);

    /* Pure architectural data — no derived state involved. */
    for (unsigned i = 0; i < 32; i++) {
        env->gpr[i] = snap->gpr[i];
        env->fpr[i] = snap->fpr[i];
    }
    env->pc = snap->pc;
    env->mepc = snap->mepc;
    env->mcause = snap->mcause;
    env->mie = snap->mie;

    /*
     * mstatus carries derived-state dependencies that the
     * canonical write_mstatus() (target/riscv/csr.c:1596)
     * handles: MXR changes trigger a TLB flush, and the trailing
     * riscv_cpu_update_mask() refresh is mandatory after a
     * mstatus rewrite. write_mstatus() is static to csr.c, so we
     * replicate the same side-effects here. We do not replay
     * write_mstatus()'s WARL legalisation because the snapshot
     * was sampled from an already-legal state.
     */
    uint64_t old_mstatus = env->mstatus;
    env->mstatus = snap->mstatus;
    if ((old_mstatus ^ env->mstatus) & MSTATUS_MXR) {
        tlb_flush(env_cpu(env));
    }

    /*
     * FP rounding/flag lanes. fflags goes through the canonical
     * accessor (riscv_cpu_set_fflags(), the same one the tier-2
     * emitter samples from). frm is a direct architectural
     * write; the derived softfloat rounding mode must be
     * rebuilt separately via riscv_spec_restore_fp_rounding().
     */
    env->frm = snap->frm;
    riscv_spec_restore_fp_rounding(env);
    riscv_cpu_set_fflags(env, snap->fflags);

    /*
     * Privilege-level lane goes LAST among the state-changing
     * writes. riscv_cpu_set_mode() (target/riscv/cpu_helper.c
     * :774) recomputes xl, refreshes env->mask (via
     * riscv_cpu_update_mask again), swaps hypervisor-mode
     * state on a virt_enabled transition (including a TLB
     * flush when RVH is present), and — critically —
     * unconditionally clears env->load_res to -1 as part of the
     * reservation-yielding contract documented at cpu_helper.c
     * :791-799. We therefore restore load_res / load_val AFTER
     * this call so the snapshot's LR/SC reservation pair
     * survives the mode transition.
     */
    riscv_cpu_set_mode(env, snap->priv, snap->virt_enabled);
    env->load_res = snap->load_res;
    env->load_val = snap->load_val;
}

/*
 * Round 31 live-path restore: unwinds ONLY the CAE-side
 * sub-blobs (predict cache, OoO scalars + ROB/IQ/LSQ/RAT/sbuffer,
 * bpred history/RAS, MSHR outstanding ring). The RV functional
 * lane (GPR/FPR/PC/priv/mstatus/mie/mepc/mcause/frm/fflags/
 * load_res/load_val) is intentionally NOT written back — QEMU TCG
 * does not execute the wrong path, so those fields already hold
 * the architecturally-correct post-branch values. Writing them
 * back to save-point values would corrupt JAL link registers and
 * env->pc's natural advance into the actual target.
 *
 * The full `riscv_spec_restore` path remains available via the
 * `.restore` vtable slot for offline / unit-test uses; the live
 * engine dispatcher routes through this `.live_restore` slot for
 * the in-flight speculation resolve.
 */
static void riscv_spec_live_restore(CaeCpu *cpu,
                                    const CaeCheckpointSnapshot *snap)
{
    if (cpu == NULL || snap == NULL) {
        return;
    }
    /*
     * Round 32 bug #1 fix: the CaeCpu lane (bpred_pending_resolve +
     * bpred_pending_valid + active_uop predict cache) captures
     * state AT SAVE TIME (TB entry). By the time live_restore
     * fires (branch retire), the engine has already drained the
     * previous pending_resolve and staged the CURRENT branch's
     * resolve into those fields. Restoring the save-time CaeCpu
     * lane would stomp the current staging and resurrect the
     * previous (already-drained) pending, causing a double-
     * process of the previous update and loss of the current
     * branch's update. So DO NOT call cae_cpu_spec_snapshot_
     * restore in the live path. The offline
     * cae_checkpoint_restore keeps the full restore for
     * regression / replay use cases where the CaeCpu lane needs
     * to round-trip.
     *
     * The OoO / bpred / MSHR sub-blobs remain restored — those
     * DID accrue speculative side effects (TAGE history was
     * mutated at predict time, speculative stores may have
     * entered the sbuffer, MSHR completion cycles were queued)
     * and must rewind to the save-point.
     */
    if (cpu->engine != NULL) {
        cae_cpu_ooo_spec_snapshot_restore(cpu->engine->cpu_model,
                                          snap->ooo_spec);
        cae_bpred_spec_snapshot_restore(cpu->engine->bpred,
                                        snap->bpred_spec);
        cae_mshr_spec_snapshot_restore(cpu->engine->mem_backend,
                                       snap->mshr_spec);
    }
}

static void riscv_spec_drop(CaeCheckpointSnapshot *snap)
{
    if (snap == NULL) {
        return;
    }
    /*
     * Each sub-blob owns its own allocation; drop through the
     * matching owning-module helper so the struct never leaks a
     * sub-blob even if the save-time path only populated one of
     * the two.
     */
    cae_cpu_spec_snapshot_drop(snap->cpu_spec);
    cae_cpu_ooo_spec_snapshot_drop(snap->ooo_spec);
    cae_bpred_spec_snapshot_drop(snap->bpred_obj, snap->bpred_spec);
    cae_mshr_spec_snapshot_drop(snap->mshr_spec);
    g_free(snap);
}

static const CaeSpecCheckpointOps riscv_spec_checkpoint_ops = {
    .snapshot     = riscv_spec_snapshot,
    .restore      = riscv_spec_restore,
    .live_restore = riscv_spec_live_restore,
    .drop         = riscv_spec_drop,
};

static void riscv_checkpoint_register_types(void)
{
    cae_checkpoint_register_emitter(&riscv_checkpoint_emitter_ops);
    cae_spec_checkpoint_register_ops(&riscv_spec_checkpoint_ops);
}
type_init(riscv_checkpoint_register_types);
