#!/usr/bin/env python3
"""Classify every field in a paired YAML as mapped/backend_only/unsupported.

Per the v1 schema contract:
  - common.*         -> mapped (must be honored by every backend)
  - gem5_only.*      -> backend_only from CAE/XS-GEM5/NEMU; mapped on
                        gem5 if the selected cpu_type can honor it
  - xs_gem5_only.*   -> backend_only from CAE/gem5/NEMU; mapped on
                        XS-GEM5 if the selected cpu_type can honor it
  - nemu_only.*      -> backend_only from CAE/gem5/XS-GEM5; mapped on
                        NEMU if consumed by the NEMU driver
  - cae_only.*       -> backend_only from gem5/XS-GEM5/NEMU; mapped on
                        CAE if the selected variant can honor it

Fields under *_only that are not meaningful for the selected backend
variant (for example, `gem5_only.cpu.o3.rob_entries` with
`cpu_type: AtomicCPU`) are classified `unsupported` and MUST fail the
gate. Fields CAE has not implemented yet (Phase-1: every
`cae_only.cache_config` / `cae_only.bpred_config` / `cae_only.ooo_config`
field) are also `unsupported` until the model lands.

Output JSON shape:

    {
      "config": "...",
      "entries": [
        {"path": "common.cpu.clock_freq_hz", "classification": "mapped",
         "reason": "common field honored by both backends"},
        {"path": "gem5_only.memory.dram_latency_ns",
         "classification": "backend_only",
         "reason": "gem5 TimingSimpleCPU maps this, CAE ignores"},
        ...
      ],
      "counts": {"mapped": N, "backend_only": N, "unsupported": N},
      "pass": <bool>    # false if any `unsupported` entry exists
    }
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _pairedyaml as p  # noqa: E402


# Field classification is driven by an explicit "who consumes what"
# table rather than a free-text allow-list, so a leaf that exists in
# YAML but is dropped by the runner becomes `unsupported`, not
# `backend_only`. This is what AC-2's negative test depends on.

# gem5_only leaves consumed by run-gem5.py + _gem5-se-config.py today.
# Exact match required; prefix match handled separately below via
# wildcards (e.g. "common.contract.*" means any deeper leaf).
GEM5_CONSUMED_FIELDS: set[str] = {
    "gem5_only.runtime.mode",
    "gem5_only.runtime.target_isa",
    "gem5_only.cpu.cpu_type",
    "gem5_only.memory.mem_mode",
    "gem5_only.memory.dram_latency_ns",
    "gem5_only.memory.response_latency_ns",
    "gem5_only.memory.l1i.size",
    "gem5_only.memory.l1i.assoc",
    "gem5_only.memory.l1i.latency_hit_cycles",
    "gem5_only.memory.l1i.latency_miss_cycles",
    "gem5_only.memory.l1d.size",
    "gem5_only.memory.l1d.assoc",
    "gem5_only.memory.l1d.latency_hit_cycles",
    "gem5_only.memory.l1d.latency_miss_cycles",
}

# xs_gem5_only leaves that the future run-xs-gem5.py driver will plumb
# through to configs/example/kmhv3.py. Populated forward from the plan's
# AC-K-5 parameter list so that xs-1c-*.yaml configs can declare these
# knobs without tripping config-equivalence.py before the driver lands.
# Anything under xs_gem5_only.* not listed here is classified
# `unsupported` and must either be wired into run-xs-gem5.py or dropped
# from the YAML — the same discipline GEM5_CONSUMED_FIELDS enforces.
XS_GEM5_CONSUMED_FIELDS: set[str] = {
    "xs_gem5_only.runtime.run_mode",
    "xs_gem5_only.runtime.target_isa",
    "xs_gem5_only.runtime.disable_difftest",
    "xs_gem5_only.runtime.raw_cpt",
    "xs_gem5_only.runtime.generic_rv_cpt",
    "xs_gem5_only.cpu.cpu_type",
    "xs_gem5_only.cpu.fetch_width",
    "xs_gem5_only.cpu.decode_width",
    "xs_gem5_only.cpu.rename_width",
    "xs_gem5_only.cpu.commit_width",
    "xs_gem5_only.cpu.issue_width",
    "xs_gem5_only.cpu.rob_size",
    "xs_gem5_only.cpu.lq_size",
    "xs_gem5_only.cpu.sq_size",
    "xs_gem5_only.cpu.num_phys_int_regs",
    "xs_gem5_only.cpu.num_phys_float_regs",
    "xs_gem5_only.cpu.sbuffer_entries",
    "xs_gem5_only.cpu.bpred.predictor",
    # Round 13 M4' realspec: the "tage_sc_l" bool flips on the
    # XS-GEM5 DecoupledBPU's TAGE-SC-L tagged-predictor path so
    # the paired CAE bpred-model=tage-sc-l has a structurally
    # matching XS-side configuration. Same classification as the
    # other bpred.* knobs — XS-only, not propagated to the
    # offline-trace comparison.
    "xs_gem5_only.cpu.bpred.tage_sc_l",
    "xs_gem5_only.cpu.bpred.ftq_size",
    "xs_gem5_only.cpu.bpred.fsq_size",
    "xs_gem5_only.memory.mem_mode",
    "xs_gem5_only.memory.dram_latency_ns",
    "xs_gem5_only.memory.response_latency_ns",
    "xs_gem5_only.memory.l1i.size",
    "xs_gem5_only.memory.l1i.assoc",
    "xs_gem5_only.memory.l1i.latency_hit_cycles",
    "xs_gem5_only.memory.l1i.latency_miss_cycles",
    "xs_gem5_only.memory.l1d.size",
    "xs_gem5_only.memory.l1d.assoc",
    "xs_gem5_only.memory.l1d.latency_hit_cycles",
    "xs_gem5_only.memory.l1d.latency_miss_cycles",
    "xs_gem5_only.memory.l2.size",
    "xs_gem5_only.memory.l2.assoc",
    "xs_gem5_only.memory.l2.latency_hit_cycles",
    # Round 13: xs-1c-realspec adds an L3 tier on the XS side
    # (kmhv3.py realspec baseline). L3 is XS-only at this tier;
    # CAE's memory-model=mshr does not model a third-level cache
    # yet (t-sbuffer-bank-conflict / M5' calibration brings it
    # in). Classified backend_only like the other XS memory
    # knobs.
    "xs_gem5_only.memory.l3.size",
    "xs_gem5_only.memory.l3.assoc",
    "xs_gem5_only.memory.l3.latency_hit_cycles",
}

# nemu_only leaves consumed by the future run-nemu-ref.sh +
# build-nemu-ref.sh + nemu-difftest.py drivers. Also forward-populated
# so xs-1c-*.yaml configs can declare NEMU-side knobs in advance.
NEMU_CONSUMED_FIELDS: set[str] = {
    "nemu_only.runtime.defconfig",
    "nemu_only.runtime.itrace_out",
    "nemu_only.runtime.itrace_mode",
    "nemu_only.runtime.checkpoint_interval",
    "nemu_only.runtime.full_trace",
    "nemu_only.runtime.build_dir",
}

# cae_only leaves consumed by run-cae.py's accel-property translation
# (see _CAE_ACCEL_FIELD_MAP in scripts/run-cae.py). Anything listed
# here is "mapped"; anything under cae_only.* not listed is
# "unsupported" and blocks ci-gate.py.
CAE_CONSUMED_FIELDS: set[str] = {
    "cae_only.cpu.cpu_model",
    "cae_only.cpu.latency_mul",
    "cae_only.cpu.latency_div",
    "cae_only.cpu.latency_fpu",
    "cae_only.cpu.overlap_permille",
    "cae_only.cpu.load_use_stall_cycles",
    # Round 49 AC-K-5: OoO-kmhv3 width + regfile forwarders,
    # mapped to cae-accel class properties ooo-rob-size /
    # ooo-lq-size / ooo-sq-size / ooo-issue-width /
    # ooo-commit-width / ooo-rename-width / ooo-num-phys-int-regs
    # / ooo-num-phys-float-regs. Consumed by cae-all.c when
    # cpu_model=ooo-kmhv3.
    "cae_only.cpu.rob_size",
    "cae_only.cpu.lq_size",
    "cae_only.cpu.sq_size",
    "cae_only.cpu.issue_width",
    "cae_only.cpu.commit_width",
    "cae_only.cpu.rename_width",
    "cae_only.cpu.num_phys_int_regs",
    "cae_only.cpu.num_phys_float_regs",
    "cae_only.cpu.issue_ports",
    "cae_only.cpu.virtual_issue_window",
    "cae_only.cpu.dependent_load_stall_cycles",
    "cae_only.bpred.bpred_model",
    "cae_only.bpred.local_history_bits",
    "cae_only.bpred.btb_entries",
    "cae_only.bpred.btb_assoc",
    "cae_only.bpred.ras_depth",
    "cae_only.bpred.mispredict_penalty_cycles",
    "cae_only.memory.memory_model",
    "cae_only.memory.l1.size_bytes",
    "cae_only.memory.l1.assoc",
    "cae_only.memory.l1.line_size",
    "cae_only.memory.l1.latency_hit_cycles",
    "cae_only.memory.l1.latency_miss_cycles",
    "cae_only.memory.dram.read_cycles",
    "cae_only.memory.dram.write_cycles",
    # Round 14 BS-34: MSHR knobs consumed by run-cae.py via
    # _CAE_ACCEL_FIELD_MAP "memory.mshr.{mshr_size, fill_queue_
    # size, writeback_queue_size}". The xs-1c-realspec YAML
    # drives these to the kmhv3.py targets; config-equivalence
    # needs to recognise them as mapped CAE fields instead of
    # unsupported leaves.
    "cae_only.memory.mshr.mshr_size",
    "cae_only.memory.mshr.fill_queue_size",
    "cae_only.memory.mshr.writeback_queue_size",
    # Round 50 AC-K-5: L1D bank-conflict + sbuffer evict-threshold
    # knobs. bank_count / bank_conflict_stall_cycles feed the
    # cae-cache-mshr per-bank last-cycle tracker;
    # sbuffer_evict_threshold feeds cae-cpu-ooo's
    # sbuffer-evict-threshold child-forwarder. run-cae.py maps
    # these onto the accel-class bank-count /
    # bank-conflict-stall-cycles / sbuffer-evict-threshold
    # properties (added round 50 in cae-all.c).
    "cae_only.memory.mshr.bank_count",
    "cae_only.memory.mshr.bank_conflict_stall_cycles",
    "cae_only.memory.sbuffer_evict_threshold",
    "cae_only.memory.tlb_miss_cycles",
    # Round 18 t-icache: separate instruction-cache knobs
    # consumed by run-cae.py via _CAE_ACCEL_FIELD_MAP
    # "memory.l1i.*" (round 18 extension) which map to the
    # accel-class icache-{size,assoc,line-size,hit-cycles,
    # miss-cycles} properties. xs-1c-realspec's `l1i` CAE
    # block is the first consumer; kmhv3.yaml (M5') will be
    # the next.
    "cae_only.memory.l1i.size_bytes",
    "cae_only.memory.l1i.assoc",
    "cae_only.memory.l1i.line_size",
    "cae_only.memory.l1i.latency_hit_cycles",
    "cae_only.memory.l1i.latency_miss_cycles",
    # Round 41 directive step 5: optional deterministic
    # speculative-memory stimulus program, forwarded by
    # run-cae.py via QMP qom-set on /objects/cae-engine.
    # Empty/missing means no auto-queue. Format:
    # "<op>:<addr>:<bytes>[:<value>][;...]".
    "cae_only.spec_stimulus_program",
}

# XS-GEM5 CPU-variant-restricted fields: similar semantics to
# GEM5_CPU_VARIANT_FIELDS below, but driven by the xs_gem5_only.cpu.cpu_type
# (default DerivO3CPU). For M3'-bootstrap this stays permissive — the
# KMH-V3 build ships a customized O3 variant called "DerivO3CPU" that
# honors every field under xs_gem5_only.cpu. If a future revision adds
# a slimmer variant (e.g. for single-issue comparison), restrictions
# land here alongside the launcher change.
XS_GEM5_CPU_VARIANT_FIELDS: dict[str, set[str]] = {
    "xs_gem5_only.cpu.rob_size":               {"DerivO3CPU"},
    "xs_gem5_only.cpu.lq_size":                {"DerivO3CPU"},
    "xs_gem5_only.cpu.sq_size":                {"DerivO3CPU"},
    "xs_gem5_only.cpu.num_phys_int_regs":      {"DerivO3CPU"},
    "xs_gem5_only.cpu.num_phys_float_regs":    {"DerivO3CPU"},
    "xs_gem5_only.cpu.sbuffer_entries":        {"DerivO3CPU"},
    "xs_gem5_only.cpu.issue_width":            {"DerivO3CPU"},
    "xs_gem5_only.cpu.rename_width":           {"DerivO3CPU"},
    "xs_gem5_only.cpu.commit_width":           {"DerivO3CPU"},
}


# gem5_only fields that require a specific cpu_type. Classified as
# unsupported when the cpu_type in the current YAML does not include
# the field's allowed set.
GEM5_CPU_VARIANT_FIELDS: dict[str, set[str]] = {
    "gem5_only.cpu.o3.rob_entries":     {"O3CPU"},
    "gem5_only.cpu.o3.iq_entries":      {"O3CPU"},
    "gem5_only.cpu.o3.lsq_entries":     {"O3CPU"},
    "gem5_only.cpu.minor.fetch1_to_fetch2_forward_delay_cycles":
                                        {"MinorCPU"},
    "gem5_only.cpu.minor.execute_to_commit_limit":
                                        {"MinorCPU"},
    "gem5_only.memory.mem_type":        {"TimingSimpleCPU", "MinorCPU",
                                         "O3CPU"},
    "gem5_only.memory.bandwidth_bytes_per_s":
                                        {"TimingSimpleCPU", "MinorCPU",
                                         "O3CPU"},
    # L1/dram_latency need mem_mode=timing; TimingSimpleCPU / MinorCPU /
    # O3CPU are the supported carriers.
    "gem5_only.memory.dram_latency_ns": {"TimingSimpleCPU", "MinorCPU",
                                         "O3CPU"},
    "gem5_only.memory.l1i.size":        {"TimingSimpleCPU", "MinorCPU",
                                         "O3CPU"},
    "gem5_only.memory.l1i.assoc":       {"TimingSimpleCPU", "MinorCPU",
                                         "O3CPU"},
    "gem5_only.memory.l1i.latency_hit_cycles":
                                        {"TimingSimpleCPU", "MinorCPU",
                                         "O3CPU"},
    "gem5_only.memory.l1i.latency_miss_cycles":
                                        {"TimingSimpleCPU", "MinorCPU",
                                         "O3CPU"},
    "gem5_only.memory.l1d.size":        {"TimingSimpleCPU", "MinorCPU",
                                         "O3CPU"},
    "gem5_only.memory.l1d.assoc":       {"TimingSimpleCPU", "MinorCPU",
                                         "O3CPU"},
    "gem5_only.memory.l1d.latency_hit_cycles":
                                        {"TimingSimpleCPU", "MinorCPU",
                                         "O3CPU"},
    "gem5_only.memory.l1d.latency_miss_cycles":
                                        {"TimingSimpleCPU", "MinorCPU",
                                         "O3CPU"},
}

# CAE fields we have NOT implemented yet in Phase-1. M2+ will move these
# out as they land.
CAE_UNIMPLEMENTED_PREFIXES: tuple[str, ...] = (
    "cae_only.cache_config",
    "cae_only.bpred_config",
    "cae_only.ooo_config",
)


def _classify(path: str, value: Any, cfg: p.PairedConfig
              ) -> tuple[str, str]:
    if path in p.VALID_ROOTS and value == {}:
        # Empty root namespace: classify as mapped so the gate doesn't
        # fail just because a YAML writer left xs_gem5_only: {} as a
        # placeholder. The namespace itself isn't a field.
        return "mapped", "empty root namespace is valid placeholder"

    if path.startswith("common."):
        return "mapped", "common field honored by every backend"

    if path.startswith("gem5_only."):
        # Variant-restricted field: unsupported when the selected
        # cpu_type can't honor it.
        allowed = GEM5_CPU_VARIANT_FIELDS.get(path)
        if allowed is not None and cfg.gem5_cpu_type not in allowed:
            return (
                "unsupported",
                f"field requires cpu_type in {sorted(allowed)} but this "
                f"config selects {cfg.gem5_cpu_type!r}",
            )
        if value == {}:
            return (
                "backend_only",
                "gem5-only placeholder namespace; not a real field",
            )
        if path not in GEM5_CONSUMED_FIELDS:
            return (
                "unsupported",
                "gem5_only leaf exists in YAML but is not consumed by "
                "run-gem5.py / _gem5-se-config.py; either wire it up "
                "or drop it from the YAML before merging",
            )
        return (
            "backend_only",
            f"gem5-only field (cpu_type={cfg.gem5_cpu_type}); "
            "classified backend_only on CAE / XS-GEM5 / NEMU",
        )

    if path.startswith("xs_gem5_only."):
        allowed = XS_GEM5_CPU_VARIANT_FIELDS.get(path)
        if allowed is not None and cfg.xs_gem5_cpu_type not in allowed:
            return (
                "unsupported",
                f"field requires cpu_type in {sorted(allowed)} but this "
                f"config selects {cfg.xs_gem5_cpu_type!r}",
            )
        if value == {}:
            return (
                "backend_only",
                "xs-gem5-only placeholder namespace; not a real field",
            )
        if path not in XS_GEM5_CONSUMED_FIELDS:
            return (
                "unsupported",
                "xs_gem5_only leaf exists in YAML but is not consumed by "
                "run-xs-gem5.py; either wire it up or drop it from the "
                "YAML before merging",
            )
        return (
            "backend_only",
            f"xs-gem5-only field (cpu_type={cfg.xs_gem5_cpu_type}); "
            "classified backend_only on CAE / gem5 / NEMU",
        )

    if path.startswith("nemu_only."):
        if value == {}:
            return (
                "backend_only",
                "nemu-only placeholder namespace; not a real field",
            )
        if path not in NEMU_CONSUMED_FIELDS:
            return (
                "unsupported",
                "nemu_only leaf exists in YAML but is not consumed by "
                "run-nemu-ref.sh / build-nemu-ref.sh / "
                "nemu-difftest.py; either wire it up or drop it from "
                "the YAML before merging",
            )
        return (
            "backend_only",
            "NEMU-only field; classified backend_only on CAE / gem5 / "
            "XS-GEM5",
        )

    if path.startswith("cae_only."):
        for prefix in CAE_UNIMPLEMENTED_PREFIXES:
            if path == prefix or path.startswith(prefix + "."):
                return (
                    "unsupported",
                    "CAE has not implemented this field yet; populating "
                    "it prematurely is a plan drift",
                )
        if value == {}:
            return (
                "backend_only",
                "CAE-only placeholder (empty map); not yet populated",
            )
        if path not in CAE_CONSUMED_FIELDS:
            return (
                "unsupported",
                "cae_only leaf exists in YAML but is not consumed by "
                "run-cae.py; either wire it up or drop it from the "
                "YAML before merging",
            )
        return (
            "backend_only",
            "CAE-only field; classified backend_only on the reference "
            "backends",
        )

    return ("unsupported",
            f"field outside the valid roots {list(p.VALID_ROOTS)}")


def classify_config(cfg: p.PairedConfig) -> dict[str, object]:
    raw: dict[str, Any] = {
        "common": cfg.common,
        "gem5_only": cfg.gem5_only,
        "xs_gem5_only": cfg.xs_gem5_only,
        "nemu_only": cfg.nemu_only,
        "cae_only": cfg.cae_only,
    }
    entries: list[dict[str, object]] = []
    counts = {"mapped": 0, "backend_only": 0, "unsupported": 0}

    for root in p.VALID_ROOTS:
        for path, value in p.flatten(root, raw[root]):
            classification, reason = _classify(path, value, cfg)
            entries.append({
                "path": path,
                "value": value,
                "classification": classification,
                "reason": reason,
            })
            counts[classification] += 1

    left_label, right_label = cfg.timing_pair
    return {
        "config": cfg.common["contract"]["config_name"],
        "timing_pair": [left_label, right_label],
        "gem5_cpu_type": cfg.gem5_cpu_type,
        "xs_gem5_cpu_type": cfg.xs_gem5_cpu_type,
        "entries": entries,
        "counts": counts,
        "pass": counts["unsupported"] == 0,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("config")
    ap.add_argument("--output", type=Path)
    args = ap.parse_args()

    cfg = p.load_config(args.config)
    report = classify_config(cfg)

    text = json.dumps(report, indent=2, sort_keys=False)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n")
    else:
        print(text)
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
