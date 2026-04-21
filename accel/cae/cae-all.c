/*
 * CAE (Cycle Approximate Engine) - Accelerator Registration
 *
 * CAE inherits TYPE_TCG_ACCEL to reuse TCG's functional execution
 * infrastructure. It overrides init_machine to force one-insn-per-tb
 * mode and initialize the timing engine.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/accel.h"
#include "accel/accel-ops.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/tcg.h"
#include "system/cpu-timers.h"
#include "exec/icount.h"
#include "exec/replay-core.h"
#include "hw/core/cpu.h"
#include "qom/object_interfaces.h"
#include "cae/engine.h"
#include "cae/mem.h"
#include "cae/cache_mshr.h"
#include "cae/cae-mem-hook.h"
#include "cae/pipeline.h"
#include "cae/trace-emit.h"
#include "cae/checkpoint.h"
#include "cae/uop.h"
#include "migration/blocker.h"

#ifndef CONFIG_USER_ONLY
#include "hw/core/boards.h"
#endif

#define TYPE_CAE_ACCEL ACCEL_CLASS_NAME("cae")

bool cae_allowed;
unsigned cae_tbs_started;
unsigned cae_tbs_charged;
/*
 * AC-K-13 per-mode TLB_FORCE_SLOW gate — the definition now lives in
 * cae/engine.c (libcommon) so tests/unit/test-cae, which compiles
 * engine.c directly without linking accel/cae/cae-all.c, can
 * observe the flag via the QMP-equivalent pure accessor. cae-all.c
 * still writes it through `cae_tlb_gate_default_for_cpu_model()` at
 * attach time; only the storage moved.
 */
static CaeEngine *global_cae_engine;
static Error *cae_migration_blocker;

/*
 * CAE accelerator properties controlling the timing-only memory model.
 *
 * These are static so they can be bound to -accel knobs via
 * object_property_add_uint*_ptr() without extending the accel class's
 * C struct (the parent TCG accel's TCGState is private to
 * accel/tcg/tcg-all.c so we can't subclass it cleanly in this tree).
 * A single accel instance exists per QEMU run, so static storage is
 * the natural scope.
 *
 * When `memory-model` is "l1-dram", cae_init_machine() constructs a
 * cae-cache -> cae-dram chain using the other properties and wires it
 * into the engine. When "stub" (the default) the engine keeps the
 * zero-latency cae-mem-stub backend so Phase-1 harnesses remain
 * bit-identical.
 */
static char *cae_cfg_memory_model; /* "stub" (default) or "l1-dram" */
static uint64_t cae_cfg_l1_size         = 32 * 1024;
static uint32_t cae_cfg_l1_assoc        = 2;
static uint32_t cae_cfg_l1_line_size    = 64;
static uint32_t cae_cfg_l1_hit_cycles   = 1;
static uint32_t cae_cfg_l1_miss_cycles  = 10;
static uint64_t cae_cfg_dram_read_cycles  = 50;
static uint64_t cae_cfg_dram_write_cycles = 50;
/*
 * AC-K-3.2 MSHR knobs. Only consulted when memory-model=mshr. The
 * defaults mirror include/cae/cache_mshr.h so an unset YAML lane
 * produces the same topology as an explicit default. Zero values
 * fall back to the default inside the mshr instance_init.
 */
static uint32_t cae_cfg_mshr_size = 8;
static uint32_t cae_cfg_mshr_fill_queue = 16;
static uint32_t cae_cfg_mshr_writeback_queue = 16;
/*
 * Round 50 AC-K-5: L1D bank-conflict knobs forwarded to the
 * cae-cache-mshr instance when memory-model=mshr. Zero leaves
 * the MSHR default (disabled); non-zero turns the bank-conflict
 * stall model on. Plan default (kmhv3.py) splits L1D into 8
 * banks; stall cycles tunable.
 */
static uint32_t cae_cfg_mshr_bank_count            = 0;
static uint32_t cae_cfg_mshr_bank_conflict_stall   = 0;
/*
 * Round 50 AC-K-5: sbuffer evict-threshold knob forwarded to
 * the cae-sbuffer child of cae-cpu-ooo when cpu-model=ooo-kmhv3.
 * Zero leaves the sbuffer tracker disabled; non-zero enables
 * the evict-threshold-events counter.
 */
static uint32_t cae_cfg_sbuffer_evict_threshold    = 0;
static uint32_t cae_cfg_tlb_miss_cycles            = 0;
/*
 * Round 18 t-icache: separate instruction-cache knobs. Only
 * consulted when memory-model=mshr (M4' track; other memory-
 * model values do not split I/D). Defaults match kmhv3.py
 * realspec: 32 KiB, 8-way, 64-byte lines, 4-cycle hit,
 * 10-cycle miss. cae_cfg_icache_size=0 is interpreted as
 * "fall back to L1D size/assoc/line"; non-zero values drive
 * an independent I-cache topology.
 */
static uint64_t cae_cfg_icache_size        = 32 * 1024;
static uint32_t cae_cfg_icache_assoc       = 8;
static uint32_t cae_cfg_icache_line_size   = 64;
static uint32_t cae_cfg_icache_hit_cycles  = 4;
static uint32_t cae_cfg_icache_miss_cycles = 10;

/*
 * CPU-model / bpred knobs. Mirror the memory-model pattern: strings
 * pick model families, numeric knobs configure the selected family.
 * Default "cpi1" keeps Phase-1 behaviour; "inorder-5stage" attaches
 * the cae-cpu-inorder object to the engine and lets
 * cae_charge_executed_tb consult it. bpred "none" leaves
 * engine->bpred NULL; "2bit-local" attaches a cae-bpred-2bit-local
 * configured by the hyperparameter knobs below.
 */
static char *cae_cfg_cpu_model;  /* "cpi1" (default) or "inorder-5stage" */
static char *cae_cfg_bpred_model; /* "none" (default) or "2bit-local"    */
/*
 * Retire-boundary trace file (AC-K-2). NULL leaves tracing off (the
 * default). Set via -accel cae,trace-out=/path.bin. The per-target
 * emitter under target/<arch>/cae/ opens this lazily on the first
 * retired instruction. `trace_out_path_set` is set-once: the setter
 * rejects a second, different value so a reset mid-run cannot
 * silently truncate an in-progress trace (the header would be
 * rewritten and earlier records lost).
 */
