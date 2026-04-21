#!/usr/bin/env python3
"""Aggregate per-side wallclock for a KMH-V3 suite run.

AC-K-7 requires every difftest run to produce
`tests/cae/riscv64/difftest/reports/performance-report-xs.json` with per-
benchmark CAE / XS-GEM5 / NEMU wallclock plus host info. This
script reads the already-emitted per-benchmark JSON reports
(cae.json, xs-gem5.json, nemu-itrace-stats.json) under a
per-suite reports directory, pulls each side's
`wallclock_seconds`, and writes the aggregated JSON file.

M5' or earlier: report-only (not gated). M6' (if implemented):
geomean(CAE / XS-GEM5 wallclock) gated per AC-K-7's downstream
threshold.

Usage:
    perf-report.py --config xs-1c-functional \\
                   --reports-dir tests/cae/riscv64/difftest/reports \\
                   --output tests/cae/riscv64/difftest/reports/performance-report-xs.json
"""
from __future__ import annotations

import argparse
import json
import platform
import socket
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _pairedyaml as p  # noqa: E402


def _load_side(path: Path) -> dict:
    if not path.is_file():
        return {}
    try:
        with path.open("r") as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError):
        return {}


def _host_info() -> dict:
    return {
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "python_version": platform.python_version(),
    }


def _suite_benchmarks(config: str) -> list[str]:
    """Return the benchmark list from the paired YAML's
    `common.measurement.suite.benchmarks`.

    Round 11: perf-report was previously glob-aggregating every
    `<config>-*` directory under `reports/`, which kept stale
    pre-suite-narrowing entries (e.g. `branchy`, `coremark-1c`,
    `matmul-small`) in `performance-report-xs.json` even after
    the suite list was narrowed to `alu + mem-stream +
    pointer-chase`. Reading the YAML suite directly keeps the
    aggregate surface in sync with the authoritative config.

    Returns an empty list if the YAML has no measurement.suite
    stanza; callers then fall back to glob mode for backwards
    compatibility with configs that predate the suite field.
    """
    try:
        cfg = p.load_config(config)
    except (FileNotFoundError, KeyError, ValueError):
        return []
    suite = cfg.measurement.get("suite", {}) or {}
    benchmarks = suite.get("benchmarks", []) or []
    return [str(b) for b in benchmarks]


def aggregate(config: str, reports_dir: Path) -> dict:
    entries: list[dict] = []
    suite = _suite_benchmarks(config)
    if suite:
        bench_source = "yaml.measurement.suite.benchmarks"
        bench_names = suite
    else:
        # Fallback for configs without a suite stanza: glob mode.
        bench_source = "glob"
        bench_names = sorted(
            sd.name[len(config) + 1:]
            for sd in reports_dir.glob(f"{config}-*")
            if sd.is_dir()
        )

    for bench in bench_names:
        sd = reports_dir / f"{config}-{bench}"
        cae = _load_side(sd / "cae.json")
        xs = _load_side(sd / "xs-gem5.json")
        nemu = _load_side(sd / "nemu-itrace-stats.json")
        entry = {
            "benchmark": bench,
            "cae_wallclock_seconds": cae.get("wallclock_seconds"),
            "xs_gem5_wallclock_seconds": xs.get("wallclock_seconds"),
            "nemu_wallclock_seconds": nemu.get("wallclock_seconds"),
            "cae_present": bool(cae),
            "xs_gem5_present": bool(xs),
            "nemu_present": bool(nemu),
        }
        entries.append(entry)

    return {
        "schema_version": 1,
        "config": config,
        "host": _host_info(),
        "benchmark_source": bench_source,
        "per_benchmark": entries,
        "benchmark_count": len(entries),
        "note": ("AC-K-7: report-only until M6' lands. geomean gate "
                 "activates with t-bound-weave once CAE / XS-GEM5 "
                 "wallclock ratios become meaningful."),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", required=True,
                    help="Paired YAML name, e.g. xs-1c-functional")
    ap.add_argument("--reports-dir", type=Path, required=True,
                    help="Suite reports directory "
                         "(tests/cae/riscv64/difftest/reports)")
    ap.add_argument("--output", type=Path, required=True,
                    help="Where to write performance-report-xs.json")
    args = ap.parse_args()

    if not args.reports_dir.is_dir():
        print(f"perf-report: reports-dir not a directory: "
              f"{args.reports_dir}", file=sys.stderr)
        return 1

    report = aggregate(args.config, args.reports_dir)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2,
                                      sort_keys=False) + "\n")
    print(f"perf-report: wrote {args.output} "
          f"({report['benchmark_count']} benchmark(s) aggregated)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
