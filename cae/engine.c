/*
 * CAE (Cycle Approximate Engine) - Engine Core
 *
 * QOM object managing the global timing state: cycle counter,
 * event queue, and registered CPU list.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "hw/core/cpu.h"
#include "cae/engine.h"
#include "cae/pipeline.h"
#include "cae/uop.h"
#include "cae/mem.h"
#include "cae/cpu_model.h"
#include "cae/cpu_ooo.h"
#include "cae/bpred.h"
#include "cae/trace-emit.h"
#include "cae/checkpoint.h"

/*
 * engine.c is compiled into libcommon where CONFIG_CAE is poisoned
 * (system-wide invariant), so we cannot include include/cae/cae-mem-hook.h
 * — its declarations live behind #ifdef CONFIG_CAE. Redeclare the
 * two functions we define here so -Wmissing-prototypes is satisfied.
 * These must match cae-mem-hook.h byte-for-byte.
 */
void cae_mem_access_notify(void *cpu, uint64_t addr,
                           uint32_t size, int op,
                           const void *value);
void cae_charge_executed_tb(void);
bool cae_tlb_gate_default_for_cpu_model(const char *name);
/* cae_first_pc_observe is declared in include/cae/pipeline.h, which
 * engine.c already includes — no local redeclaration needed. */

/*
 * Round 20 t-mem-async-iface: parameters / helpers shared by the
 * async dispatch path in cae_mem_access_notify. The backpressure
 * budget caps how many 1-cycle stalls the engine takes while a
 * backend refuses via can_accept(); the drain budget caps the
 * cycle-by-cycle pump that waits for the scheduled completion
 * event. The values are deliberately wide — real completions
 * land within tens of cycles at 1 GHz engine freq; the budget
 * exists solely to escape a broken backend that never completes.
 */
#define CAE_MEM_BACKPRESSURE_MAX 64u
#define CAE_MEM_DRAIN_MAX        100000u

struct CaeAsyncWait {
    CaeMemResp resp;
    bool done;
};

static void cae_async_wait_cb(CaeMemResp *resp, void *opaque)
{
    struct CaeAsyncWait *ctx = opaque;
    ctx->resp = *resp;
    ctx->done = true;
}

/*
 * AC-K-13 pure policy helper. Keeps the decide-which-value-the-
 * gate-takes-per-cpu-model logic in a single place so (a) round-3+
 * refactors cannot accidentally re-decide the flip direction and
 * (b) tests/unit/test-cae can exercise both directions without
 * spinning up the full accel init path. Callers should assign the
 * result to `cae_tlb_force_slow_active` at cpu-model attach.
 */
bool cae_tlb_gate_default_for_cpu_model(const char *name)
{
    if (name == NULL) {
        return true;
    }
    if (strcmp(name, "ooo-kmhv3") == 0) {
        return false;
    }
    /* inorder-5stage, cpi1, and any future model default true so
     * round-4 memory-visibility behavior (AC-K-8) stays preserved
     * until the new model's attach path explicitly opts out. */
    return true;
}

/*
 * AC-K-10 first-PC observe point (round 6). Called from
 * HELPER(lookup_tb_ptr) BEFORE the classify block rewrites
 * active_uop->pc, with `pc` = the INCOMING TB's PC (= the
 * architectural PC that's about to retire next). Latches
 * `cpu->first_pc` on the first call where pc >= trace_start_pc
 * (so the virt-machine bootrom at 0x1000 is skipped when
 * trace_start_pc is set to MBASE).
 *
 * Round 5 latched in cae_charge_executed_tb using
 * active_uop->pc, but that field is the JUST-retired PC — it
 * lags one helper call and captured 0x8000004e (inner-loop
 * entry) instead of 0x80000000 (benchmark entry). Moving the
 * latch to the pc-known moment in the helper fixes the
 * contract directly.
 */
void cae_first_pc_observe(CaeCpu *cpu, uint64_t pc)
{
    CaeEngine *engine;

    if (!cpu || pc == 0) {
        return;
    }
    engine = cpu->engine;
    if (!engine) {
        return;
    }
    if (cpu->first_pc_latched) {
        return;
    }
    if (pc < engine->trace_start_pc) {
        return;
    }
    cpu->first_pc = pc;
    cpu->first_pc_latched = true;
}

/* Default base frequency: 1 GHz */
#define CAE_DEFAULT_FREQ_HZ 1000000000ULL

/* Default sync interval in cycles */
#define CAE_DEFAULT_SYNC_INTERVAL 10000

static void cae_engine_get_virtual_clock_ns(Object *obj, Visitor *v,
                                            const char *name,
                                            void *opaque, Error **errp)
{
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t value = (uint64_t)ns;
    visit_type_uint64(v, name, &value, errp);
}

/*
 * current_cycle is written from the vCPU thread and read from the I/O
 * thread (QMP, accel clock hook). On 32-bit hosts a plain 64-bit load
 * tears, so QMP consumers could observe garbage or backwards jumps.
 * Route the QMP read through qatomic_read to match the writer-side
 * atomic publication.
 */
static void cae_engine_get_current_cycle(Object *obj, Visitor *v,
                                         const char *name,
                                         void *opaque, Error **errp)
{
    CaeEngine *engine = CAE_ENGINE(obj);
    uint64_t value = qatomic_read(&engine->current_cycle);
    visit_type_uint64(v, name, &value, errp);
}

static void cae_engine_get_counters_frozen(Object *obj, Visitor *v,
                                           const char *name,
                                           void *opaque, Error **errp)
{
    CaeEngine *engine = CAE_ENGINE(obj);
    bool value = qatomic_read(&engine->counters_frozen);
    visit_type_bool(v, name, &value, errp);
}

/*
 * AC-K-13 per-mode TLB_FORCE_SLOW gate. Definition lives here (in
 * libcommon) so tests/unit/test-cae, which builds against engine.c
 * without linking accel/cae/cae-all.c, resolves the symbol. Defaults
 * to true so the Phase-2 in-order track keeps its round-4 memory
 * visibility (AC-K-8). cae_tlb_gate_default_for_cpu_model() is the
 * write-through policy helper; cputlb.c reads the flag on the
 * slow-path decision. The QMP-observable property below is the
 * run-cae-test.sh hook.
 */
bool cae_tlb_force_slow_active = true;

static void cae_engine_get_tlb_force_slow(Object *obj, Visitor *v,
                                          const char *name,
                                          void *opaque, Error **errp)
{
    bool value = cae_tlb_force_slow_active;
    visit_type_bool(v, name, &value, errp);
}

/*
 * Round 41 directive step 5: parse a deterministic stimulus
 * program string into an array of CaeSpecStimulus entries.
 * Format: "<op>:<addr>:<bytes>[:<value>][;...]". See the
 * CaeEngine.spec_stimulus_program field comment for the full
 * grammar. Errors produce an Error* set and `false` returned;
 * `*out_count` is set to the entries parsed so far (useful for
 * diagnostics but undefined when the function fails).
 *
 * `out` may be NULL when callers only want validation (the
 * setter uses this form). `out_cap` is the array capacity.
 * Parsing stops with an error when the input contains more
 * entries than `out_cap`.
 *
 * Unit tests and run-cae.py agree on this format via
 * `-global cae-engine.spec-stimulus-program=<value>`.
 */