static char *cae_cfg_trace_out;
/*
 * Tier-2 checkpoint output file (AC-K-2). NULL leaves checkpoint
 * emission off. Set via -accel cae,checkpoint-out=<path>. Mutually
 * exclusive with trace-out at the suite level (run-cae.py picks
 * one or the other based on --trace-out vs --checkpoint-out);
 * CAE itself accepts both simultaneously without complaint —
 * the two dispatch paths are independent.
 */
static char *cae_cfg_checkpoint_out;
static uint32_t cae_cfg_local_history_bits = 10;
static uint32_t cae_cfg_btb_entries        = 64;
static uint32_t cae_cfg_btb_assoc          = 2;
static uint32_t cae_cfg_ras_depth          = 16;
static uint32_t cae_cfg_mispredict_penalty = 3;
static uint32_t cae_cfg_latency_mul        = 3;
static uint32_t cae_cfg_latency_div        = 20;
static uint32_t cae_cfg_latency_fpu        = 4;
static uint32_t cae_cfg_overlap_permille   = 0;
static uint32_t cae_cfg_load_use_stall     = 0;

/*
 * Round 49 AC-K-5: OoO-kmhv3 width + regfile knobs. These are
 * accel-level forwarders; when cpu-model=ooo-kmhv3 is selected,
 * the accel `init_machine()` path sets the matching QOM
 * properties on the freshly-constructed CaeCpuOoo object
 * BEFORE calling user_creatable_complete, so the xs-1c-kmhv3
 * paired-YAML can drive the plan-required width/regfile
 * alignment (fetchW=32 / decodeW=8 / renameW=8 / commitW=8 /
 * ROB=352 / LQ=72 / SQ=56 / numPhysIntRegs=224 /
 * numPhysFloatRegs=256) without baking values into the source.
 *
 * Zero = "leave the CaeCpuOoo default" so the YAML only needs
 * to specify fields it wants to override. Non-OoO cpu-models
 * ignore these knobs entirely.
 */
static uint32_t cae_cfg_ooo_rob_size            = 0;
static uint32_t cae_cfg_ooo_lq_size             = 0;
static uint32_t cae_cfg_ooo_sq_size             = 0;
static uint32_t cae_cfg_ooo_issue_width         = 0;
static uint32_t cae_cfg_ooo_commit_width        = 0;
static uint32_t cae_cfg_ooo_rename_width        = 0;
static uint32_t cae_cfg_ooo_num_phys_int_regs   = 0;
static uint32_t cae_cfg_ooo_num_phys_float_regs = 0;
static uint32_t cae_cfg_ooo_issue_ports         = 0;
static uint32_t cae_cfg_ooo_virtual_issue_window = 0;
static uint32_t cae_cfg_ooo_dep_load_stall       = 0;

/*
 * AC-11 determinism support. 0 (default) leaves the freeze path
 * inert. A non-zero value is plumbed into engine->sentinel_addr at
 * init_machine(); the engine then freezes retire / warp-idle
 * accounting on the first guest store targeting this address.
 */
static uint64_t cae_cfg_sentinel_addr      = 0;

/*
 * AC-K-10 trace/first-PC alignment with NEMU. QEMU's virt machine
 * boots via the SiFive-style reset vector at 0x1000 which jumps to
 * MBASE (typically 0x80000000). NEMU on the other hand boots the
 * raw *.xs.bin directly at MBASE — no bootrom trampoline. Comparing
 * "first retired PC" across backends therefore needs a PC-range
 * filter on the CAE side: we skip retires below `trace_start_pc`
 * so the first latched PC + the first on-disk trace record match
 * the benchmark's entry point (set from MANIFEST.reset_pc by
 * run-cae.py). Default 0 = no filter (preserves existing
 * inorder-1c behaviour where no trace writer is attached).
 */
static uint64_t cae_cfg_trace_start_pc     = 0;

CaeEngine *cae_get_engine(void)
{
    return global_cae_engine;
}

