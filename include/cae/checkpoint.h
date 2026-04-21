/*
 * CAE tier-2 checkpoint emitter dispatch (arch-neutral).
 *
 * Tier-2 benchmarks (ready-to-run workloads) produce periodic
 * architectural-state checkpoints every
 * `CAE_TRACE_CHECKPOINT_INTERVAL_DEFAULT` retired instructions
 * (default 1 000 000 per include/cae/trace.h) rather than a per-
 * retire byte trace. The on-disk record layout is shared with
 * the tier-1 emitter via `CaeTraceHeader` (mode=CHECKPOINT) +
 * `CaeTraceCheckpointRecord`.
 *
 * This header declares the small dispatch surface the arch-
 * neutral engine uses to reach the per-target checkpoint writer.
 * The per-target emitter (target/<arch>/cae/cae-<arch>-
 * checkpoint.c) reads the architectural register file + CSR
 * snapshot; the arch-neutral layer owns the retire counter and
 * interval trigger.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_CHECKPOINT_H
#define CAE_CHECKPOINT_H

#include <stdbool.h>
#include "qapi/error.h"

typedef struct CaeCpu CaeCpu;
typedef struct CaeUop CaeUop;

/*
 * Checkpoint emitter vtable. `on_interval` fires when the
 * arch-neutral retire counter wraps the configured interval
 * (every N retires). The callback is expected to snapshot the
 * guest register/CSR state from `cpu->qemu_cpu` and write one
 * CaeTraceCheckpointRecord to the output file. `on_close`
 * flushes + closes the backing FILE* at atexit.
 */
typedef struct CaeCheckpointEmitterOps {
    /*
     * Fires when the arch-neutral retire counter wraps the
     * configured interval (every N retires). `uop` is the
     * RETIRING uop (the one whose retirement just triggered
     * the interval boundary); the emitter uses it to serialize
     * the checkpoint `pc` field at the correct retire boundary.
     * Round 7 passed only (cpu, retire_index) which forced the
     * emitter to read env->pc — but env->pc at this point is
     * the NEXT PC to execute, so the checkpoint pc was off by
     * one retirement (Codex round-7 gap 2). Forwarding uop
     * closes that.
     */
    void (*on_interval)(CaeCpu *cpu, const CaeUop *uop,
                        uint64_t retire_index);
    void (*on_close)(void);
} CaeCheckpointEmitterOps;

/*
 * Target-side emitter registration. Called from the per-target
 * type_init (target/<arch>/cae/cae-<arch>-checkpoint.c). The
 * pointer must remain valid for the lifetime of the process.
 */
void cae_checkpoint_register_emitter(const CaeCheckpointEmitterOps *ops);

/*
 * Set the output file path from the accel
 * `checkpoint-out=<path>` property. Mirrors
 * cae_trace_set_out_path's set-once semantics (a second
 * different value is rejected).
 */
bool cae_checkpoint_set_out_path(const char *path, Error **errp);

/* Current checkpoint output path or NULL when disabled. */
const char *cae_checkpoint_get_out_path(void);

/*
 * Override the retire interval. Must be called before the first
 * cae_checkpoint_notify_retire(). Default is
 * CAE_TRACE_CHECKPOINT_INTERVAL_DEFAULT.
 */
void cae_checkpoint_set_interval(uint64_t interval);

/*
 * Hot-path retire hook. Arch-neutral callers (cae/engine.c)
 * invoke this from cae_charge_executed_tb after the per-retire
 * counter advances. The internal retire counter is incremented
 * and the emitter callback is invoked on interval boundaries.
 * No-op when no emitter is registered or no output path is
 * configured.
 */
void cae_checkpoint_notify_retire(CaeCpu *cpu, const CaeUop *uop);

/* Flush + close the backing file (called from atexit). */
void cae_checkpoint_close(void);

/* ------------------------------------------------------------------ */
/*  Speculation save/restore substrate (round 25+)                    */
/* ------------------------------------------------------------------ */

/*
 * In-memory architectural-state snapshot used by the M4' TCG
 * predicted-path execution + restore contract. Distinct from the
 * tier-2 TRACE emitter above: that writes periodic file records;
 * this path captures a live snapshot at speculation entry and
 * restores it on squash. The struct layout is target-private; the
 * arch-neutral layer treats this as an opaque handle allocated
 * and freed by the target emitter.
 */
typedef struct CaeCheckpointSnapshot CaeCheckpointSnapshot;

/*
 * Target-side speculation save/restore vtable. Called from
 * cae_checkpoint_save / cae_checkpoint_restore / cae_checkpoint_drop
 * when a target has registered. save() allocates the snapshot from
 * the heap and copies the architectural state (GPR/FPR/PC/priv/...).
 * restore() copies it back. drop() frees the snapshot.
 */
typedef struct CaeSpecCheckpointOps {
    CaeCheckpointSnapshot *(*snapshot)(CaeCpu *cpu);
    void (*restore)(CaeCpu *cpu, const CaeCheckpointSnapshot *snap);
    void (*drop)(CaeCheckpointSnapshot *snap);

    /*
     * Round 31 live-path restore (optional). Rewinds ONLY the
     * CAE-side sub-blobs (cpu_spec predict cache + OoO scalars
     * + ROB/IQ/LSQ/RAT/sbuffer + bpred history/RAS + MSHR
     * outstanding ring). The RV functional lane (GPR/FPR/PC/
     * priv/CSRs/AMO) is intentionally skipped — QEMU TCG never
     * executes the wrong path, so those fields already hold the
     * correct post-branch values. Writing them back to
     * save-point would corrupt JAL link registers and env->pc.
     *
     * If `live_restore` is NULL the arch-neutral dispatch falls
     * back to `restore`; that fallback is wrong for real
     * QEMU-TCG integration and exists only so minimal test
     * targets that don't carry RV functional lanes (unit test
     * harnesses) keep working.
     */
    void (*live_restore)(CaeCpu *cpu,
                         const CaeCheckpointSnapshot *snap);
} CaeSpecCheckpointOps;

/*
 * Target-side registration; symmetric with
 * cae_checkpoint_register_emitter(). The pointer must remain
 * valid for the lifetime of the process. NULL clears the
 * registration (e.g., for test teardown); the public API then
 * degrades to safe no-ops (save returns NULL, restore / drop
 * do nothing) so legacy callers without a target keep working.
 */
void cae_spec_checkpoint_register_ops(const CaeSpecCheckpointOps *ops);

/*
 * Arch-neutral dispatch. Callers (M4' speculation code paths)
 * invoke these from the live engine; the target emitter owns the
 * real register copy. Save returns NULL when no target is
 * registered.
 */
CaeCheckpointSnapshot *cae_checkpoint_save(CaeCpu *cpu);
void cae_checkpoint_restore(CaeCpu *cpu,
                            const CaeCheckpointSnapshot *snap);
void cae_checkpoint_drop(CaeCheckpointSnapshot *snap);

/*
 * Live-path restore variant. Unwinds CAE-side sub-blobs only;
 * skips the RV functional lane so QEMU TCG's post-branch state
 * is not corrupted. Falls back to the full `restore` path when
 * no `live_restore` vtable method is registered. NULL-safe.
 */
void cae_checkpoint_live_restore(CaeCpu *cpu,
                                 const CaeCheckpointSnapshot *snap);

#endif /* CAE_CHECKPOINT_H */