static bool cae_engine_parse_spec_stimulus_program(const char *program,
                                                   CaeSpecStimulus *out,
                                                   size_t out_cap,
                                                   size_t *out_count,
                                                   Error **errp)
{
    if (out_count) {
        *out_count = 0;
    }
    if (program == NULL || program[0] == '\0') {
        return true;
    }

    gchar **entries = g_strsplit(program, ";", -1);
    size_t n = 0;
    bool ok = true;

    for (size_t i = 0; entries[i] != NULL; i++) {
        const gchar *raw = entries[i];
        /* Skip leading whitespace */
        while (*raw == ' ' || *raw == '\t') {
            raw++;
        }
        if (*raw == '\0') {
            continue; /* tolerate trailing ';' and blank segments */
        }

        gchar **fields = g_strsplit(raw, ":", -1);
        unsigned field_count = g_strv_length(fields);
        if (field_count < 3 || field_count > 4) {
            error_setg(errp,
                "spec-stimulus-program entry '%s': expected 3 or 4 "
                "colon-separated fields, got %u", raw, field_count);
            g_strfreev(fields);
            ok = false;
            break;
        }

        /*
         * Round 43 (Codex round-42 review blocker): the op
         * field must be exactly ONE character. Round-42's
         * parser picked fields[0][0] without bounding the
         * length, silently accepting "rr:0x1000:8" or
         * "write:0x1000:8:0xdead" as READ/WRITE — looser
         * than the documented r/w/f grammar in
         * include/cae/engine.h and xs-1c-realspec.yaml.
         */
        if (fields[0][0] == '\0' || fields[0][1] != '\0') {
            error_setg(errp,
                "spec-stimulus-program entry '%s': op field "
                "'%s' must be exactly one character (r/w/f)",
                raw, fields[0]);
            g_strfreev(fields);
            ok = false;
            break;
        }

        uint8_t op_code;
        char op_ch = fields[0][0];
        if (op_ch == 'r' || op_ch == 'R') {
            op_code = 0; /* READ */
        } else if (op_ch == 'w' || op_ch == 'W') {
            op_code = 1; /* WRITE */
        } else if (op_ch == 'f' || op_ch == 'F') {
            op_code = 2; /* FETCH */
        } else {
            error_setg(errp,
                "spec-stimulus-program entry '%s': op must be "
                "r/w/f (got '%s')", raw, fields[0]);
            g_strfreev(fields);
            ok = false;
            break;
        }

        /*
         * Round 42 tightening (Codex round-41 review):
         * documented grammar says addr and value are
         * "0x-prefixed hex". Reject decimal / octal forms
         * that the previous `base=0` parse silently
         * accepted, so `qom-set` rejects the same inputs
         * the YAML comment says are invalid.
         */
        gchar *endp = NULL;
        if (fields[1][0] != '0' ||
            (fields[1][1] != 'x' && fields[1][1] != 'X')) {
            error_setg(errp,
                "spec-stimulus-program entry '%s': addr '%s' "
                "must be 0x-prefixed hex", raw, fields[1]);
            g_strfreev(fields);
            ok = false;
            break;
        }
        uint64_t addr = g_ascii_strtoull(fields[1] + 2, &endp, 16);
        if (endp == fields[1] + 2 || (endp && *endp != '\0')) {
            error_setg(errp,
                "spec-stimulus-program entry '%s': addr '%s' "
                "is not a valid 0x-prefixed hex number",
                raw, fields[1]);
            g_strfreev(fields);
            ok = false;
            break;
        }

        endp = NULL;
        uint64_t bytes64 = g_ascii_strtoull(fields[2], &endp, 10);
        if (endp == fields[2] || (endp && *endp != '\0') ||
            (bytes64 != 1 && bytes64 != 2 && bytes64 != 4 &&
             bytes64 != 8)) {
            error_setg(errp,
                "spec-stimulus-program entry '%s': bytes '%s' "
                "must be 1, 2, 4, or 8", raw, fields[2]);
            g_strfreev(fields);
            ok = false;
            break;
        }

        uint64_t value = 0;
        if (field_count == 4) {
            if (fields[3][0] != '0' ||
                (fields[3][1] != 'x' && fields[3][1] != 'X')) {
                error_setg(errp,
                    "spec-stimulus-program entry '%s': value "
                    "'%s' must be 0x-prefixed hex",
                    raw, fields[3]);
                g_strfreev(fields);
                ok = false;
                break;
            }
            endp = NULL;
            value = g_ascii_strtoull(fields[3] + 2, &endp, 16);
            if (endp == fields[3] + 2 || (endp && *endp != '\0')) {
                error_setg(errp,
                    "spec-stimulus-program entry '%s': value "
                    "'%s' is not a valid 0x-prefixed hex number",
                    raw, fields[3]);
                g_strfreev(fields);
                ok = false;
                break;
            }
        } else if (op_code == 1) {
            error_setg(errp,
                "spec-stimulus-program entry '%s': WRITE requires "
                "a value field", raw);
            g_strfreev(fields);
            ok = false;
            break;
        }

        /*
         * Round 42 blocker fix (Codex round-41 blocking issue #1):
         * capacity enforcement is independent of whether the caller
         * passes an array to fill. The round-41 setter called with
         * out=NULL specifically to "validate only"; leaving the
         * cap check inside the `out != NULL` branch meant a 17-entry
         * program was accepted by qom-set but unreachable from the
         * drain. Move the cap check outside the guard so the
         * shared-parser contract is actually uniform across call
         * paths — the claim from Corollary J.
         */
        if (n >= out_cap) {
            error_setg(errp,
                "spec-stimulus-program has more than %zu entries "
                "(limit is CAE_SPEC_STIMULI_MAX)", out_cap);
            g_strfreev(fields);
            ok = false;
            break;
        }

        if (out != NULL) {
            out[n].addr  = addr;
            out[n].bytes = (uint32_t)bytes64;
            out[n].op    = op_code;
            out[n].value = value;
        }
        n++;
        g_strfreev(fields);
    }

    g_strfreev(entries);
    if (out_count) {
        *out_count = n;
    }
    return ok;
}

static char *cae_engine_get_spec_stimulus_program(Object *obj,
                                                  Error **errp)
{
    CaeEngine *engine = CAE_ENGINE(obj);
    return g_strdup(engine->spec_stimulus_program != NULL
                    ? engine->spec_stimulus_program
                    : "");
}

static void cae_engine_set_spec_stimulus_program(Object *obj,
                                                 const char *value,
                                                 Error **errp)
{
    CaeEngine *engine = CAE_ENGINE(obj);
    /* Validate before committing so a bad config fails the setter. */
    if (!cae_engine_parse_spec_stimulus_program(value, NULL,
                                                CAE_SPEC_STIMULI_MAX,
                                                NULL, errp)) {
        return;
    }
    g_free(engine->spec_stimulus_program);
    engine->spec_stimulus_program = (value != NULL && value[0] != '\0')
                                    ? g_strdup(value) : NULL;
}

static void cae_engine_instance_init(Object *obj)
{
    CaeEngine *engine = CAE_ENGINE(obj);

    engine->current_cycle = 0;
    engine->event_queue = NULL;
    engine->next_event_seq = 0;
    QTAILQ_INIT(&engine->cpus);
    engine->num_cpus = 0;
    engine->base_freq_hz = CAE_DEFAULT_FREQ_HZ;
    engine->sync_interval = CAE_DEFAULT_SYNC_INTERVAL;
    engine->mem_backend = NULL;
    engine->icache_backend = NULL;
    engine->sentinel_addr = 0;
    engine->counters_frozen = false;
    engine->freeze_pending = false;
    engine->frozen_charge_calls = 0;
    engine->frozen_notify_calls = 0;

    /*
     * Expose timing counters as read-only QOM properties so integration
     * tests can query them via QMP qom-get. The engine is parented
     * under /objects/cae-engine by cae_init_machine, so the full path
     * is /objects/cae-engine/current-cycle.
     */
    object_property_add(obj, "current-cycle", "uint64",
                        cae_engine_get_current_cycle,
                        NULL, NULL, NULL);
    object_property_add_uint32_ptr(obj, "num-cpus",
                                   &engine->num_cpus,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "base-freq-hz",
                                   &engine->base_freq_hz,
                                   OBJ_PROP_FLAG_READ);

    /*
     * virtual-clock-ns routes through qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
     * which dispatches via cpus_get_virtual_clock() → cpus_accel->
     * get_virtual_clock (i.e. cae_get_virtual_clock). A QMP qom-get on
     * this property is therefore a direct regression probe for the
     * accel virtual-clock fallback: if the fallback ever recurses back
     * to itself again, this getter stack-overflows.
     */
    object_property_add(obj, "virtual-clock-ns", "uint64",
                        cae_engine_get_virtual_clock_ns,
                        NULL, NULL, NULL);
    object_property_add(obj, "counters-frozen", "bool",
                        cae_engine_get_counters_frozen,
                        NULL, NULL, NULL);
    object_property_add(obj, "tlb-force-slow-active", "bool",
                        cae_engine_get_tlb_force_slow,
                        NULL, NULL, NULL);
    object_property_add_uint64_ptr(obj, "sentinel-addr",
                                   &engine->sentinel_addr,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "tlb-miss-cycles",
                                   &engine->tlb_miss_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "frozen-charge-calls",
                                   &engine->frozen_charge_calls,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "frozen-notify-calls",
                                   &engine->frozen_notify_calls,
                                   OBJ_PROP_FLAG_READ);

    /*
     * Round 41 directive step 5: deterministic speculative-
     * stimulus program. Optional config consumed by
     * `cae_cpu_drain_spec_stimuli`; empty/NULL means no
     * auto-queue (the default; round-40 behavior). Parsed
     * lazily on each drain so benchmark harness runs
     * re-execute the program for every live window.
     */
    engine->spec_stimulus_program = NULL;
    object_property_add_str(obj, "spec-stimulus-program",
                            cae_engine_get_spec_stimulus_program,
                            cae_engine_set_spec_stimulus_program);
}