static int cae_init_machine(AccelState *as, MachineState *ms)
{
    AccelClass *parent_class;
    int ret;
    bool mttcg;

    /*
     * Reject targets that have not plugged into the CAE framework.
     * Each supported target's type_init() constructors must register
     *  - a CAE AccelCPUClass (so CPU realization wiring exists), and
     *  - a guest classifier via cae_uop_register_classifier().
     * Both predicates are checked in arch-neutral form; absence of
     * either means the current binary's target was not wired up.
     */
    if (!cae_accel_cpu_is_registered() || !cae_uop_has_classifier()) {
        error_report("CAE not implemented for target %s", target_name());
        return -1;
    }

    /* Reject SMP: phase-1 only supports single-core */
    if (ms->smp.max_cpus > 1) {
        error_report("CAE does not support SMP in this version; "
                     "use -smp 1");
        return -1;
    }

    /*
     * Reject incompatible options before TCG init.
     *
     * Check replay_mode before icount_enabled(): QEMU's record/replay
     * implicitly enables icount, so the plain icount check would shadow
     * the dedicated record-replay message otherwise.
     */
    if (replay_mode != REPLAY_MODE_NONE) {
        error_report("CAE does not support record-replay");
        return -1;
    }

    if (icount_enabled()) {
        error_report("CAE does not support -icount");
        return -1;
    }

    /*
     * Reject MTTCG. CAE instance_init forces thread=single after
     * TCG sets the default. If thread is now "multi", the user
     * explicitly passed thread=multi on the command line.
     */
    {
        char *thread = object_property_get_str(OBJECT(current_accel()),
                                               "thread", NULL);
        if (thread) {
            mttcg = (strcmp(thread, "multi") == 0);
            g_free(thread);
        } else {
            mttcg = false;
        }
    }
    if (mttcg) {
        error_report("CAE does not support multi-threaded mode (MTTCG)");
        return -1;
    }

    /*
     * Force one-insn-per-tb mode via the QOM property.
     * This must be set before TCG initialization so the global
     * one_insn_per_tb flag is picked up during code generation.
     */
    object_property_set_bool(OBJECT(current_accel()),
                             "one-insn-per-tb", true, &error_fatal);

    /*
     * Reset tb-size to 0 (default) so user-specified values don't
     * change the TCG translation buffer allocation. tb-size is a
     * TCG-specific knob that should be inert under CAE.
     */
    {
        Error *local_err = NULL;
        object_property_set_uint(OBJECT(current_accel()),
                                 "tb-size", 0, &local_err);
        if (local_err) {
            error_free(local_err);
        }
    }

    /* Let TCG handle its own initialization */
    parent_class = ACCEL_CLASS(object_class_get_parent(
        OBJECT_CLASS(ACCEL_GET_CLASS(current_accel()))));
    ret = parent_class->init_machine(as, ms);
    if (ret < 0) {
        return ret;
    }

    cae_allowed = true;

    /* Block migration: CAE virtual clock has no VMState/migration path */
    error_setg(&cae_migration_blocker,
               "CAE does not support migration or savevm");
    migrate_add_blocker(&cae_migration_blocker, &error_fatal);

    /* Initialize the timing engine */
    global_cae_engine = CAE_ENGINE(object_new(TYPE_CAE_ENGINE));
    global_cae_engine->sentinel_addr = cae_cfg_sentinel_addr;
    global_cae_engine->trace_start_pc = cae_cfg_trace_start_pc;
    global_cae_engine->tlb_miss_cycles = cae_cfg_tlb_miss_cycles;
    global_cae_engine->counters_frozen = false;

    /*
     * Plumb trace-out (AC-K-2) into the dispatch layer. The per-target
     * emitter (target/<arch>/cae/cae-<arch>-trace.c) already registered
     * its vtable at type_init time; we just hand it the path so its
     * first retire hook can open the file. cae_cfg_trace_out is NULL
     * by default — no path, no tracing.
     */
    if (cae_cfg_trace_out != NULL) {
        Error *trace_err = NULL;
        if (!cae_trace_set_out_path(cae_cfg_trace_out, &trace_err)) {
            error_report_err(trace_err);
            return -1;
        }
    }

    /*
     * AC-K-2 tier-2 checkpoint producer plumbing. The per-target
     * emitter (target/<arch>/cae/cae-<arch>-checkpoint.c)
     * registered its vtable at type_init; this hands it the
     * output path so the first interval trigger opens the file.
     * Unset by default — no checkpoint emission.
     */
    if (cae_cfg_checkpoint_out != NULL) {
        Error *cp_err = NULL;
        if (!cae_checkpoint_set_out_path(cae_cfg_checkpoint_out, &cp_err)) {
            error_report_err(cp_err);
            return -1;
        }
    }

    /*
     * Parent the engine under /objects/cae-engine so integration tests
     * can read the timing state through QMP qom-get.
     */
    object_property_add_child(object_get_objects_root(), "cae-engine",
                              OBJECT(global_cae_engine));

    /* Verify AC-5 reject path: non-CaeMemClass object must be rejected */
    {
        Error *reject_err = NULL;
        /* Use the engine itself as a non-CaeMemClass object */
        bool rejected = !cae_engine_set_mem_backend(
            global_cae_engine,
            OBJECT(global_cae_engine), &reject_err);
        g_assert(rejected);
        g_assert(reject_err != NULL);
        error_free(reject_err);
    }

    /*
     * Attach the memory-timing backend. Three paths, tried in order:
     *
     *   1. User instantiated /objects/cae-mem-root via some future QMP
     *      / -object path (not reachable today - adding cae-cache and
     *      cae-dram to qapi/qom.json's ObjectType enum is a follow-up).
     *      If it exists and implements CaeMemClass, use it.
     *
     *   2. memory-model=l1-dram: build a cae-cache -> cae-dram chain
     *      from the l1-* / dram-* accel properties. This is the only
     *      way to enable timing-accurate memory from the command line
     *      today.
     *
     *   3. default (memory-model=stub): keep the zero-latency
     *      cae-mem-stub from Phase-1 so existing harnesses run
     *      bit-identical to before this change.
     */
    {
        Object *root = object_resolve_path_component(object_get_objects_root(),
                                                     "cae-mem-root");
        const char *model = cae_cfg_memory_model ? cae_cfg_memory_model
                                                 : "stub";

        if (root && object_dynamic_cast(root, TYPE_CAE_MEM)) {
            cae_engine_set_mem_backend(global_cae_engine, root,
                                       &error_fatal);
            /* engine_set_mem_backend adds its own strong ref; the
             * /objects child property keeps the caller-visible path. */
        } else if (strcmp(model, "l1-dram") == 0) {
            Object *dram = object_new("cae-dram");
            object_property_set_uint(dram, "read-latency-cycles",
                                     cae_cfg_dram_read_cycles,
                                     &error_fatal);
            object_property_set_uint(dram, "write-latency-cycles",
                                     cae_cfg_dram_write_cycles,
                                     &error_fatal);
            user_creatable_complete(USER_CREATABLE(dram), &error_fatal);
            /* Parent the DRAM so the cache's link property can resolve
             * its canonical path during set_link. */
            object_property_add_child(object_get_objects_root(),
                                      "cae-dram-auto", dram);
            object_unref(dram);

            Object *cache = object_new("cae-cache");
            object_property_set_uint(cache, "size",
                                     cae_cfg_l1_size, &error_fatal);
            object_property_set_uint(cache, "assoc",
                                     cae_cfg_l1_assoc, &error_fatal);
            object_property_set_uint(cache, "line-size",
                                     cae_cfg_l1_line_size, &error_fatal);
            object_property_set_uint(cache, "latency-hit-cycles",
                                     cae_cfg_l1_hit_cycles, &error_fatal);
            object_property_set_uint(cache, "latency-miss-cycles",
                                     cae_cfg_l1_miss_cycles, &error_fatal);
            object_property_set_link(cache, "downstream", dram,
                                     &error_fatal);
            user_creatable_complete(USER_CREATABLE(cache), &error_fatal);
            object_property_add_child(object_get_objects_root(),
                                      "cae-mem-root", cache);
            object_unref(cache);

            cae_engine_set_mem_backend(global_cae_engine, cache,
                                       &error_fatal);
        } else if (strcmp(model, "stub") == 0) {
            Object *stub = object_new("cae-mem-stub");
            cae_engine_set_mem_backend(global_cae_engine, stub,
                                       &error_fatal);
            /*
             * cae_engine_set_mem_backend() retains its own reference;
             * drop the local one so the engine is the sole owner.
             */
            object_unref(stub);
        } else if (strcmp(model, "mshr") == 0) {
            /*
             * KMH-V3 track memory model: cae-cache-mshr wrapping the
             * existing cae-cache -> cae-dram chain. The MSHR layer
             * tracks parallel-outstanding miss count and applies an
             * overlap discount when mshr_size >= 2 (AC-K-3.2). The
             * inner chain is constructed identically to the
             * l1-dram path so round-2's knob plumbing stays valid
             * for the OoO track.
             */
            Object *dram = object_new("cae-dram");
            object_property_set_uint(dram, "read-latency-cycles",
                                     cae_cfg_dram_read_cycles,
                                     &error_fatal);
            object_property_set_uint(dram, "write-latency-cycles",
                                     cae_cfg_dram_write_cycles,
                                     &error_fatal);
            user_creatable_complete(USER_CREATABLE(dram), &error_fatal);
            object_property_add_child(object_get_objects_root(),
                                      "cae-dram-auto", dram);
            object_unref(dram);

            Object *cache = object_new("cae-cache");
            object_property_set_uint(cache, "size",
                                     cae_cfg_l1_size, &error_fatal);
            object_property_set_uint(cache, "assoc",
                                     cae_cfg_l1_assoc, &error_fatal);
            object_property_set_uint(cache, "line-size",
                                     cae_cfg_l1_line_size, &error_fatal);
            object_property_set_uint(cache, "latency-hit-cycles",
                                     cae_cfg_l1_hit_cycles, &error_fatal);
            object_property_set_uint(cache, "latency-miss-cycles",
                                     cae_cfg_l1_miss_cycles, &error_fatal);
            object_property_set_link(cache, "downstream", dram,
                                     &error_fatal);
            user_creatable_complete(USER_CREATABLE(cache), &error_fatal);
            object_property_add_child(object_get_objects_root(),
                                      "cae-cache-auto", cache);
            object_unref(cache);

            Object *mshr = object_new("cae-cache-mshr");
            object_property_set_link(mshr, "downstream", cache,
                                     &error_fatal);
            /*
             * AC-K-3.2 knob plumbing. These are propagated to the
             * MSHR wrapper so mshr_size=1 vs mshr_size=8 produces
             * observably different cycle counts on miss-heavy
             * workloads (the cache_mshr.c sync-window accounting
             * exposes the delta as of round 4). cae-cache-mshr
             * falls back to include/cae/cache_mshr.h defaults when
             * the knob is zero.
             */
            object_property_set_uint(mshr, "mshr-size",
                                     cae_cfg_mshr_size, &error_fatal);
            object_property_set_uint(mshr, "fill-queue-size",
                                     cae_cfg_mshr_fill_queue,
                                     &error_fatal);
            object_property_set_uint(mshr, "writeback-queue-size",
                                     cae_cfg_mshr_writeback_queue,
                                     &error_fatal);
            /*
             * Round 50 AC-K-5: bank-conflict forwarders. Zero
             * leaves the MSHR default (disabled); non-zero turns
             * on the bank-conflict stall model so xs-1c-kmhv3.yaml
             * can deliver live kmhv3-aligned behaviour.
             */
            if (cae_cfg_mshr_bank_count) {
                object_property_set_uint(mshr, "bank-count",
                                         cae_cfg_mshr_bank_count,
                                         &error_fatal);
            }
            if (cae_cfg_mshr_bank_conflict_stall) {
                object_property_set_uint(mshr,
                                         "bank-conflict-stall-cycles",
                                         cae_cfg_mshr_bank_conflict_stall,
                                         &error_fatal);
            }
            user_creatable_complete(USER_CREATABLE(mshr), &error_fatal);
            object_property_add_child(object_get_objects_root(),
                                      "cae-mem-root", mshr);
            object_unref(mshr);

            cae_engine_set_mem_backend(global_cae_engine, mshr,
                                       &error_fatal);

            /*
             * Round 18 t-icache: M4' separate instruction-
             * cache. Built as a second cae-cache pointing at
             * the same DRAM, attached to the engine via
             * cae_engine_set_icache_backend(). Routing logic
             * in cae_mem_access_notify sends CAE_MEM_FETCH
             * here; CAE_MEM_READ/_WRITE continue to hit the
             * D-cache chain (mshr) above. If the knobs are
             * left at default values this is still a
             * distinct cache instance — that's the point of
             * a split-I/D model.
             */
            /*
             * Round 19 drive-by (from R-18 Codex review): honour the
             * documented `icache-size=0 -> fall back to L1D geometry`
             * knob by normalising zeros to the current L1D config at
             * construction time. Non-zero values still drive an
             * independent I-cache topology. assoc / line-size follow
             * the same fallback so a caller can zero-init all three
             * to mean "same as L1D".
             */
            uint64_t ic_size  = cae_icache_effective_size(
                                    cae_cfg_icache_size,
                                    cae_cfg_l1_size);
            uint32_t ic_assoc = cae_icache_effective_u32(
                                    cae_cfg_icache_assoc,
                                    cae_cfg_l1_assoc);
            uint32_t ic_line  = cae_icache_effective_u32(
                                    cae_cfg_icache_line_size,
                                    cae_cfg_l1_line_size);
            Object *icache = object_new("cae-cache");
            object_property_set_uint(icache, "size",
                                     ic_size, &error_fatal);
            object_property_set_uint(icache, "assoc",
                                     ic_assoc, &error_fatal);
            object_property_set_uint(icache, "line-size",
                                     ic_line, &error_fatal);
            object_property_set_uint(icache, "latency-hit-cycles",
                                     cae_cfg_icache_hit_cycles,
                                     &error_fatal);
            object_property_set_uint(icache, "latency-miss-cycles",
                                     cae_cfg_icache_miss_cycles,
                                     &error_fatal);
            object_property_set_link(icache, "downstream", dram,
                                     &error_fatal);
            user_creatable_complete(USER_CREATABLE(icache),
                                    &error_fatal);
            object_property_add_child(object_get_objects_root(),
                                      "cae-icache", icache);
            object_unref(icache);

            cae_engine_set_icache_backend(global_cae_engine,
                                          icache, &error_fatal);
        } else {
            error_report("CAE: unknown memory-model '%s' "
                         "(accepted: stub, l1-dram, mshr)", model);
            return -1;
        }
    }

    /*
     * Attach the CPU timing model + branch predictor the same way:
     * programmatically build the requested objects, parent them under
     * /objects/ with stable ids, and stash them on the engine. Strong
     * refs are held by the engine; the /objects parent keeps canonical
     * paths alive for QMP introspection.
     */
    {
        const char *cpu_model = cae_cfg_cpu_model ? cae_cfg_cpu_model
                                                  : "cpi1";
        const char *bpred_model = cae_cfg_bpred_model ? cae_cfg_bpred_model
                                                      : "none";

        if (strcmp(bpred_model, "2bit-local") == 0) {
            Object *bpred = object_new("cae-bpred-2bit-local");
            object_property_set_uint(bpred, "local-history-bits",
                                     cae_cfg_local_history_bits,
                                     &error_fatal);
            object_property_set_uint(bpred, "btb-entries",
                                     cae_cfg_btb_entries, &error_fatal);
            object_property_set_uint(bpred, "btb-assoc",
                                     cae_cfg_btb_assoc, &error_fatal);
            object_property_set_uint(bpred, "ras-depth",
                                     cae_cfg_ras_depth, &error_fatal);
            object_property_set_uint(bpred, "mispredict-penalty-cycles",
                                     cae_cfg_mispredict_penalty,
                                     &error_fatal);
            user_creatable_complete(USER_CREATABLE(bpred), &error_fatal);
            object_property_add_child(object_get_objects_root(),
                                      "cae-bpred", bpred);
            object_unref(bpred);
            cae_engine_set_bpred(global_cae_engine, bpred, &error_fatal);
        } else if (strcmp(bpred_model, "tournament") == 0) {
            Object *bpred = object_new("cae-bpred-tournament");
            object_property_set_uint(bpred, "local-history-bits",
                                     cae_cfg_local_history_bits,
                                     &error_fatal);
            /*
             * No global-history-bits accel property yet (round-3
             * minimal wiring); use the QOM default (12) baked into
             * cae-bpred-tournament's instance_init. The xs-1c-*
             * calibration round can expose it later if needed.
             */
            object_property_set_uint(bpred, "btb-entries",
                                     cae_cfg_btb_entries, &error_fatal);
            object_property_set_uint(bpred, "btb-assoc",
                                     cae_cfg_btb_assoc, &error_fatal);
            object_property_set_uint(bpred, "ras-depth",
                                     cae_cfg_ras_depth, &error_fatal);
            object_property_set_uint(bpred, "mispredict-penalty-cycles",
                                     cae_cfg_mispredict_penalty,
                                     &error_fatal);
            user_creatable_complete(USER_CREATABLE(bpred), &error_fatal);
            object_property_add_child(object_get_objects_root(),
                                      "cae-bpred", bpred);
            object_unref(bpred);
            cae_engine_set_bpred(global_cae_engine, bpred, &error_fatal);
        } else if (strcmp(bpred_model, "tage-sc-l") == 0) {
            /*
             * Round 13 M4' deliverable: TAGE-SC-L predictor. The
             * accel path takes the same BTB/RAS/penalty knobs as
             * tournament plus the predictor-specific tunables.
             * Each tunable is also available via the per-object
             * QOM property, so xs-1c-realspec.yaml can drive them
             * through the paired-YAML's `accel_override` map once
             * the calibration round lands. For now we set the
             * common-subset knobs here and leave the TAGE-specific
             * sizes at their QOM instance_init defaults.
             */
            Object *bpred = object_new("cae-bpred-tage-sc-l");
            object_property_set_uint(bpred, "btb-entries",
                                     cae_cfg_btb_entries, &error_fatal);
            object_property_set_uint(bpred, "btb-assoc",
                                     cae_cfg_btb_assoc, &error_fatal);
            object_property_set_uint(bpred, "ras-depth",
                                     cae_cfg_ras_depth, &error_fatal);
            object_property_set_uint(bpred, "mispredict-penalty-cycles",
                                     cae_cfg_mispredict_penalty,
                                     &error_fatal);
            user_creatable_complete(USER_CREATABLE(bpred), &error_fatal);
            object_property_add_child(object_get_objects_root(),
                                      "cae-bpred", bpred);
            object_unref(bpred);
            cae_engine_set_bpred(global_cae_engine, bpred, &error_fatal);
        } else if (strcmp(bpred_model, "decoupled") == 0) {
            /*
             * Round 15 M4' deliverable: DecoupledBPU. Wraps an
             * inner TAGE-SC-L (created in the decoupled
             * wrapper's complete()) with an FTQ/FSQ frontend
             * queue layer. The BTB/RAS/penalty knobs flow through
             * the wrapper into the inner TAGE-SC-L; the FTQ/FSQ
             * sizes keep their QOM instance_init defaults (64
             * each, matching kmhv3.py's DecoupledBPU baseline).
             */
            Object *bpred = object_new("cae-bpred-decoupled");
            object_property_set_uint(bpred, "btb-entries",
                                     cae_cfg_btb_entries, &error_fatal);
            object_property_set_uint(bpred, "btb-assoc",
                                     cae_cfg_btb_assoc, &error_fatal);
            object_property_set_uint(bpred, "ras-depth",
                                     cae_cfg_ras_depth, &error_fatal);
            object_property_set_uint(bpred, "mispredict-penalty-cycles",
                                     cae_cfg_mispredict_penalty,
                                     &error_fatal);
            user_creatable_complete(USER_CREATABLE(bpred), &error_fatal);
            object_property_add_child(object_get_objects_root(),
                                      "cae-bpred", bpred);
            object_unref(bpred);
            cae_engine_set_bpred(global_cae_engine, bpred, &error_fatal);
        } else if (strcmp(bpred_model, "none") != 0) {
            error_report("CAE: unknown bpred-model '%s' "
                         "(accepted: none, 2bit-local, tournament, "
                         "tage-sc-l, decoupled)", bpred_model);
            return -1;
        }

        if (strcmp(cpu_model, "inorder-5stage") == 0) {
            Object *model = object_new("cae-cpu-inorder");
            object_property_set_uint(model, "latency-mul",
                                     cae_cfg_latency_mul, &error_fatal);
            object_property_set_uint(model, "latency-div",
                                     cae_cfg_latency_div, &error_fatal);
            object_property_set_uint(model, "latency-fpu",
                                     cae_cfg_latency_fpu, &error_fatal);
            object_property_set_uint(model, "mispredict-penalty-cycles",
                                     cae_cfg_mispredict_penalty,
                                     &error_fatal);
            object_property_set_uint(model, "overlap-permille",
                                     cae_cfg_overlap_permille,
                                     &error_fatal);
            object_property_set_uint(model, "load-use-stall-cycles",
                                     cae_cfg_load_use_stall,
                                     &error_fatal);
            /*
             * Link the bpred onto the cpu model so object-level tools
             * (QMP qom-list / unit tests) can walk the topology. The
             * engine still retains its own refs for hot-path dispatch.
             */
            if (global_cae_engine->bpred) {
                object_property_set_link(model, "bpred",
                                         global_cae_engine->bpred,
                                         &error_fatal);
            }
            user_creatable_complete(USER_CREATABLE(model), &error_fatal);
            object_property_add_child(object_get_objects_root(),
                                      "cae-cpu-model", model);
            object_unref(model);
            cae_engine_set_cpu_model(global_cae_engine, model,
                                     &error_fatal);
            /* AC-K-13: policy decided by a pure helper so test-cae
             * can exercise both flip directions. */
            cae_tlb_force_slow_active =
                cae_tlb_gate_default_for_cpu_model("inorder-5stage");
        } else if (strcmp(cpu_model, "ooo-kmhv3") == 0) {
            Object *model = object_new("cae-cpu-ooo");
            if (global_cae_engine->bpred) {
                object_property_set_link(model, "bpred",
                                         global_cae_engine->bpred,
                                         &error_fatal);
            }
            /*
             * Round 49 AC-K-5: forward the accel-level width/regfile
             * knobs into the CaeCpuOoo instance BEFORE
             * user_creatable_complete so they are captured by the
             * sub-structure inits (rob/lq/sq/rat sizing). Zero =
             * "leave the CaeCpuOoo default."
             */
            if (cae_cfg_ooo_rob_size) {
                object_property_set_uint(model, "rob-size",
                                         cae_cfg_ooo_rob_size,
                                         &error_fatal);
            }
            if (cae_cfg_ooo_lq_size) {
                object_property_set_uint(model, "lq-size",
                                         cae_cfg_ooo_lq_size,
                                         &error_fatal);
            }
            if (cae_cfg_ooo_sq_size) {
                object_property_set_uint(model, "sq-size",
                                         cae_cfg_ooo_sq_size,
                                         &error_fatal);
            }
            if (cae_cfg_ooo_issue_width) {
                object_property_set_uint(model, "issue-width",
                                         cae_cfg_ooo_issue_width,
                                         &error_fatal);
            }
            if (cae_cfg_ooo_commit_width) {
                object_property_set_uint(model, "commit-width",
                                         cae_cfg_ooo_commit_width,
                                         &error_fatal);
            }
            if (cae_cfg_ooo_rename_width) {
                object_property_set_uint(model, "rename-width",
                                         cae_cfg_ooo_rename_width,
                                         &error_fatal);
            }
            if (cae_cfg_ooo_num_phys_int_regs) {
                object_property_set_uint(model, "num-phys-int-regs",
                                         cae_cfg_ooo_num_phys_int_regs,
                                         &error_fatal);
            }
            if (cae_cfg_ooo_num_phys_float_regs) {
                object_property_set_uint(model, "num-phys-float-regs",
                                         cae_cfg_ooo_num_phys_float_regs,
                                         &error_fatal);
            }
            /*
             * Round 50 AC-K-5: forward the sbuffer evict-threshold
             * knob onto the CaeCpuOoo BEFORE completion so the
             * child sbuffer is built with the threshold live.
             * Zero = keep the round-49 disabled behaviour.
             */
            if (cae_cfg_sbuffer_evict_threshold) {
                object_property_set_uint(model,
                    "sbuffer-evict-threshold",
                    cae_cfg_sbuffer_evict_threshold,
                    &error_fatal);
            }
            /*
             * Round 52 AC-K-5: forward the bank-conflict knobs to
             * the cpu-model's retire-path tracker. Same accel
             * statics already drive the mshr-side copy (round 50);
             * the cpu-model needs its own mirror so the retire
             * cycle charge can account for bank pressure under
             * CAE's one-insn-per-TB cadence.
             */
            if (cae_cfg_ooo_issue_ports) {
                object_property_set_uint(model, "sched-issue-ports",
                    cae_cfg_ooo_issue_ports, &error_fatal);
            }
            if (cae_cfg_ooo_virtual_issue_window) {
                object_property_set_uint(model, "virtual-issue-window",
                    cae_cfg_ooo_virtual_issue_window, &error_fatal);
            }
            if (cae_cfg_ooo_dep_load_stall) {
                object_property_set_uint(model,
                    "dependent-load-stall-cycles",
                    cae_cfg_ooo_dep_load_stall, &error_fatal);
            }
            if (cae_cfg_mshr_bank_count) {
                object_property_set_uint(model, "bank-count",
                    cae_cfg_mshr_bank_count, &error_fatal);
            }
            if (cae_cfg_mshr_bank_conflict_stall) {
                object_property_set_uint(model,
                    "bank-conflict-stall-cycles",
                    cae_cfg_mshr_bank_conflict_stall,
                    &error_fatal);
            }
            user_creatable_complete(USER_CREATABLE(model), &error_fatal);
            object_property_add_child(object_get_objects_root(),
                                      "cae-cpu-model", model);
            object_unref(model);
            cae_engine_set_cpu_model(global_cae_engine, model,
                                     &error_fatal);
            /* AC-K-13: policy decided by a pure helper; ooo-kmhv3
             * returns false so MSHR overlap becomes real
             * (AC-K-3.2). */
            cae_tlb_force_slow_active =
                cae_tlb_gate_default_for_cpu_model("ooo-kmhv3");
        } else if (strcmp(cpu_model, "cpi1") != 0) {
            error_report("CAE: unknown cpu-model '%s' "
                         "(accepted: cpi1, inorder-5stage, ooo-kmhv3)",
                         cpu_model);
            return -1;
        }
    }

    return 0;
}

