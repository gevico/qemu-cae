#!/usr/bin/env python3
#
# CAE difftest CI gate.
#
# Runs staged checks on the current tree and returns exit 0 only when
# every stage passes. Callers (CI runners, pre-merge hooks) should
# invoke this from the repository root.
#
# Stages implemented:
#   pre_arch_neutral     -- tools/cae/check-cae-arch-neutrality.sh
#   gem5_version_pin     -- tools/cae/check-gem5-version.sh (in-order track)
#   benchmark_manifest   -- verifies sha256 of every tier-1 benchmark
#                           variant in tests/cae/difftest/benchmarks/
#                           MANIFEST.json against the built ELFs; tier-2
#                           entries are skipped at this stage (they
#                           reference host-provided binaries staged by
#                           the operator under $CAE_TIER2_BINARIES_DIR
#                           and are validated at run time by
#                           run-xs-suite.sh --tier 2)
#   config_equivalence   -- runs config-equivalence.py over every paired
#                           YAML in configs/ and fails on any `unsupported`
#                           classification
#   determinism          -- runs determinism-check.sh --mode serial for
#                           the default (config, benchmark) pair selected
#                           via --config/--benchmark (or skipped when CAE
#                           binary / gem5 are unavailable and --strict is
#                           off)
#   diff_threshold       -- reads the most recent
#                           reports/<config>-<benchmark>/accuracy-gate.json
#                           and fails when its `pass` field is false
#   suite                -- aggregates per-benchmark accuracy-gate.json
#                           over the suite's benchmark list, checks max
#                           + geomean rel-error thresholds (YAML-driven
#                           via common.measurement.suite_thresholds; falls
#                           back to the original AC-4 numbers when the
#                           YAML is silent)
#
# Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
#
# This program is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2 or later, as published by the Free Software Foundation.

import argparse
import hashlib
import os
import json
import math
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable


REPO_ROOT = Path(__file__).resolve().parents[5]
DIFFTEST_SRC = REPO_ROOT / "tests" / "cae" / "riscv64" / "difftest"
CAE_BUILD_DIR = Path(os.environ.get("CAE_BUILD_DIR", REPO_ROOT / "build"))
CONFIGS_DIR = DIFFTEST_SRC / "configs"
BENCH_DIR = DIFFTEST_SRC / "benchmarks"
BENCH_BUILD_DIR = CAE_BUILD_DIR / "tests" / "cae" / "difftest" / "benchmarks"
REPORTS_DIR = CAE_BUILD_DIR / "tests" / "cae" / "riscv64" / "difftest" / "reports"
MANIFEST = BENCH_DIR / "MANIFEST.json"
SCRIPTS = DIFFTEST_SRC / "scripts"


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def run_pre_arch_neutral(_args: argparse.Namespace) -> int:
    script = REPO_ROOT / "tools" / "cae" / "check-cae-arch-neutrality.sh"
    if not script.is_file():
        print(f"ci-gate.py: pre_arch_neutral: missing {script}",
              file=sys.stderr)
        return 2
    return subprocess.run([str(script)], cwd=str(REPO_ROOT)).returncode


def run_gem5_version_pin(_args: argparse.Namespace) -> int:
    script = REPO_ROOT / "tools" / "cae" / "check-gem5-version.sh"
    if not script.is_file():
        print(f"ci-gate.py: gem5_version_pin: missing {script}",
              file=sys.stderr)
        return 2
    return subprocess.run([str(script)], cwd=str(REPO_ROOT)).returncode