static void cae_engine_instance_finalize(Object *obj)
{
    CaeEngine *engine = CAE_ENGINE(obj);
    cae_engine_finalize(engine);
}

void cae_engine_finalize(CaeEngine *engine)
{
    if (engine->event_queue) {
        g_tree_destroy(engine->event_queue);
        engine->event_queue = NULL;
    }
    if (engine->mem_backend) {
        object_unref(engine->mem_backend);
        engine->mem_backend = NULL;
    }
    if (engine->icache_backend) {
        object_unref(engine->icache_backend);
        engine->icache_backend = NULL;
    }
    if (engine->cpu_model) {
        object_unref(engine->cpu_model);
        engine->cpu_model = NULL;
    }
    if (engine->bpred) {
        object_unref(engine->bpred);
        engine->bpred = NULL;
    }
    g_free(engine->spec_stimulus_program);
    engine->spec_stimulus_program = NULL;
}

/*
 * Called from cpu-exec.c after each TB retires. Charges cycles on the
 * per-CPU and engine counters according to the attached CPU model
 * (falls back to CPI=1 when no model is wired, preserving Phase-1
 * semantics). If a branch predictor is attached and the retiring uop
 * is a branch, the predictor is queried + updated coherently here and
 * mispredict penalty is added to stall_cycles.
 *
 * This path fires from both the post-cpu_loop_exec_tb and the
 * HELPER(lookup_tb_ptr) paths (see BL-20260417-cae-goto-ptr-chain-
 * undercount), so every retired TB — including chained ones — is
 * accounted for exactly once.
 */