static char *cae_mm_get(Object *obj, Error **errp)
{
    return g_strdup(cae_cfg_memory_model ? cae_cfg_memory_model : "stub");
}

static void cae_mm_set(Object *obj, const char *value, Error **errp)
{
    if (strcmp(value, "stub") != 0 &&
        strcmp(value, "l1-dram") != 0 &&
        strcmp(value, "mshr") != 0) {
        error_setg(errp, "memory-model: expected 'stub' / 'l1-dram' "
                         "/ 'mshr', got '%s'", value);
        return;
    }
    g_free(cae_cfg_memory_model);
    cae_cfg_memory_model = g_strdup(value);
}

static char *cae_cpu_get(Object *obj, Error **errp)
{
    return g_strdup(cae_cfg_cpu_model ? cae_cfg_cpu_model : "cpi1");
}

static void cae_cpu_set(Object *obj, const char *value, Error **errp)
{
    if (strcmp(value, "cpi1") != 0 &&
        strcmp(value, "inorder-5stage") != 0 &&
        strcmp(value, "ooo-kmhv3") != 0) {
        error_setg(errp, "cpu-model: expected 'cpi1' / 'inorder-5stage'"
                         " / 'ooo-kmhv3', got '%s'", value);
        return;
    }
    g_free(cae_cfg_cpu_model);
    cae_cfg_cpu_model = g_strdup(value);
}

