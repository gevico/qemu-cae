/*
 * CAE retire-boundary trace emitter dispatch (arch-neutral).
 *
 * The per-target emitter registers through cae_trace_register_emitter().
 * The accel init path wires the `trace-out` property through
 * cae_trace_set_out_path(). The engine retire path calls
 * cae_trace_notify_retire() once per retired architectural instruction.
 *
 * Neither end of the dispatch surface depends on guest-ISA details;
 * this file stays arch-neutral (it never reads guest registers).
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cae/trace-emit.h"

static const CaeTraceEmitterOps *g_emitter;
static char *g_trace_out_path;

void cae_trace_register_emitter(const CaeTraceEmitterOps *ops)
{
    /*
     * A per-binary target drives CAE; duplicate emitter registration
     * would hide a per-arch wiring bug. Abort hard rather than mask it.
     */
    assert(ops != NULL);
    assert(g_emitter == NULL || g_emitter == ops);
    g_emitter = ops;
}

bool cae_trace_set_out_path(const char *path, Error **errp)
{
    if (path == NULL || path[0] == '\0') {
        error_setg(errp, "cae: trace-out path must be non-empty");
        return false;
    }

    if (g_trace_out_path != NULL) {
        if (strcmp(g_trace_out_path, path) == 0) {
            return true;
        }
        error_setg(errp,
                   "cae: trace-out already set to '%s', "
                   "cannot change to '%s' mid-run",
                   g_trace_out_path, path);
        return false;
    }

    g_trace_out_path = g_strdup(path);
    return true;
}

const char *cae_trace_get_out_path(void)
{
    return g_trace_out_path;
}

void cae_trace_notify_retire(CaeCpu *cpu, const CaeUop *uop)
{
    /*
     * Hot-path guard: paying one null-pointer compare when tracing is
     * off is the intended cost; the engine calls this unconditionally
     * at retire. The check order (path first) minimizes work in the
     * common case: no path set -> skip the deref through the ops
     * pointer entirely.
     */
    if (g_trace_out_path == NULL) {
        return;
    }
    if (g_emitter == NULL || g_emitter->on_retire == NULL) {
        return;
    }
    g_emitter->on_retire(cpu, uop);
}

void cae_trace_close(void)
{
    if (g_emitter != NULL && g_emitter->on_close != NULL) {
        g_emitter->on_close();
    }
}