def run_benchmark_manifest(_args: argparse.Namespace) -> int:
    if not MANIFEST.is_file():
        print(f"ci-gate.py: benchmark_manifest: missing {MANIFEST}",
              file=sys.stderr)
        return 2
    with MANIFEST.open("r") as f:
        manifest = json.load(f)
    rc = 0
    tier1_checked = 0
    tier2_skipped = 0
    for name, entry in manifest.get("benchmarks", {}).items():
        tier = entry.get("tier", 1)
        if tier == 2:
            # Tier-2 binaries live outside the tree; ci-gate.py does
            # not resolve $CAE_TIER2_BINARIES_DIR here. run-xs-suite.sh
            # --tier 2 is responsible for locating + sha256-checking
            # tier-2 binaries at run time.
            tier2_skipped += 1
            continue
        variants = entry.get("variants", {})
        if not variants:
            print(
                f"ci-gate.py: benchmark_manifest: tier-1 entry {name!r} "
                f"has no variants; tier-1 entries must record "
                f"variants.qemu + variants.se with pinned sha256.",
                file=sys.stderr,
            )
            rc = 1
            continue
        for vname, vinfo in variants.items():
            elf = BENCH_BUILD_DIR / vinfo["binary"]
            if not elf.is_file():
                print(
                    f"ci-gate.py: benchmark_manifest: missing {elf} "
                    f"({name}/{vname}); run `make -C {BENCH_DIR/'src'}`",
                    file=sys.stderr,
                )
                rc = 1
                continue
            actual = _sha256(elf)
            if actual != vinfo["sha256"]:
                print(
                    f"ci-gate.py: benchmark_manifest: sha256 drift for "
                    f"{name}/{vname}:\n  file    : {elf}\n"
                    f"  expected: {vinfo['sha256']}\n"
                    f"  actual  : {actual}",
                    file=sys.stderr,
                )
                rc = 1
            else:
                tier1_checked += 1
    if rc == 0:
        print(f"ci-gate.py: benchmark_manifest: {tier1_checked} tier-1 "
              f"variant(s) match manifest; {tier2_skipped} tier-2 "
              f"entry/entries deferred to run-xs-suite.sh")
    return rc