static char *cae_bp_get(Object *obj, Error **errp)
{
    return g_strdup(cae_cfg_bpred_model ? cae_cfg_bpred_model : "none");
}

static void cae_bp_set(Object *obj, const char *value, Error **errp)
{
    if (strcmp(value, "none") != 0 &&
        strcmp(value, "2bit-local") != 0 &&
        strcmp(value, "tournament") != 0 &&
        strcmp(value, "tage-sc-l") != 0 &&
        strcmp(value, "decoupled") != 0) {
        error_setg(errp, "bpred-model: expected 'none' / '2bit-local'"
                         " / 'tournament' / 'tage-sc-l' / 'decoupled'"
                         ", got '%s'", value);
        return;
    }
    g_free(cae_cfg_bpred_model);
    cae_cfg_bpred_model = g_strdup(value);
}

static char *cae_checkpoint_out_get(Object *obj, Error **errp)
{
    return g_strdup(cae_cfg_checkpoint_out ? cae_cfg_checkpoint_out : "");
}

static void cae_checkpoint_out_set(Object *obj, const char *value,
                                   Error **errp)
{
    g_free(cae_cfg_checkpoint_out);
    cae_cfg_checkpoint_out = (value && value[0] != '\0')
                             ? g_strdup(value) : NULL;
}