void cae_charge_executed_tb(void)
{
    CaeCpu *cpu = cae_get_current_cpu();
    CaeEngine *engine;
    const CaeUop *uop;
    uint32_t cycles;
    uint32_t penalty = 0;

    if (!cpu) {
        return;
    }
    engine = cpu->engine;
    if (!engine) {
        return;
    }

    /*
     * AC-11 determinism guard: once a write has hit the configured
     * sentinel address, stop mutating cycle / insn / stall counters
     * so subsequent halt-loop retirement is invisible to the gate.
     * Return before ticking anything — including the engine cycle —
     * so clock-notify side effects also freeze.
     */
    if (qatomic_read(&engine->counters_frozen)) {
        qatomic_set(&engine->frozen_charge_calls,
                        qatomic_read(&engine->frozen_charge_calls) + 1);
        /*
         * Round 31 exit-path audit: a post-sentinel freeze can
         * arrive with a live spec snap if a branch was predicted
         * before the sentinel write retired. Drop it so the
         * snapshot does not leak through the frozen no-op path.
         */
        cae_cpu_spec_slot_drop_if_live(cpu);
        return;
    }

    uop = cpu->active_uop;
    /*
     * Round 47 AC-K-2 byte-identity: idempotent retire-charge
     * on the same classified active_uop. Under one_insn_per_tb
     * + CF_NO_GOTO_TB, HELPER(lookup_tb_ptr) and cpu_exec_loop's
     * post-TB site can both observe the same uop — without this
     * gate both sites would emit a retire record per
     * classification, duplicating the non-branch predecessor of
     * a chained backward branch. The flag is cleared inside
     * cae_uop_classify_bytes and set below after a successful
     * charge. NULL uop short-circuits (no-op engine / test path).
     */
    if (uop && uop->charged) {
        return;
    }
    cycles = cae_cpu_model_charge(engine->cpu_model, cpu, uop);
    /*
     * cycles == 0 is a legitimate return when the cpu-model accrues
     * sub-cycle overlap credit (cpu_inorder's pipeline-overlap
     * permille). The retire still bumps insn_count exactly once
     * below; it just doesn't move the cycle clock on this call.
     * The dispatcher's NULL-model fallback returns 1, so this path
     * only observes 0 when a model is actively modelling overlap.
     */

    /*
     * Round 17 drift-recovery: drain any pending branch
     * resolve on EVERY retire, not just branch retires.
     * Round 16's design only drained on the next-branch
     * retire, so a branch followed by non-branch retires
     * left the pending entry stale until another branch
     * appeared (possibly never). Hoisting the drain out of
     * the is_branch block bounds the lag to exactly one
     * retire regardless of uop class.
     *
     * The drain happens BEFORE this retire's predict call
     * so the FTQ-head identity invariant still holds (see
     * BL-20260419-bpred-push-pop-order-for-lag).
     */
    if (engine->bpred && cpu->bpred_pending_valid) {
        cae_bpred_update(engine->bpred,
                         &cpu->bpred_pending_resolve);
        cpu->bpred_pending_valid = false;
    }

    /*
     * Branch predictor resolve. Keyed on the uop's branch metadata
     * (populated by the target classifier at translate time / the
     * softmmu hook at execute time). When bpred is absent or uop is
     * not a branch, skip cleanly.
     */
    if (engine->bpred && uop && uop->is_branch) {
        uint8_t bytes = uop->insn_bytes ? uop->insn_bytes : 4;
        CaeBPredResolve r = {
            .pc = uop->pc,
            .actual_target = uop->branch_target,
            .actual_taken = uop->branch_taken,
            .insn_bytes = bytes,
            .is_conditional = uop->is_conditional,
            .is_call = uop->is_call,
            .is_return = uop->is_return,
            .is_indirect = uop->is_indirect,
        };
        /*
         * Round 17 drift-recovery: consume the frontend-
         * side prediction if one was stashed at TB entry.
         * pred_valid=true means cae_engine_on_frontend_
         * predict() ran at HELPER(lookup_tb_ptr) for this
         * branch and pushed into the FTQ there. In that
         * case we do NOT call cae_bpred_predict() again —
         * the FTQ entry is already live and the mispredict
         * check uses the stashed prediction.
         *
         * Fallback: pred_valid=false (first TB of a
         * cpu_exec slice; classifier absent; engine->bpred
         * attached after TB entry) triggers the retire-
         * side predict call to keep the round-16
         * accounting invariants.
         */
        CaeBPredPrediction p;
        if (cpu->spec_snap_valid) {
            /*
             * Round 32: the live save call-site at
             * HELPER(lookup_tb_ptr) stashed the prediction into
             * cpu->spec_predicted at TB entry. Prefer that
             * stashed copy over reading uop->pred_* directly so
             * the mispredict check is robust against any
             * reclassification of active_uop that might happen
             * between save and retire.
             */
            p = cpu->spec_predicted;
        } else if (uop->pred_valid) {
            p.target_pc = uop->pred_target;
            p.taken = uop->pred_taken;
            p.target_known = uop->pred_target_known;
        } else {
            CaeBPredQuery q = {
                .pc = uop->pc,
                .fallthrough_pc = uop->pc + bytes,
                .is_conditional = uop->is_conditional,
                .is_call = uop->is_call,
                .is_return = uop->is_return,
                .is_indirect = uop->is_indirect,
            };
            p = cae_bpred_predict(engine->bpred, &q);
        }

        bool mispredict = cae_bpred_is_mispredict(&p, &r);
        if (mispredict) {
            /*
             * Penalty source preference: cpu_model owns the pipeline
             * frontend-flush cost when present; the bpred object also
             * carries the same knob so that a bpred-only configuration
             * (cpu-model=cpi1 + bpred-model=2bit-local) still charges a
             * penalty instead of segfaulting on a NULL cpu_model. The
             * bpred knob is guaranteed non-NULL here because this entire
             * branch fires only when engine->bpred is set.
             */
            Object *penalty_src = engine->cpu_model ? engine->cpu_model
                                                    : engine->bpred;
            uint64_t v = object_property_get_uint(
                penalty_src, "mispredict-penalty-cycles", NULL);
            penalty = (uint32_t)v;
            qatomic_set(&cpu->bpred_mispredictions,
                            qatomic_read(&cpu->bpred_mispredictions) + 1);
        }
        qatomic_set(&cpu->bpred_predictions,
                        qatomic_read(&cpu->bpred_predictions) + 1);
        cpu->bpred_pending_resolve = r;
        cpu->bpred_pending_valid = true;

        /*
         * Round 31 live speculation resolve (t-tcg-spec-path).
         * When a live snapshot was taken at HELPER(lookup_tb_ptr)
         * for this branch, consume it here:
         *   - mispredict: restore the full CAE + RV architectural
         *     state to the save-point, squash any speculative
         *     stores with sqn >= spec_squash_sqn, drop the snap,
         *     clear the slot. Functional RV state is already
         *     correct (QEMU TCG does not speculate on the wrong
         *     path); the restore only unwinds the CAE timing
         *     model's speculative trajectory.
         *   - correct: just drop the snap and clear the slot.
         * Legacy path (no snap) is a no-op.
         */
        if (cpu->spec_snap_valid) {
            if (mispredict) {
                /*
                 * Live restore rewinds the CAE-side sub-blobs
                 * (OoO scalars + ROB/IQ/LSQ/RAT/sbuffer, bpred
                 * history + RAS, MSHR outstanding ring). The
                 * target's functional lane + the CaeCpu
                 * pending-resolve lane are intentionally NOT
                 * touched here — see the target-side
                 * live_restore implementation for the
                 * rationale.
                 *
                 * Round 32 bug #2 fix: do NOT call
                 * `cae_cpu_ooo_squash_after()` after the live
                 * restore. The restore has already composed
                 * through the five round-30 owner-module
                 * snapshots (ROB / IQ / LSQ / RAT / sbuffer) and
                 * brought each container back to its save-time
                 * state. Calling squash_after afterwards would
                 * unconditionally flush ROB/IQ/LSQ/RAT to empty
                 * — discarding the restoration that just
                 * happened and leaving sbuffer_squashes double-
                 * counted (its squashed counter is part of the
                 * just-restored sbuffer state, so the restore
                 * already reflects the save-time squash
                 * baseline). By design — per the plan-mandated
                 * one-insn-per-tb choice documented at
                 * hw/cae/cpu_ooo.c:7-8 — the live speculative
                 * window between save (TB entry) and retire spans
                 * one branch insn; wrong-path speculative memory
                 * traffic is not simulated functionally on this
                 * path. The stamp-and-gate pair in
                 * cae_mem_access_notify + hw/cae/cache.c exists
                 * for the unit-test-layer Option-X contract (and
                 * for any future multi-insn extension under
                 * DEC-4 R-2 relaxation), not for fire-during-live
                 * observability. The composed container restore
                 * is complete and sufficient here;
                 * `cae_cpu_ooo_squash_after()` remains available
                 * to unit tests + future R-2 fallback code paths
                 * that don't have a live snapshot to restore.
                 */
                cae_checkpoint_live_restore(cpu, cpu->spec_snap);
            }
            cae_checkpoint_drop(cpu->spec_snap);
            cpu->spec_snap = NULL;
            cpu->spec_snap_valid = false;
            cpu->spec_squash_sqn = 0;
            memset(&cpu->spec_predicted, 0, sizeof(cpu->spec_predicted));
        }
    }

    uint32_t total = cycles + penalty;
    qatomic_set(&cpu->cycle_count,
                    qatomic_read(&cpu->cycle_count) + total);
    qatomic_set(&cpu->insn_count,
                    qatomic_read(&cpu->insn_count) + 1);

    /*
     * AC-K-10 note: the first-PC latch moved to
     * cae_first_pc_observe() (called from HELPER(lookup_tb_ptr)
     * with the incoming TB's PC). The charge path no longer
     * latches because active_uop->pc at this point is the
     * JUST-retired PC, which lags by one helper call and gave
     * round 5's 0x8000004e instead of 0x80000000.
     */
    if (penalty) {
        qatomic_set(&cpu->stall_cycles,
                        qatomic_read(&cpu->stall_cycles) + penalty);
    }

    uint64_t new_cycle = qatomic_read(&engine->current_cycle) + total;
    qatomic_set(&engine->current_cycle, new_cycle);
    /*
     * Drain events scheduled on-or-before the new cycle so consumers
     * that rely on cae_engine_schedule_event() see firings during
     * ordinary TB retirement (not just when a memory-latency hook or
     * idle warp happens to advance the engine via advance_cycle).
     */
    cae_engine_process_events(engine, new_cycle);
    qemu_clock_notify(QEMU_CLOCK_VIRTUAL);

    /*
     * Retire-boundary trace hook (AC-K-2). The dispatch layer is
     * arch-neutral; the per-target emitter under target/<arch>/cae/
     * reads guest register state. No-op when `trace-out=` is unset,
     * which is the default. AC-K-10 alignment: skip retires below
     * engine->trace_start_pc so the first record matches NEMU's
     * first record (which boots raw *.xs.bin at MBASE, not the
     * virt-machine bootrom).
     */
    if (uop && uop->pc >= engine->trace_start_pc) {
        cae_trace_notify_retire(cpu, uop);
    }

    /*
     * Tier-2 checkpoint hook (AC-K-2.2 / AC-K-4). The arch-neutral
     * checkpoint layer owns the retire counter + interval trigger;
     * it calls the per-target emitter every
     * CAE_TRACE_CHECKPOINT_INTERVAL_DEFAULT retires. No-op when
     * `checkpoint-out=` is unset. The same trace_start_pc filter
     * applies: retires below MBASE (virt bootrom) don't count
     * toward the interval.
     */
    if (uop && uop->pc >= engine->trace_start_pc) {
        cae_checkpoint_notify_retire(cpu, uop);
    }

    /*
     * Clear softmmu-hook-populated fields on active_uop so the next TB
     * in a chain starts with a fresh slate. Without this, is_load /
     * is_store latch monotonically across a chain of TBs and bleed
     * into subsequent charge calls. Keep the classifier-populated
     * fields (type, fu_type, is_branch, etc.) intact — those are set
     * per-TB by whoever populates active_uop before TB retire.
     */
    if (cpu->active_uop) {
        CaeUop *u = cpu->active_uop;
        u->mem_addr = 0;
        u->mem_size = 0;
        u->is_load = false;
        u->is_store = false;
        /*
         * Round 47: mark this uop charged so any subsequent
         * charge site (HELPER / post-TB) that observes the
         * SAME active_uop before the next classify skips its
         * emit. Cleared by cae_uop_classify_bytes when the
         * next TB's classification populates active_uop.
         */
        u->charged = true;
    }

    /*
     * Round 47 AC-K-2.4 byte-identity: promote a pending sentinel
     * freeze now that the sentinel store's retire record has been
     * emitted above. The softmmu hook (cae_mem_access_notify) set
     * freeze_pending during the store's TB body; flipping the hard
     * counters_frozen latch here ensures the next charge (whatever
     * post-sentinel halt-loop insn fires) takes the early-return
     * path without dropping this final emit.
     */
    if (qatomic_read(&engine->freeze_pending)) {
        qatomic_set(&engine->freeze_pending, false);
        qatomic_set(&engine->counters_frozen, true);
    }
}