def run_config_equivalence(_args: argparse.Namespace) -> int:
    configs = sorted(CONFIGS_DIR.glob("*.yaml"))
    if not configs:
        print("ci-gate.py: config_equivalence: no *.yaml in "
              f"{CONFIGS_DIR}", file=sys.stderr)
        return 2
    rc = 0
    for cfg in configs:
        res = subprocess.run(
            [sys.executable, str(SCRIPTS / "config-equivalence.py"),
             str(cfg)],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        if res.returncode != 0:
            print(f"ci-gate.py: config_equivalence: {cfg.name} FAILED",
                  file=sys.stderr)
            sys.stderr.write(res.stdout)
            sys.stderr.write(res.stderr)
            rc = res.returncode
    if rc == 0:
        print(f"ci-gate.py: config_equivalence: {len(configs)} config(s) "
              "classify clean")
    return rc


def _default_pair(args: argparse.Namespace) -> tuple[str, str] | None:
    cfg = args.config
    bench = args.benchmark
    if cfg and bench:
        return cfg, bench
    # Default: pick the lexicographically first accuracy-gate.json under
    # reports/. This is intended for the self-check where run-all.sh is
    # the caller and has just produced one pair.
    candidates = sorted(REPORTS_DIR.glob("*/accuracy-gate.json"))
    if not candidates:
        return None
    # reports/<config>-<benchmark>/accuracy-gate.json. Match against
    # the known config stems so hyphenated configs (e.g. atomic-1c)
    # are not mis-split at the first hyphen.
    leaf = candidates[0].parent.name
    config_stems = sorted((cp.stem for cp in CONFIGS_DIR.glob("*.yaml")),
                          key=len, reverse=True)
    for stem in config_stems:
        prefix = f"{stem}-"
        if leaf.startswith(prefix):
            return stem, leaf[len(prefix):]
    # Fallback: treat everything before the LAST hyphen as the config
    # name. This catches ad-hoc report directories whose config YAML
    # may have been removed, without regressing to partition("-").
    config, sep, benchmark = leaf.rpartition("-")
    if not sep or not config or not benchmark:
        return None
    return config, benchmark


def run_determinism(args: argparse.Namespace) -> int:
    pair = _default_pair(args)
    if pair is None:
        msg = ("ci-gate.py: determinism: no <config> <benchmark> supplied "
               "and no report found; pass --config/--benchmark or run "
               "run-all.sh first")
        if args.strict:
            print(msg, file=sys.stderr)
            return 2
        print(f"ci-gate.py: determinism: SKIP ({msg})")
        return 0
    cfg, bench = pair
    script = SCRIPTS / "determinism-check.sh"
    qemu_bin = REPO_ROOT / "build-cae" / "qemu-system-riscv64"
    if not qemu_bin.is_file():
        msg = (f"ci-gate.py: determinism: {qemu_bin} missing; build "
               "build-cae first")
        if args.strict:
            print(msg, file=sys.stderr)
            return 2
        print(f"ci-gate.py: determinism: SKIP ({msg})")
        return 0
    return subprocess.run(
        ["bash", str(script), "--mode", "serial", cfg, bench],
        cwd=str(REPO_ROOT),
    ).returncode


def run_diff_threshold(args: argparse.Namespace) -> int:
    pair = _default_pair(args)
    if pair is None:
        msg = ("ci-gate.py: diff_threshold: no report under "
               f"{REPORTS_DIR}; run `run-all.sh <config> <benchmark>` "
               "first")
        if args.strict:
            print(msg, file=sys.stderr)
            return 2
        print(f"ci-gate.py: diff_threshold: SKIP ({msg})")
        return 0
    cfg, bench = pair
    gate_path = REPORTS_DIR / f"{cfg}-{bench}" / "accuracy-gate.json"
    if not gate_path.is_file():
        msg = f"missing {gate_path}; run run-all.sh first"
        if args.strict:
            print(f"ci-gate.py: diff_threshold: {msg}", file=sys.stderr)
            return 2
        print(f"ci-gate.py: diff_threshold: SKIP ({msg})")
        return 0
    with gate_path.open("r") as f:
        gate = json.load(f)
    # Post-revision reports carry left_backend / right_backend + per-metric
    # {left, right}. Pre-revision reports use cae_backend_variant +
    # per-metric {cae, gem5}. Honor both so a stale on-disk report
    # keeps producing a readable diagnostic until the next run-all.sh
    # regenerates it.
    if "left_backend" in gate:
        left_label = gate["left_backend"]
        right_label = gate["right_backend"]
        left_key, right_key = "left", "right"
    else:
        left_label, right_label = "cae", "gem5"
        left_key, right_key = "cae", "gem5"
    if not gate.get("pass", False):
        print(f"ci-gate.py: diff_threshold: {gate_path} FAILED",
              file=sys.stderr)
        for name, m in gate.get("metrics", {}).items():
            mark = "OK" if m.get("pass") else "FAIL"
            print(f"  {mark}: {name} "
                  f"{left_label}={m.get(left_key)} "
                  f"{right_label}={m.get(right_key)} "
                  f"rel_err={m.get('rel_error_pct'):.3g}% "
                  f"threshold={m.get('threshold_pct'):.3g}%",
                  file=sys.stderr)
        return 1
    print(f"ci-gate.py: diff_threshold: {gate_path} passes all metrics")
    return 0


# Fallback suite thresholds when the paired YAML is silent. These are
# the original AC-4 numbers the in-order track was calibrated against;
# AC-K-8 forbids changing them for inorder-1c via anything other than
# an explicit YAML edit. KMH-V3 configs (xs-1c-*) set their own
# suite_thresholds under common.measurement.
_AC4_BENCHMARKS: tuple[str, ...] = (
    "alu", "mem-stream", "pointer-chase",
    "branchy", "matmul-small", "coremark-1c",
)
_AC4_MAX_IPC_PCT = 10.0
_AC4_GEOMEAN_IPC_PCT = 5.0
_AC4_SECONDARY_PCT = 10.0
_AC4_SECONDARY_RELAX: dict[str, float] = {"pointer-chase": 15.0}
_AC4_SUITE_METRICS: tuple[str, ...] = ("ipc", "mispredict_rate", "l1d_mpki",
                                       "avg_load_latency")


def _load_suite_spec(cfg_name: str) -> dict[str, Any]:
    """Load (benchmarks, metrics, thresholds) from the paired YAML.

    Falls back to the Phase-2 in-order AC-4 numbers when the YAML
    does not declare a ``common.measurement.suite`` or
    ``common.measurement.suite_thresholds`` block. This keeps
    AC-K-8 byte-for-byte compatibility with the pre-revision
    inorder-1c gate while letting the KMH-V3 paired YAMLs (xs-1c-*)
    carry their own numbers explicitly.
    """
    cfg_path = CONFIGS_DIR / f"{cfg_name}.yaml"
    benchmarks = list(_AC4_BENCHMARKS)
    metrics = list(_AC4_SUITE_METRICS)
    max_ipc = _AC4_MAX_IPC_PCT
    geomean_ipc = _AC4_GEOMEAN_IPC_PCT
    secondary = _AC4_SECONDARY_PCT
    secondary_relax = dict(_AC4_SECONDARY_RELAX)

    if cfg_path.is_file():
        sys.path.insert(0, str(SCRIPTS))
        import _pairedyaml as p  # noqa: E402  (local lazy import)
        try:
            cfg = p.load_config(cfg_path)
        except Exception as exc:
            # Preserve old behaviour on a malformed YAML: do not
            # silently succeed, but do not prevent a run on a valid
            # YAML either. Caller prints the AC-4-default fallback.
            print(
                f"ci-gate.py: suite: could not parse {cfg_path} "
                f"({exc}); using AC-4 defaults",
                file=sys.stderr,
            )
            return {
                "benchmarks": benchmarks,
                "metrics": metrics,
                "max_ipc_pct": max_ipc,
                "geomean_ipc_pct": geomean_ipc,
                "secondary_pct": secondary,
                "secondary_relax": secondary_relax,
                "source": "ac4_fallback_parse_error",
            }
        meas = cfg.measurement
        suite = meas.get("suite") or {}
        if "benchmarks" in suite and isinstance(suite["benchmarks"], list):
            benchmarks = list(suite["benchmarks"])
        if "metrics" in suite and isinstance(suite["metrics"], list):
            metrics = list(suite["metrics"])
        thr = meas.get("suite_thresholds") or {}
        if "max_ipc_pct" in thr:
            max_ipc = float(thr["max_ipc_pct"])
        if "geomean_ipc_pct" in thr:
            geomean_ipc = float(thr["geomean_ipc_pct"])
        if "secondary_pct" in thr:
            secondary = float(thr["secondary_pct"])
        if "secondary_relax" in thr and isinstance(thr["secondary_relax"],
                                                   dict):
            secondary_relax = {k: float(v) for k, v
                               in thr["secondary_relax"].items()}
    return {
        "benchmarks": benchmarks,
        "metrics": metrics,
        "max_ipc_pct": max_ipc,
        "geomean_ipc_pct": geomean_ipc,
        "secondary_pct": secondary,
        "secondary_relax": secondary_relax,
        "source": ("yaml" if cfg_path.is_file() else "ac4_fallback"),
    }


def _suite_geomean(values: list[float]) -> float:
    # Geomean of positive rel-errors. Use an additive-epsilon floor so
    # a zero-delta benchmark contributes log(eps) instead of
    # annihilating the product (BL-20260419).
    if not values:
        return 0.0
    eps = 1e-9
    logs = [math.log(max(v, eps)) for v in values]
    return math.exp(sum(logs) / len(logs))


def _metric_is_ipc(name: str) -> bool:
    return name == "ipc"


def run_suite(args: argparse.Namespace) -> int:
    cfg = args.config or "inorder-1c"
    spec = _load_suite_spec(cfg)
    benchmarks = spec["benchmarks"]
    suite_metrics = spec["metrics"]
    max_ipc = spec["max_ipc_pct"]
    geomean_ipc = spec["geomean_ipc_pct"]
    secondary_pct = spec["secondary_pct"]
    secondary_relax = spec["secondary_relax"]

    missing: list[str] = []
    per_bench: dict[str, dict] = {}
    for bench in benchmarks:
        gate = REPORTS_DIR / f"{cfg}-{bench}" / "accuracy-gate.json"
        if not gate.is_file():
            missing.append(bench)
            continue
        with gate.open("r") as f:
            per_bench[bench] = json.load(f)
    if missing:
        print(f"ci-gate.py: suite: missing accuracy-gate.json for "
              f"benchmarks {missing} under {REPORTS_DIR}; run the full "
              f"run-suite.sh / run-xs-suite.sh first", file=sys.stderr)
        return 2

    # Collect per-benchmark rel errors for each metric in the suite.
    # "skip" entries (dropped below noise floor by diff.py) do not
    # contribute to max or geomean.
    failed = False
    metric_errors: dict[str, list[tuple[str, float]]] = {}
    for bench, gate in per_bench.items():
        for m_name, m_body in gate.get("metrics", {}).items():
            if m_name not in suite_metrics:
                continue
            if m_body.get("skip"):
                continue
            metric_errors.setdefault(m_name, []).append(
                (bench, float(m_body.get("rel_error_pct", 0.0)))
            )

    print(f"ci-gate.py: suite ({cfg}, thresholds from {spec['source']}): "
          f"evaluating {len(benchmarks)} benchmarks, "
          f"{len(metric_errors)} metric(s)")
    for m_name, points in sorted(metric_errors.items()):
        values = [v for _, v in points]
        mx = max(values) if values else 0.0
        gm = _suite_geomean(values)
        mx_bench = max(points, key=lambda pv: pv[1])[0] if points else ""
        if _metric_is_ipc(m_name):
            max_t = max_ipc
            gmean_t = geomean_ipc
        else:
            max_t = secondary_pct
            gmean_t = secondary_pct
        for b, v in points:
            if _metric_is_ipc(m_name):
                bench_threshold = max_t
            else:
                bench_threshold = secondary_relax.get(b, max_t)
            ok = v <= bench_threshold
            marker = "OK" if ok else "FAIL"
            print(f"  {marker}: {m_name:18s} {b:14s} "
                  f"rel_err={v:6.2f}% threshold={bench_threshold:5.1f}%")
            if not ok:
                failed = True
        if gm > gmean_t:
            print(f"  FAIL: {m_name:18s} suite-geomean={gm:6.2f}% "
                  f"threshold={gmean_t:5.1f}%", file=sys.stderr)
            failed = True
        else:
            print(f"  OK:   {m_name:18s} suite-geomean={gm:6.2f}% "
                  f"threshold={gmean_t:5.1f}%")
        if mx > max_t and _metric_is_ipc(m_name):
            print(f"  (ipc max = {mx:.2f}% from {mx_bench})",
                  file=sys.stderr)

    return 1 if failed else 0


StageFn = Callable[[argparse.Namespace], int]

STAGES: list[tuple[str, StageFn]] = [
    ("pre_arch_neutral", run_pre_arch_neutral),
    ("gem5_version_pin", run_gem5_version_pin),
    ("benchmark_manifest", run_benchmark_manifest),
    ("config_equivalence", run_config_equivalence),
    ("determinism", run_determinism),
    ("diff_threshold", run_diff_threshold),
    ("suite", run_suite),
]


def main() -> int:
    ap = argparse.ArgumentParser(
        description="CAE difftest CI gate (staged pre-merge checks).",
    )
    ap.add_argument(
        "--stage",
        choices=[name for name, _ in STAGES] + ["all"],
        default="all",
        help="Run only the named stage, or 'all' (default).",
    )
    ap.add_argument("--config",
                    help="Paired YAML name (for determinism/diff/suite "
                         "stages)")
    ap.add_argument("--benchmark",
                    help="Benchmark name (for determinism/diff stages)")
    ap.add_argument("--strict", action="store_true",
                    help="Fail when determinism/diff_threshold cannot "
                         "locate their inputs instead of skipping")
    args = ap.parse_args()

    stages = STAGES if args.stage == "all" else [
        (name, fn) for name, fn in STAGES if name == args.stage
    ]

    for name, fn in stages:
        rc = fn(args)
        if rc != 0:
            print(f"ci-gate.py: stage '{name}' failed (exit {rc})",
                  file=sys.stderr)
            return rc

    print("ci-gate.py: all stages passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