static char *cae_trace_out_get(Object *obj, Error **errp)
{
    return g_strdup(cae_cfg_trace_out ? cae_cfg_trace_out : "");
}

static void cae_trace_out_set(Object *obj, const char *value, Error **errp)
{
    /*
     * Empty string means "clear", so the qmp cli can reset the
     * property in tests before binding a new path. We do NOT propagate
     * a clear into the dispatch layer — trace-emit.c is set-once per
     * run; clearing here only affects what `cae_init_machine` will
     * plumb in on the NEXT accel init. In the normal accel-init-once
     * flow, the string is accept-or-reject.
     */
    g_free(cae_cfg_trace_out);
    cae_cfg_trace_out = (value && value[0] != '\0') ? g_strdup(value) : NULL;
}

/*
 * CAE instance init runs AFTER the parent TCG's instance_init.
 * Force thread=single to override TCG's default which may be
 * "multi" on platforms with TARGET_SUPPORTS_MTTCG, and expose the
 * memory-model knobs.
 */
static void cae_accel_instance_init(Object *obj)
{
    object_property_set_str(obj, "thread", "single", &error_abort);

    /* Reset backing fields on every instance (the accel is created once
     * per QEMU run, but re-instantiation during tests would otherwise
     * inherit stale values). */
    g_free(cae_cfg_memory_model);
    cae_cfg_memory_model       = g_strdup("stub");
    g_free(cae_cfg_cpu_model);
    cae_cfg_cpu_model          = g_strdup("cpi1");
    g_free(cae_cfg_bpred_model);
    cae_cfg_bpred_model        = g_strdup("none");
    g_free(cae_cfg_trace_out);
    cae_cfg_trace_out          = NULL;
    g_free(cae_cfg_checkpoint_out);
    cae_cfg_checkpoint_out     = NULL;
    cae_cfg_l1_size            = 32 * 1024;
    cae_cfg_l1_assoc           = 2;
    cae_cfg_l1_line_size       = 64;
    cae_cfg_l1_hit_cycles      = 1;
    cae_cfg_l1_miss_cycles     = 10;
    cae_cfg_dram_read_cycles   = 50;
    cae_cfg_dram_write_cycles  = 50;
    cae_cfg_mshr_size             = 8;
    cae_cfg_mshr_fill_queue       = 16;
    cae_cfg_mshr_writeback_queue  = 16;
    cae_cfg_mshr_bank_count          = 0;
    cae_cfg_mshr_bank_conflict_stall = 0;
    cae_cfg_sbuffer_evict_threshold  = 0;
static uint32_t cae_cfg_tlb_miss_cycles            = 0;
    cae_cfg_ooo_issue_ports          = 0;
    cae_cfg_ooo_virtual_issue_window = 0;
    cae_cfg_ooo_dep_load_stall       = 0;
    cae_cfg_icache_size           = 32 * 1024;
    cae_cfg_icache_assoc          = 8;
    cae_cfg_icache_line_size      = 64;
    cae_cfg_icache_hit_cycles     = 4;
    cae_cfg_icache_miss_cycles    = 10;
    cae_cfg_local_history_bits = 10;
    cae_cfg_btb_entries        = 64;
    cae_cfg_btb_assoc          = 2;
    cae_cfg_ras_depth          = 16;
    cae_cfg_mispredict_penalty = 3;
    cae_cfg_latency_mul        = 3;
    cae_cfg_latency_div        = 20;
    cae_cfg_latency_fpu        = 4;
    cae_cfg_overlap_permille   = 0;
    cae_cfg_load_use_stall     = 0;
    cae_cfg_sentinel_addr      = 0;
    cae_cfg_trace_start_pc     = 0;

    object_property_add_str(obj, "memory-model",
                            cae_mm_get, cae_mm_set);
    object_property_add_str(obj, "cpu-model",
                            cae_cpu_get, cae_cpu_set);
    object_property_add_str(obj, "bpred-model",
                            cae_bp_get, cae_bp_set);
    object_property_add_str(obj, "trace-out",
                            cae_trace_out_get, cae_trace_out_set);
    object_property_add_str(obj, "checkpoint-out",
                            cae_checkpoint_out_get,
                            cae_checkpoint_out_set);
    object_property_add_uint64_ptr(obj, "l1-size",
                                   &cae_cfg_l1_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "l1-assoc",
                                   &cae_cfg_l1_assoc,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "l1-line-size",
                                   &cae_cfg_l1_line_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "l1-hit-cycles",
                                   &cae_cfg_l1_hit_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "l1-miss-cycles",
                                   &cae_cfg_l1_miss_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "dram-read-cycles",
                                   &cae_cfg_dram_read_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "dram-write-cycles",
                                   &cae_cfg_dram_write_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "mshr-size",
                                   &cae_cfg_mshr_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "fill-queue-size",
                                   &cae_cfg_mshr_fill_queue,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "writeback-queue-size",
                                   &cae_cfg_mshr_writeback_queue,
                                   OBJ_PROP_FLAG_READWRITE);
    /*
     * Round 50 AC-K-5: accel-level L1D bank-conflict + sbuffer
     * evict-threshold forwarders. Consumed only under
     * memory-model=mshr (bank-*) and cpu-model=ooo-kmhv3
     * (sbuffer-evict-threshold). Zero = keep the downstream
     * default (bank-conflict disabled, sbuffer evict tracker
     * disabled).
     */
    object_property_add_uint32_ptr(obj, "bank-count",
                                   &cae_cfg_mshr_bank_count,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "bank-conflict-stall-cycles",
                                   &cae_cfg_mshr_bank_conflict_stall,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "sbuffer-evict-threshold",
                                   &cae_cfg_sbuffer_evict_threshold,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "tlb-miss-cycles",
                                   &cae_cfg_tlb_miss_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    /*
     * Round 18 t-icache: accel knobs for the separate
     * instruction cache. Consumed only when memory-model=mshr;
     * other topologies leave these unused. Defaults match
     * kmhv3.py realspec (32 KiB / 8-way / 64-B / 4-cycle hit
     * / 10-cycle miss).
     */
    object_property_add_uint64_ptr(obj, "icache-size",
                                   &cae_cfg_icache_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "icache-assoc",
                                   &cae_cfg_icache_assoc,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "icache-line-size",
                                   &cae_cfg_icache_line_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "icache-hit-cycles",
                                   &cae_cfg_icache_hit_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "icache-miss-cycles",
                                   &cae_cfg_icache_miss_cycles,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "local-history-bits",
                                   &cae_cfg_local_history_bits,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "btb-entries",
                                   &cae_cfg_btb_entries,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "btb-assoc",
                                   &cae_cfg_btb_assoc,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ras-depth",
                                   &cae_cfg_ras_depth,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "mispredict-penalty-cycles",
                                   &cae_cfg_mispredict_penalty,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-mul",
                                   &cae_cfg_latency_mul,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-div",
                                   &cae_cfg_latency_div,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "latency-fpu",
                                   &cae_cfg_latency_fpu,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "overlap-permille",
                                   &cae_cfg_overlap_permille,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "load-use-stall-cycles",
                                   &cae_cfg_load_use_stall,
                                   OBJ_PROP_FLAG_READWRITE);
    /*
     * Round 49 AC-K-5: OoO-kmhv3 width + regfile forwarders.
     * Applied to the cae-cpu-ooo instance in init_machine only
     * when cpu-model=ooo-kmhv3 is selected; non-OoO cpu-models
     * ignore them.
     */
    object_property_add_uint32_ptr(obj, "ooo-rob-size",
                                   &cae_cfg_ooo_rob_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ooo-lq-size",
                                   &cae_cfg_ooo_lq_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ooo-sq-size",
                                   &cae_cfg_ooo_sq_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ooo-issue-width",
                                   &cae_cfg_ooo_issue_width,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ooo-commit-width",
                                   &cae_cfg_ooo_commit_width,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ooo-rename-width",
                                   &cae_cfg_ooo_rename_width,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ooo-num-phys-int-regs",
                                   &cae_cfg_ooo_num_phys_int_regs,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ooo-num-phys-float-regs",
                                   &cae_cfg_ooo_num_phys_float_regs,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ooo-issue-ports",
                                   &cae_cfg_ooo_issue_ports,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ooo-virtual-issue-window",
                                   &cae_cfg_ooo_virtual_issue_window,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ooo-dependent-load-stall-cycles",
                                   &cae_cfg_ooo_dep_load_stall,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "sentinel-addr",
                                   &cae_cfg_sentinel_addr,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "trace-start-pc",
                                   &cae_cfg_trace_start_pc,
                                   OBJ_PROP_FLAG_READWRITE);
}

static void cae_accel_class_init(ObjectClass *oc, const void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);

    ac->name = "cae";
    ac->init_machine = cae_init_machine;
    ac->allowed = &cae_allowed;
}

static const TypeInfo cae_accel_type = {
    .name = TYPE_CAE_ACCEL,
    .parent = ACCEL_CLASS_NAME("tcg"),
    .instance_init = cae_accel_instance_init,
    .class_init = cae_accel_class_init,
};
module_obj(TYPE_CAE_ACCEL);

static void cae_accel_register_types(void)
{
    type_register_static(&cae_accel_type);
}
type_init(cae_accel_register_types);