void cae_engine_advance_cycle(CaeEngine *engine, uint64_t cycles)
{
    /* Sentinel-write freeze guard — see cae_charge_executed_tb. */
    if (qatomic_read(&engine->counters_frozen)) {
        return;
    }
    uint64_t new_cycle = qatomic_read(&engine->current_cycle) + cycles;
    qatomic_set(&engine->current_cycle, new_cycle);
    cae_engine_process_events(engine, new_cycle);
}

void cae_engine_warp_idle(CaeEngine *engine, uint64_t target_cycle)
{
    uint64_t cur = qatomic_read(&engine->current_cycle);
    uint64_t delta;
    CaeCpu *cpu;

    /* Sentinel-write freeze guard — see cae_charge_executed_tb. */
    if (qatomic_read(&engine->counters_frozen)) {
        return;
    }

    if (target_cycle <= cur) {
        return;
    }
    delta = target_cycle - cur;
    qatomic_set(&engine->current_cycle, target_cycle);

    /*
     * The whole delta is time that passed while no vCPU was
     * executing: advance each registered CPU's cycle_count so it
     * tracks the engine, and record the same span as a stall so
     * QMP reporting remains internally consistent.
     */
    QTAILQ_FOREACH(cpu, &engine->cpus, next) {
        qatomic_set(&cpu->cycle_count,
                        qatomic_read(&cpu->cycle_count) + delta);
        qatomic_set(&cpu->stall_cycles,
                        qatomic_read(&cpu->stall_cycles) + delta);
    }

    /*
     * Drain events whose cycle falls in the warped-over window
     * (cur, target_cycle]. Without this, a backend that uses
     * cae_engine_schedule_event() for an async wakeup targeted at an
     * idle vCPU leaves the event pending past its firing cycle —
     * callers would only see it when the next TB retires or a memory
     * hook advances the engine. cae_charge_executed_tb() drains on
     * the hot path; this is the idle-warp counterpart.
     */
    cae_engine_process_events(engine, target_cycle);
}

void cae_engine_sync_virtual_clock(CaeEngine *engine)
{
    if (engine->base_freq_hz == 0) {
        return;
    }
    qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
}

/*
 * Round 19 t-mem-async-iface: expose the engine's current global
 * cycle to timing backends (cache_mshr's can_accept()) that are
 * inspected outside a CaeMemReq dispatch path and therefore cannot
 * read now_cycle off a request. Returns 0 when no engine is
 * registered, which keeps unit-test harnesses that never construct
 * the global engine safe.
 */
uint64_t cae_engine_current_cycle(void)
{
    CaeEngine *engine = cae_get_engine();
    if (!engine) {
        return 0;
    }
    return qatomic_read(&engine->current_cycle);
}

void cae_engine_register_cpu(CaeEngine *engine, CaeCpu *cpu)
{
    g_autofree char *name = NULL;

    QTAILQ_INSERT_TAIL(&engine->cpus, cpu, next);

    /*
     * Parent the CPU under the engine so QMP exposes deterministic
     * paths like /objects/cae-engine/cpu0. object_property_add_child
     * takes its own reference; drop the caller's object_new()
     * reference so the engine's child property is the sole retained
     * owner and the CPU is finalized cleanly when the engine is
     * torn down.
     */
    name = g_strdup_printf("cpu%u", engine->num_cpus);
    object_property_add_child(OBJECT(engine), name, OBJECT(cpu));
    object_unref(OBJECT(cpu));

    engine->num_cpus++;
}

CaeCpu *cae_engine_find_cpu(CaeEngine *engine, CPUState *qemu_cpu)
{
    CaeCpu *cpu;

    QTAILQ_FOREACH(cpu, &engine->cpus, next) {
        if (cpu->qemu_cpu == qemu_cpu) {
            return cpu;
        }
    }
    return NULL;
}

bool cae_engine_set_mem_backend(CaeEngine *engine, Object *backend,
                               Error **errp)
{
    if (!object_dynamic_cast(backend, TYPE_CAE_MEM)) {
        error_setg(errp, "Object does not implement CaeMemClass interface");
        return false;
    }
    if (engine->mem_backend) {
        object_unref(engine->mem_backend);
    }
    object_ref(backend);
    engine->mem_backend = backend;
    return true;
}

/*
 * Round 18 t-icache: attach a separate instruction-cache
 * backend. NULL is accepted and clears the slot (falling the
 * FETCH-hook routing back to mem_backend, preserving round-9
 * shared-cache semantics).
 */
bool cae_engine_set_icache_backend(CaeEngine *engine, Object *backend,
                                   Error **errp)
{
    if (backend && !object_dynamic_cast(backend, TYPE_CAE_MEM)) {
        error_setg(errp,
                   "Object does not implement CaeMemClass interface");
        return false;
    }
    if (engine->icache_backend) {
        object_unref(engine->icache_backend);
    }
    if (backend) {
        object_ref(backend);
    }
    engine->icache_backend = backend;
    return true;
}

bool cae_engine_set_cpu_model(CaeEngine *engine, Object *model, Error **errp)
{
    if (model && !object_dynamic_cast(model, TYPE_CAE_CPU_MODEL)) {
        error_setg(errp,
                   "Object does not implement CaeCpuModelClass interface");
        return false;
    }
    if (engine->cpu_model) {
        object_unref(engine->cpu_model);
    }
    if (model) {
        object_ref(model);
    }
    engine->cpu_model = model;
    return true;
}

bool cae_engine_set_bpred(CaeEngine *engine, Object *bpred, Error **errp)
{
    if (bpred && !object_dynamic_cast(bpred, TYPE_CAE_BPRED)) {
        error_setg(errp,
                   "Object does not implement CaeBPredClass interface");
        return false;
    }
    if (engine->bpred) {
        object_unref(engine->bpred);
    }
    if (bpred) {
        object_ref(bpred);
    }
    engine->bpred = bpred;
    return true;
}

/*
 * Round 17 drift-recovery: frontend-side predict hook.
 *
 * Fires at TB ENTRY from HELPER(lookup_tb_ptr), immediately
 * after the classifier has populated cpu->active_uop with
 * the INCOMING TB's first-insn metadata. When the classified
 * uop is a branch and an engine-level predictor is attached,
 * this hook calls cae_bpred_predict() (pushing into a
 * DecoupledBPU FTQ when one wraps the inner predictor) and
 * stashes the prediction into active_uop->pred_* so the
 * retire path (cae_charge_executed_tb) can consume it
 * without calling predict() again. That separates predict
 * (TB entry) from update (TB retire), which is the real
 * producer/consumer decoupling Codex asked for in rounds
 * 15-16.
 *
 * No-op on non-branches, when engine->bpred is NULL, or
 * when active_uop is NULL. This keeps the TCG call site
 * unconditional and cheap.
 */
