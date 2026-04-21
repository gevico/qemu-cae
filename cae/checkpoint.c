/*
 * CAE tier-2 checkpoint emitter dispatch (arch-neutral).
 *
 * Owns the retire counter + interval trigger. The per-target
 * emitter (target/<arch>/cae/cae-<arch>-checkpoint.c) provides
 * the snapshot writer through cae_checkpoint_register_emitter().
 * The accel init path wires the `checkpoint-out` property
 * through cae_checkpoint_set_out_path(). The engine retire path
 * calls cae_checkpoint_notify_retire() on every retired insn;
 * interval firing + dispatch + file management all live here.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cae/trace.h"
#include "cae/checkpoint.h"

static const CaeCheckpointEmitterOps *g_emitter;
static char *g_checkpoint_out_path;
static uint64_t g_retire_count;
static uint64_t g_interval = CAE_TRACE_CHECKPOINT_INTERVAL_DEFAULT;

void cae_checkpoint_register_emitter(const CaeCheckpointEmitterOps *ops)
{
    assert(ops != NULL);
    assert(g_emitter == NULL || g_emitter == ops);
    g_emitter = ops;
}

bool cae_checkpoint_set_out_path(const char *path, Error **errp)
{
    if (path == NULL || path[0] == '\0') {
        error_setg(errp, "cae: checkpoint-out path must be non-empty");
        return false;
    }
    if (g_checkpoint_out_path != NULL) {
        if (strcmp(g_checkpoint_out_path, path) == 0) {
            return true;
        }
        error_setg(errp,
                   "cae: checkpoint-out already set to '%s', "
                   "cannot change to '%s' mid-run",
                   g_checkpoint_out_path, path);
        return false;
    }
    g_checkpoint_out_path = g_strdup(path);
    return true;
}

const char *cae_checkpoint_get_out_path(void)
{
    return g_checkpoint_out_path;
}

void cae_checkpoint_set_interval(uint64_t interval)
{
    if (interval == 0) {
        interval = CAE_TRACE_CHECKPOINT_INTERVAL_DEFAULT;
    }
    g_interval = interval;
}

void cae_checkpoint_notify_retire(CaeCpu *cpu, const CaeUop *uop)
{
    if (g_checkpoint_out_path == NULL) {
        return;
    }
    if (g_emitter == NULL || g_emitter->on_interval == NULL) {
        return;
    }
    /*
     * Retire index is 1-based for the consumer-facing
     * `retire_index` field in CaeTraceCheckpointRecord: the
     * checkpoint at N covers the first N retired insns. The
     * trigger fires on the retire that brings the count to a
     * multiple of `g_interval` (retire index = N). Zero is
     * never valid as an interval boundary, so the guard on
     * `g_retire_count > 0` skips the pre-retire state.
     *
     * Round 8: `uop` is forwarded to the emitter so it can
     * serialize the CHECKPOINT boundary's retiring PC rather
     * than env->pc (which is the NEXT PC to execute, off by
     * one retirement — Codex round-7 gap 2).
     */
    g_retire_count++;
    if ((g_retire_count % g_interval) != 0) {
        return;
    }
    g_emitter->on_interval(cpu, uop, g_retire_count);
}

void cae_checkpoint_close(void)
{
    if (g_emitter != NULL && g_emitter->on_close != NULL) {
        g_emitter->on_close();
    }
}

/* ------------------------------------------------------------------ */
/*  Speculation save/restore substrate                                */
/* ------------------------------------------------------------------ */

/*
 * Speculation save/restore vtable. Orthogonal to the tier-2 TRACE
 * emitter above: this path is hit from the (future) M4' speculation
 * code at branch-resolve / squash time to snapshot and restore
 * architectural state in memory. Registered with NULL to clear
 * (used by unit-test teardown so one test's registration does not
 * leak to the next).
 */
static const CaeSpecCheckpointOps *g_spec_ops;

void cae_spec_checkpoint_register_ops(const CaeSpecCheckpointOps *ops)
{
    g_spec_ops = ops;
}

CaeCheckpointSnapshot *cae_checkpoint_save(CaeCpu *cpu)
{
    if (g_spec_ops == NULL || g_spec_ops->snapshot == NULL) {
        return NULL;
    }
    return g_spec_ops->snapshot(cpu);
}

void cae_checkpoint_restore(CaeCpu *cpu,
                            const CaeCheckpointSnapshot *snap)
{
    if (g_spec_ops == NULL || g_spec_ops->restore == NULL || snap == NULL) {
        return;
    }
    g_spec_ops->restore(cpu, snap);
}

void cae_checkpoint_live_restore(CaeCpu *cpu,
                                 const CaeCheckpointSnapshot *snap)
{
    if (g_spec_ops == NULL || snap == NULL) {
        return;
    }
    if (g_spec_ops->live_restore != NULL) {
        g_spec_ops->live_restore(cpu, snap);
        return;
    }
    /*
     * Fallback: no live_restore method registered. Call the full
     * restore. This is correct for test harnesses whose restore is
     * a pure data copy (no QEMU env side effects). For the real
     * RISC-V emitter, a live_restore method MUST be provided so
     * the live path does not corrupt env->pc / GPR / CSRs.
     */
    if (g_spec_ops->restore != NULL) {
        g_spec_ops->restore(cpu, snap);
    }
}

void cae_checkpoint_drop(CaeCheckpointSnapshot *snap)
{
    if (snap == NULL) {
        return;
    }
    if (g_spec_ops != NULL && g_spec_ops->drop != NULL) {
        g_spec_ops->drop(snap);
    }
}
