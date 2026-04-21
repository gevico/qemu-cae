/*
 * CAE retire-boundary trace emitter dispatch (arch-neutral).
 *
 * The on-disk format lives in include/cae/trace.h. This header
 * declares the small dispatch surface that the arch-neutral engine
 * uses to reach the per-target emitter. The engine does not know
 * about guest register files; at retire it hands the CaeCpu + CaeUop
 * to whatever emitter was registered at accel init, and the
 * per-target emitter reads guest state (GPRs, CSRs, etc.) from
 * cpu->qemu_cpu.
 *
 * Exactly zero or one emitter may be registered per QEMU binary —
 * duplicate registrations abort the process. A NULL registration
 * after one was installed is invalid and asserts.
 *
 * When no trace-out path is set the notify path is a no-op, so
 * the hot charge path pays only a NULL-pointer compare when tracing
 * is disabled (the default).
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef CAE_TRACE_EMIT_H
#define CAE_TRACE_EMIT_H

#include <stdbool.h>
#include "qapi/error.h"

typedef struct CaeCpu CaeCpu;
typedef struct CaeUop CaeUop;

/*
 * Emitter vtable. `on_retire` is called exactly once per retired
 * architectural instruction after the engine's counters have moved.
 * `on_close` is called at cae_trace_close() (atexit / reset-path) so
 * the emitter can flush ring buffers and close the backing FILE*.
 *
 * Both callbacks may be invoked from the vCPU thread; implementations
 * must not take any global lock that the retire path doesn't already
 * hold.
 */
typedef struct CaeTraceEmitterOps {
    void (*on_retire)(CaeCpu *cpu, const CaeUop *uop);
    void (*on_close)(void);
} CaeTraceEmitterOps;

/*
 * Target-side emitter registration. Called from the per-target
 * type_init() constructor in target/<arch>/cae/. The pointer must
 * remain valid for the lifetime of the process.
 */
void cae_trace_register_emitter(const CaeTraceEmitterOps *ops);

/*
 * Set the output file path from the accel `trace-out=<path>`
 * property. Returns true on success; returns false and sets errp
 * when the path is already set to a different value (callers treat
 * the first-set path as authoritative; later sets on the same run
 * are ignored when identical and rejected otherwise).
 */
bool cae_trace_set_out_path(const char *path, Error **errp);

/* Current output path or NULL when tracing is disabled. */
const char *cae_trace_get_out_path(void);

/*
 * Hot-path retire hook. Arch-neutral callers (cae/engine.c) invoke
 * this from cae_charge_executed_tb after the cycle/insn counters
 * advance. No-op when no emitter is registered or no out path is
 * configured. Safe to call from any context where `cpu` and `uop`
 * are live — in practice only the engine's retire path.
 */
void cae_trace_notify_retire(CaeCpu *cpu, const CaeUop *uop);

/*
 * Flush + close the backing file. Registered via atexit() the first
 * time `on_retire` opens the file, so callers normally do not invoke
 * this directly; exposed for tests that want to force a flush
 * mid-run.
 */
void cae_trace_close(void);

#endif /* CAE_TRACE_EMIT_H */