void cae_engine_on_frontend_predict(CaeEngine *engine, CaeCpu *cpu)
{
    if (!engine || !engine->bpred || !cpu || !cpu->active_uop) {
        return;
    }
    /*
     * Skip frozen counters to keep AC-11 serial-determinism
     * guarantees. On sentinel-freeze we still want to stop
     * pushing to the FTQ so the post-sentinel halt loop does
     * not grow the stats.
     */
    if (qatomic_read(&engine->counters_frozen)) {
        return;
    }
    CaeUop *uop = cpu->active_uop;
    if (!uop->is_branch) {
        uop->pred_valid = false;
        return;
    }
    uint8_t bytes = uop->insn_bytes ? uop->insn_bytes : 4;
    CaeBPredQuery q = {
        .pc = uop->pc,
        .fallthrough_pc = uop->pc + bytes,
        .is_conditional = uop->is_conditional,
        .is_call = uop->is_call,
        .is_return = uop->is_return,
        .is_indirect = uop->is_indirect,
    };
    CaeBPredPrediction p = cae_bpred_predict(engine->bpred, &q);
    uop->pred_valid = true;
    uop->pred_taken = p.taken;
    uop->pred_target = p.target_pc;
    uop->pred_target_known = p.target_known;
}

/*
 * Round 17 drift-recovery: drain the pending-bpred-resolve
 * slot. Called from sentinel-freeze and explicit teardown
 * paths so the final branch of a benchmark always gets its
 * update() processed. No-op when pending is already clear.
 */
void cae_engine_bpred_flush_pending(CaeEngine *engine, CaeCpu *cpu)
{
    if (!engine || !engine->bpred || !cpu) {
        return;
    }
    if (cpu->bpred_pending_valid) {
        cae_bpred_update(engine->bpred,
                         &cpu->bpred_pending_resolve);
        cpu->bpred_pending_valid = false;
    }
    /*
     * Round 31 exit-path audit: a benchmark ending on a
     * predicted-but-not-resolved branch would otherwise leak
     * its spec snapshot through teardown. Drop it here so every
     * benchmark-final-drain path is snap-clean.
     */
    cae_cpu_spec_slot_drop_if_live(cpu);
}

/* Current CaeCpu executing (set per-instruction in cae_cpu_exec) */
static __thread CaeCpu *current_cae_cpu;

void cae_set_current_cpu(CaeCpu *cpu)
{
    current_cae_cpu = cpu;
}

CaeCpu *cae_get_current_cpu(void)
{
    return current_cae_cpu;
}

/*
 * Softmmu memory access notification.
 * Called from cputlb.c load/store helpers when cae_allowed is true.
 * Constructs a CaeMemReq, calls the backend's access() method if
 * available, and adds the returned latency to the engine cycle counter.
 */
void cae_mem_access_notify(void *opaque_cpu, uint64_t addr,
                           uint32_t size, int op,
                           const void *value)
{
    CaeCpu *cae_cpu = current_cae_cpu;
    CaeEngine *engine;
    CaeMemOp mem_op;

    if (!cae_cpu) {
        return;
    }

    engine = cae_cpu->engine;
    /*
     * Round 18 t-icache: the fetch-only path may have only
     * icache_backend attached. Allow the function to proceed
     * as long as EITHER backend is available; the backend
     * selection below picks per-op.
     */
    if (!engine || (!engine->mem_backend && !engine->icache_backend)) {
        return;
    }

    /*
     * Already frozen from a previous sentinel write? Skip the
     * backend dispatch and latency accounting — the subsequent
     * halt-loop retirement that called into us still must not move
     * counters.
     */
    if (qatomic_read(&engine->counters_frozen)) {
        qatomic_set(&engine->frozen_notify_calls,
                        qatomic_read(&engine->frozen_notify_calls) + 1);
        return;
    }

    /* Map hook op constants to CaeMemOp */
    switch (op) {
    case 3:  /* CAE_MEM_HOOK_TLB_MISS */
        if (engine->tlb_miss_cycles > 0) {
            engine->current_cycle += engine->tlb_miss_cycles;
            cae_cpu->stall_cycles += engine->tlb_miss_cycles;
            cae_cpu->tlb_miss_count++;
        }
        return;
    case 1:  /* CAE_MEM_HOOK_WRITE */
        mem_op = CAE_MEM_WRITE;
        break;
    case 2:  /* CAE_MEM_HOOK_FETCH */
        mem_op = CAE_MEM_FETCH;
        /*
         * AC-K-3.3 / AC-K-5: count every instruction-fetch hook
         * firing as an `insn_fetch_count` tick on the CaeCpu.
         * Exposed as a read-only QOM uint64 property so the M4'
         * I-cache model can key off a real hook stream instead
         * of estimating. Incremented before the mem-backend
         * dispatch so even when no mem_backend is attached
         * (Phase-1 default), the counter still moves.
         */
        qatomic_set(&cae_cpu->insn_fetch_count,
                        qatomic_read(&cae_cpu->insn_fetch_count) + 1);
        break;
    default: /* CAE_MEM_HOOK_READ */
        mem_op = CAE_MEM_READ;
        break;
    }

    /* Update active UOP memory fields if available */
    if (cae_cpu->active_uop) {
        CaeUop *uop = cae_cpu->active_uop;
        /*
         * Round 46 AC-K-2.4 store-record fix: only stamp mem_*
         * for architectural load/store. The fetch hook fires on
         * every TB's instruction read and would otherwise
         * overwrite a just-captured store's mem_addr with PC+N
         * of the *next* TB's fetch, corrupting the retired-insn
         * trace's mem_addr column on store records. FETCHes
         * still go through to the backend cache below.
         */
        if (mem_op == CAE_MEM_WRITE) {
            uop->mem_addr = addr;
            uop->mem_size = size;
            uop->is_store = true;
            /*
             * Capture the architectural store value so the retire-
             * side trace writer can record it (AC-K-2.4). We accept
             * only natural widths (<= 8 bytes) in trace v1. Values
             * larger than 8 bytes (crosspage split) are passed with
             * value=NULL; the uop field stays 0 and the split flag
             * is surfaced at the record-emit boundary.
             */
            if (value != NULL && size <= sizeof(uop->mem_value)) {
                uop->mem_value = 0;
                memcpy(&uop->mem_value, value, size);
            } else {
                uop->mem_value = 0;
            }
        } else if (mem_op == CAE_MEM_READ) {
            uop->mem_addr = addr;
            uop->mem_size = size;
            uop->is_load = true;
        }
    }

    /*
     * Call the memory backend. Round 18 t-icache: select the
     * I-cache backend when the hook is a FETCH and an I-cache
     * is attached; fall back to mem_backend otherwise. This
     * lets the M4' realspec model fetch timing independently
     * of data timing without breaking the round-9 shared-
     * cache configuration (where icache_backend is NULL and
     * every hook routes to mem_backend).
     */
    Object *target_backend = engine->mem_backend;
    if (mem_op == CAE_MEM_FETCH && engine->icache_backend) {
        target_backend = engine->icache_backend;
    }
    if (!target_backend) {
        /* FETCH with no icache AND no mem_backend — nothing to
         * dispatch. The early-return gate above caught the
         * neither-backend case, so this only fires when the
         * selected-but-unavailable combination happens (e.g.,
         * mem_backend is NULL and a READ hits it). */
        return;
    }
    {
        CaeMemClass *mc = CAE_MEM_CLASS(
            object_class_dynamic_cast(
                object_get_class(target_backend), TYPE_CAE_MEM));
        if (mc && mc->access) {
            CaeMemReq req = {
                .addr = addr,
                .size = size,
                .op = mem_op,
                .src_id = cae_cpu->cpu_id,
                .opaque = NULL,
                /*
                 * Round 19 t-mem-async-iface: stamp the engine's
                 * current global cycle so the MSHR can key
                 * completion entries against absolute time. Without
                 * this, outstanding-miss bookkeeping cannot expire
                 * correctly and LSQ / QMP observers (on the I/O
                 * thread) would see stale counts.
                 */
                .now_cycle = qatomic_read(&engine->current_cycle),
                /*
                 * Round 34 AC-K-4: propagate the live spec-window
                 * indicator down the memory path. When a checkpoint
                 * snapshot is in flight (cpu->spec_snap_valid ==
                 * true, set at HELPER(lookup_tb_ptr) save and
                 * cleared at cae_charge_executed_tb resolve), every
                 * access that happens inside that window is
                 * potentially on a wrong path. Downstream cache /
                 * MSHR layers MUST NOT install missed lines in that
                 * case (plan.md:87). The CPU still observes latency
                 * + data; squash rewinds CPU state separately via
                 * live_restore.
                 */
                .speculative = cae_cpu->spec_snap_valid,
            };
            CaeMemResp resp;
            bool used_async = false;

            /*
             * Round 20 t-mem-async-iface: prefer the async entry
             * point when the backend implements it. can_accept()
             * gates dispatch as the "active backpressure" contract
             * the plan asks for; when the backend refuses, advance
             * the engine one cycle (draining any in-flight
             * completions) and retry. A hard retry budget prevents
             * a broken backend from hanging the live path — on
             * exhaustion we fall through to the synchronous access
             * and emit a single diagnostic.
             */
            unsigned backpressure_retries = 0;
            if (mc->access_async) {
                while (mc->can_accept &&
                       !mc->can_accept(target_backend)) {
                    if (backpressure_retries >=
                        CAE_MEM_BACKPRESSURE_MAX) {
                        break;
                    }
                    cae_engine_advance_cycle(engine, 1);
                    backpressure_retries++;
                }
                /*
                 * Round 21 fix: refresh req.now_cycle after the
                 * backpressure stall so the backend sees the
                 * ACTUAL dispatch cycle, not the pre-stall cycle
                 * captured at req-construction time. MSHR expiry,
                 * capacity checks, and completion scheduling all
                 * key off req->now_cycle; without the refresh, an
                 * accepted-after-wait request is modelled as if
                 * it arrived before the wait.
                 */
                req.now_cycle = qatomic_read(&engine->current_cycle);
                uint64_t async_start =
                    qatomic_read(&engine->current_cycle);
                struct CaeAsyncWait ctx = { .done = false };
                bool accepted = mc->access_async(target_backend, &req,
                                                 cae_async_wait_cb,
                                                 &ctx);
                if (accepted) {
                    /*
                     * Drain the engine one cycle at a time until
                     * the scheduled completion event fires our
                     * callback. For backends that schedule a
                     * future CaeEvent (MSHR) this advances by
                     * `completion_cycle - now_cycle`. For
                     * backends whose access_async is a synchronous
                     * wrapper (dram.c, cache.c), the callback
                     * fired BEFORE access_async returned, so
                     * ctx.done is already true and the drain is a
                     * no-op — we compensate for that with the
                     * remainder-advance below so engine time
                     * still tracks the access's latency.
                     */
                    unsigned drain_retries = 0;
                    while (!ctx.done) {
                        if (drain_retries >= CAE_MEM_DRAIN_MAX) {
                            break;
                        }
                        if (qatomic_read(&engine->counters_frozen)) {
                            break;
                        }
                        cae_engine_advance_cycle(engine, 1);
                        drain_retries++;
                    }
                    if (ctx.done) {
                        resp = ctx.resp;
                        used_async = true;
                        /*
                         * Round 21 fix: guarantee total engine-
                         * cycle advance equals resp.latency for
                         * this access regardless of whether the
                         * backend fired its callback synchronously
                         * or deferred it via the event queue. If
                         * the drain advanced less than
                         * resp.latency (including the common
                         * drain==0 case for sync wrappers),
                         * advance the remainder now so
                         * engine->current_cycle and per-CPU
                         * cycle_count stay in sync. The round-20
                         * implementation skipped this and
                         * produced an aggregate undercount
                         * exactly equal to
                         * cpu0.memory_stall_cycles on
                         * l1-dram / mshr+icache paths.
                         */
                        uint64_t advanced =
                            qatomic_read(&engine->current_cycle)
                            - async_start;
                        if (resp.latency > advanced) {
                            cae_engine_advance_cycle(
                                engine, resp.latency - advanced);
                        }
                    } else {
                        /*
                         * Round 22 fix: drain aborted before the
                         * backend fired its callback (either
                         * counters_frozen latched mid-drain or
                         * CAE_MEM_DRAIN_MAX was exhausted on a
                         * broken backend). The backend has ALREADY
                         * accepted the request — re-dispatching
                         * through mc->access() would duplicate
                         * backend stats/state and break the
                         * single-dispatch contract the round-21
                         * review called out. Instead acknowledge
                         * the first dispatch as authoritative,
                         * charge the CPU for the cycles the drain
                         * actually consumed, and continue.
                         * counters_frozen path in particular
                         * must not re-enter mc->access — the
                         * sentinel freeze is supposed to stop the
                         * CAE accounting cleanly.
                         */
                        resp = (CaeMemResp){
                            .latency = drain_retries,
                            .result = CAE_MEM_HIT,
                            .opaque = NULL,
                            .completion_cycle =
                                qatomic_read(
                                    &engine->current_cycle),
                        };
                        used_async = true;
                    }
                }
            }
            if (!used_async) {
                resp = mc->access(target_backend, &req);
                if (resp.latency > 0) {
                    cae_engine_advance_cycle(engine, resp.latency);
                }
            }

            /*
             * Round 22 fix: backpressure stall cycles are part of
             * the per-access charge from the CPU's point of view.
             * Engine cycle was already advanced during the stall
             * loop; we fold the same count into resp.latency here
             * so the counter-update block below (which adds
             * resp.latency to cycle_count / stall / memory_stall /
             * load_stall) charges the CPU for the wait time too.
             * Without this, a backend that truly refuses
             * can_accept() would reintroduce
             * aggregate.total_cycles != cpu.cycles, just inverted
             * from the round-20 sync-wrapper bug.
             */
            if (used_async && backpressure_retries > 0) {
                resp.latency += backpressure_retries;
            }

            /*
             * Fold memory latency into the per-CPU counters. The
             * engine cycle itself was advanced during the async
             * drain (or just above in the sync path), so we only
             * charge per-CPU stall / cycle_count here. resp.latency
             * stays authoritative for the "how long this access
             * cost" attribution because it reflects the backend's
             * scheduling view regardless of which entry point was
             * used.
             */
            if (resp.latency > 0) {
                qatomic_set(&cae_cpu->cycle_count,
                                qatomic_read(&cae_cpu->cycle_count)
                                    + resp.latency);
                qatomic_set(&cae_cpu->stall_cycles,
                                qatomic_read(&cae_cpu->stall_cycles)
                                    + resp.latency);
                qatomic_set(&cae_cpu->memory_stall_cycles,
                                qatomic_read(
                                    &cae_cpu->memory_stall_cycles)
                                    + resp.latency);
                if (mem_op == CAE_MEM_READ) {
                    qatomic_set(&cae_cpu->load_stall_cycles,
                                    qatomic_read(
                                        &cae_cpu->load_stall_cycles)
                                        + resp.latency);
                }
            }
            /*
             * Data-access hit/miss attribution.
             *
             * `l1d_hits` / `l1d_misses` count every non-fetch access
             * (LOAD + STORE + AMO) — kept for L1D MPKI reporting.
             *
             * `load_hits` / `load_misses` count only loads. The AC-4
             * `avg_load_latency` metric is defined as per-LOAD latency,
             * so the gate must compute
             * `load_stall_cycles / (load_hits + load_misses)` and not
             * include stores. See round-4 contract §3 and
             * BL-20260418-avg-load-latency-load-only for the plan
             * alignment history.
             */
            if (mem_op != CAE_MEM_FETCH) {
                if (resp.result == CAE_MEM_HIT) {
                    qatomic_set(&cae_cpu->l1d_hits,
                                    qatomic_read(&cae_cpu->l1d_hits)
                                        + 1);
                    if (mem_op == CAE_MEM_READ) {
                        qatomic_set(&cae_cpu->load_hits,
                                        qatomic_read(
                                            &cae_cpu->load_hits) + 1);
                    }
                } else if (resp.result == CAE_MEM_MISS) {
                    qatomic_set(&cae_cpu->l1d_misses,
                                    qatomic_read(&cae_cpu->l1d_misses)
                                        + 1);
                    if (mem_op == CAE_MEM_READ) {
                        qatomic_set(&cae_cpu->load_misses,
                                        qatomic_read(
                                            &cae_cpu->load_misses) + 1);
                    }
                }
            }
        }
    }

    /*
     * AC-11 determinism: if this store hits the benchmark's
     * completion sentinel, latch `counters_frozen` AND force an
     * architectural stop of the vCPU. Round-3's round-3 summary
     * documented a residual (~1 mispredict / ~7 cycles) leak after
     * only the freeze-counters path was in place: post-sentinel
     * halt-loop retires still dispatched through
     * HELPER(lookup_tb_ptr)'s branch-resolve, and QMP cont/stop
     * slices let the VM execute a nondeterministic number of
     * halt-loop TBs before the sample boundary. Calling cpu_exit()
     * here breaks out of the current TB immediately; combined with
     * cae_cpu_exec()'s frozen-early-return guard, no further TBs
     * execute until run-cae.py's QMP `stop` pauses the VM cleanly.
     * The sentinel write's own memory latency has already been
     * accrued above; this just prevents any instruction after the
     * sentinel from touching the counters.
     */
    if (mem_op == CAE_MEM_WRITE && engine->sentinel_addr != 0 &&
        addr == engine->sentinel_addr) {
        /*
         * Round 17 drift-recovery: flush any pending
         * bpred-resolve slot BEFORE flipping counters_frozen.
         * Post-freeze, cae_bpred_update is skipped entirely
         * (the hot path gates on counters_frozen), so the
         * final branch's resolve would leak unless we drain
         * here. Drain before freeze means the last branch's
         * update() still counts.
         */
        cae_engine_bpred_flush_pending(engine, cae_cpu);
        /*
         * Round 47 AC-K-2.4 byte-identity: flip freeze_pending
         * instead of counters_frozen directly. The softmmu hook
         * runs mid-TB (BEFORE post-TB fires cae_charge_executed_tb
         * for this same store), so setting counters_frozen here
         * would cause the early-return guard at the top of
         * cae_charge_executed_tb to skip emitting the sentinel
         * store's own retire record. freeze_pending is promoted to
         * counters_frozen by cae_charge_executed_tb AFTER this
         * store's trace notify + counter bumps complete — one
         * final emit then a hard freeze.
         */
        qatomic_set(&engine->freeze_pending, true);
        if (cae_cpu->qemu_cpu) {
            cpu_exit(cae_cpu->qemu_cpu);
        }
    }

    /*
     * Notify QEMU_CLOCK_VIRTUAL during execution so that guest timers
     * (stimecmp, vstimecmp, PMU overflow) can fire between instructions
     * rather than waiting for cpu_exec() to return. This is called from
     * the softmmu hook path, which fires on every load/store/fetch.
     */
    qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
}

/*
 * Round 38 directive step 1: drain any queued live speculative-
 * memory stimuli through cae_mem_access_notify. Called
 * immediately after cae_checkpoint_save() sets spec_snap_valid
 * in HELPER(lookup_tb_ptr) (softmmu path), or directly from unit
 * tests. No-op when spec_snap_valid is false — drain is
 * scoped-to-the-live-window by contract.
 *
 * Reuses the engine's existing cae_mem_access_notify entry
 * point. Because that function reads the live CaeCpu from the
 * module-local `current_cae_cpu`, we set it to `cpu` for the
 * duration of the drain and restore the prior value afterwards.
 * This lets unit tests drive the drain without needing to also
 * have wired cae_set_current_cpu (though the round-33/37 test
 * harnesses do set it). Each stimulus becomes exactly one
 * cae_mem_access_notify call with the stimulus's (addr, bytes,
 * op) tuple; `value == NULL` because round 38 only exercises
 * loads (round 39's store extension will fold in the stimulus's
 * `value` field).
 */
uint32_t cae_cpu_drain_spec_stimuli(CaeCpu *cpu)
{
    if (cpu == NULL) {
        return 0;
    }
    if (!cpu->spec_snap_valid) {
        return 0;
    }

    /*
     * Round 41 directive step 5: if the per-CPU FIFO is empty
     * AND the engine has a stimulus program configured, parse
     * + auto-queue. Tests that manually pre-populate the FIFO
     * override the program (Corollary J). The setter
     * pre-validates, so a program reaching this point parses
     * cleanly — any error (should be impossible under normal
     * flow) leaves the queue empty and the drain returns 0.
     */
    if (cpu->spec_stimuli_count == 0 &&
        cpu->engine != NULL &&
        cpu->engine->spec_stimulus_program != NULL &&
        cpu->engine->spec_stimulus_program[0] != '\0') {
        size_t parsed = 0;
        if (cae_engine_parse_spec_stimulus_program(
                cpu->engine->spec_stimulus_program,
                cpu->spec_stimuli, CAE_SPEC_STIMULI_MAX,
                &parsed, NULL) && parsed > 0) {
            cpu->spec_stimuli_count = (uint8_t)parsed;
        }
    }

    if (cpu->spec_stimuli_count == 0) {
        return 0;
    }

    CaeCpu *prev_current = current_cae_cpu;
    current_cae_cpu = cpu;

    uint32_t fired = 0;
    uint32_t rejected = 0;
    for (uint8_t i = 0; i < cpu->spec_stimuli_count; i++) {
        const CaeSpecStimulus *s = &cpu->spec_stimuli[i];
        bool accepted = false;
        /*
         * Round 39 directive step 4: op-dispatch drain. Loads
         * and fetches go through the existing memory-notify
         * path (which stamps req.speculative=true and triggers
         * the round-34/35 cache gates). WRITES route through
         * the sbuffer, because plan.md:85 Option-X forbids any
         * externally-visible memory state from wrong-path
         * stores — they stay in the sbuffer and the round-32
         * squash_after path discards them on mispredict.
         *
         * Round 40 blocker fix (Codex round-39 review): the
         * WRITE branch now tracks backend acceptance. A
         * rejection (sbuffer full, NULL cpu_model, or
         * wrong-typed cpu_model) counts against
         * `spec_stimuli_rejected` instead of silently being
         * reported as fired. The queue is still cleared on
         * every drain — stimuli expire with the live window.
         * This is the harness-promotion prerequisite flagged
         * in Codex's round-39 review.
         */
        if (s->op == 1 /* WRITE */) {
            if (cpu->engine != NULL && cpu->engine->cpu_model != NULL) {
                accepted = cae_cpu_ooo_sbuffer_stage_spec_store(
                    cpu->engine->cpu_model,
                    s->addr, (uint16_t)s->bytes, s->value);
            }
            /* else: accepted stays false (missing dispatcher). */
        } else {
            /*
             * READ / FETCH dispatchers are infallible from
             * the drain's point of view: the notify function
             * handles its own internal fallbacks and never
             * signals rejection upward. Treat as accepted.
             */
            cae_mem_access_notify(cpu, s->addr, s->bytes,
                                  (int)s->op, NULL);
            accepted = true;
        }
        if (accepted) {
            fired++;
        } else {
            rejected++;
        }
    }

    current_cae_cpu = prev_current;

    cpu->spec_stimuli_count = 0;
    cpu->spec_stimuli_drained += fired;
    cpu->spec_stimuli_rejected += rejected;
    return fired;
}

static void cae_engine_class_init(ObjectClass *oc, const void *data)
{
    /* No class-level methods needed yet */
}

static const TypeInfo cae_engine_type = {
    .name = TYPE_CAE_ENGINE,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CaeEngine),
    .instance_init = cae_engine_instance_init,
    .instance_finalize = cae_engine_instance_finalize,
    .class_init = cae_engine_class_init,
};

static void cae_engine_register_types(void)
{
    type_register_static(&cae_engine_type);
}
type_init(cae_engine_register_types);
