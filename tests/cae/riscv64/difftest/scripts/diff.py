#!/usr/bin/env python3
"""Compare two timing-backend JSON reports against a shared paired YAML.

The two backends are declared in the YAML as
``common.contract.timing_pair: [left, right]``. Defaults to
``["cae", "gem5"]`` for backward compatibility with the Phase-2
in-order track; KMH-V3 configs set e.g. ``["cae", "xs-gem5"]`` to
drive the CAE-vs-XiangShan-GEM5 timing comparison. Functional
comparison (retired-instruction trace / architectural-state
checkpoint vs NEMU) is **not** done by this script — that lives in
``nemu-difftest.py`` and is a hard prerequisite for the timing gate.

Output:

    <report_dir>/diff-<config>-<benchmark>.md     (markdown summary)
    <report_dir>/accuracy-gate.json               (machine-readable gate)

accuracy-gate.json shape:

    {
      "config": "inorder-1c",
      "benchmark": "alu",
      "left_backend":  "cae",
      "right_backend": "gem5",
      "thresholds": {"ipc": 2.0},
      "metrics": {
        "ipc": {
          "left": 1.0,
          "right": 0.833,
          "rel_error_pct": 20.0,
          "threshold_pct": 2.0,
          "pass": false
        },
        ...
      },
      "pass": false
    }

The ``pass`` bit is the bottom-line gate. ``ci-gate.py`` reads this
file in the diff_threshold stage.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _pairedyaml as p  # noqa: E402


def _load_side(path: Path, expected_backend: str) -> dict[str, object]:
    if not path.is_file():
        raise FileNotFoundError(f"diff.py: report not found: {path}")
    with path.open("r") as f:
        data = json.load(f)
    if "backend" not in data:
        raise ValueError(f"{path}: missing 'backend' field")
    if data["backend"] != expected_backend:
        raise ValueError(
            f"diff.py: expected backend={expected_backend!r} at {path}, "
            f"got {data['backend']!r}"
        )
    return data


def _rel_error_pct(left: float, right: float) -> float:
    if right == 0:
        return float("inf") if left != 0 else 0.0
    return abs(left - right) / abs(right) * 100.0


def _render_markdown(gate: dict, left: dict, right: dict) -> str:
    ll = gate["left_backend"]
    rl = gate["right_backend"]
    lines = [
        f"# Difftest report: {gate['config']} / {gate['benchmark']}",
        "",
        f"- {ll} variant  : `{left['backend_variant']}`",
        f"- {rl} variant  : `{right['backend_variant']}`",
        f"- clock         : {left['clock_freq_hz']:,} Hz",
        f"- num_cpus      : {ll}={left['num_cpus']} {rl}={right['num_cpus']}",
        "",
        "## Metrics",
        "",
        f"| metric | {ll} | {rl} | rel. error (%) | threshold (%) | pass |",
        "| --- | ---: | ---: | ---: | ---: | :---: |",
    ]
    for name, m in gate["metrics"].items():
        mark = "OK" if m["pass"] else "FAIL"
        lines.append(
            f"| {name} | {m['left']:.6g} | {m['right']:.6g} | "
            f"{m['rel_error_pct']:.4g} | {m['threshold_pct']:.4g} | {mark} |"
        )
    lines.extend([
        "",
        f"**Gate: {'PASS' if gate['pass'] else 'FAIL'}**",
        "",
        f"{ll} wallclock: {left.get('wallclock_seconds', 0):.3f} s  |  "
        f"{rl} wallclock: {right.get('wallclock_seconds', 0):.3f} s",
        "",
    ])
    return "\n".join(lines)


# Per-metric extractor tables. Same for every backend: each report
# declares its metrics under ``aggregate.*`` keyed by canonical name.
_KNOWN_METRICS: dict[str, tuple[str, str]] = {
    "ipc":              ("aggregate", "ipc"),
    "cycles":           ("aggregate", "total_cycles"),
    "total_cycles":     ("aggregate", "total_cycles"),
    "insns":            ("aggregate", "total_insns"),
    "total_insns":      ("aggregate", "total_insns"),
    "mispredict_rate":  ("aggregate", "mispredict_rate"),
    "l1d_mpki":         ("aggregate", "l1d_mpki"),
    "avg_load_latency": ("aggregate", "avg_load_latency"),
}


def compute_gate(left: dict, right: dict, cfg: p.PairedConfig,
                 left_label: str, right_label: str) -> dict:
    if left["config_name"] != right["config_name"]:
        raise ValueError(
            f"diff.py: config mismatch: {left_label}="
            f"{left['config_name']!r}, {right_label}="
            f"{right['config_name']!r}"
        )
    if left["benchmark"] != right["benchmark"]:
        raise ValueError(
            f"diff.py: benchmark mismatch: {left_label}="
            f"{left['benchmark']!r}, {right_label}="
            f"{right['benchmark']!r}"
        )

    thresholds = cfg.measurement.get("thresholds", {})
    metrics_to_check = cfg.measurement.get("metrics", ["cycles", "insns",
                                                       "ipc"])
    # Per-metric noise floor: when both backends report absolute
    # magnitudes below the floor, per-benchmark rel-err is meaningless
    # (div-by-near-zero) and the entry is marked "skip" so the suite
    # gate excludes it (see
    # BL-20260418-noise-floor-metric-gate).
    noise_floor = cfg.measurement.get("noise_floor", {}) or {}

    metrics: dict[str, dict[str, object]] = {}
    overall_pass = True
    default_threshold = thresholds.get("ipc", 2.0)

    for name in metrics_to_check:
        mapping = _KNOWN_METRICS.get(name)
        if mapping is None:
            raise ValueError(
                f"diff.py: unknown metric '{name}' in config "
                f"'{left['config_name']}' measurement.metrics "
                f"(supported: {sorted(_KNOWN_METRICS)})"
            )
        sect, key = mapping
        if key not in left.get(sect, {}):
            raise ValueError(
                f"diff.py: {left_label} report missing {sect}.{key} for "
                f"metric '{name}'"
            )
        if key not in right.get(sect, {}):
            raise ValueError(
                f"diff.py: {right_label} report missing {sect}.{key} for "
                f"metric '{name}'"
            )
        left_v = float(left[sect][key])
        right_v = float(right[sect][key])

        threshold = float(thresholds.get(name, default_threshold))
        floor = float(noise_floor.get(name, 0.0))
        if floor > 0.0 and abs(left_v) < floor and abs(right_v) < floor:
            metrics[name] = {
                "left": left_v,
                "right": right_v,
                "rel_error_pct": 0.0,
                "threshold_pct": threshold,
                "noise_floor": floor,
                "skip": True,
                "pass": True,
            }
            continue

        err = _rel_error_pct(left_v, right_v)
        ok = err <= threshold
        if not ok:
            overall_pass = False
        metrics[name] = {
            "left": left_v,
            "right": right_v,
            "rel_error_pct": err,
            "threshold_pct": threshold,
            "pass": ok,
        }

    return {
        "config": left["config_name"],
        "benchmark": left["benchmark"],
        "left_backend": left_label,
        "right_backend": right_label,
        "left_backend_variant": left["backend_variant"],
        "right_backend_variant": right["backend_variant"],
        "thresholds": {k: float(v) for k, v in thresholds.items()},
        "metrics": metrics,
        "pass": overall_pass,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    # New N-side interface: --left/--right plus labels inferred from
    # the paired YAML's timing_pair.
    ap.add_argument("--left", type=Path,
                    help="Path to the LEFT timing-backend JSON report")
    ap.add_argument("--right", type=Path,
                    help="Path to the RIGHT timing-backend JSON report")
    # Deprecated aliases preserved for every invocation that still
    # hardcodes the old cae-vs-gem5 pair (run-all.sh, run-suite.sh,
    # pre-revision callers). They map to --left / --right when the
    # config's timing_pair is the default ("cae", "gem5").
    ap.add_argument("--cae", dest="cae", type=Path,
                    help="(deprecated alias for --left when timing_pair "
                         "is the default (cae, gem5))")
    ap.add_argument("--gem5", dest="gem5", type=Path,
                    help="(deprecated alias for --right when timing_pair "
                         "is the default (cae, gem5))")
    ap.add_argument("--config", required=True,
                    help="Paired YAML name or path (needed for thresholds "
                         "and timing_pair)")
    ap.add_argument("--report-dir", type=Path, required=True)
    args = ap.parse_args()

    cfg = p.load_config(args.config)
    left_label, right_label = cfg.timing_pair

    left_path = args.left
    right_path = args.right
    if left_path is None and args.cae is not None:
        if left_label != "cae":
            ap.error(
                f"--cae alias only valid when timing_pair starts with "
                f"'cae' (config has left_label={left_label!r}); use "
                f"--left instead"
            )
        left_path = args.cae
    if right_path is None and args.gem5 is not None:
        if right_label != "gem5":
            ap.error(
                f"--gem5 alias only valid when timing_pair ends with "
                f"'gem5' (config has right_label={right_label!r}); use "
                f"--right instead"
            )
        right_path = args.gem5
    if left_path is None or right_path is None:
        ap.error("both --left and --right (or their --cae / --gem5 "
                 "aliases) are required")

    left = _load_side(left_path, left_label)
    right = _load_side(right_path, right_label)

    gate = compute_gate(left, right, cfg, left_label, right_label)

    args.report_dir.mkdir(parents=True, exist_ok=True)
    gate_path = args.report_dir / "accuracy-gate.json"
    with gate_path.open("w") as f:
        json.dump(gate, f, indent=2, sort_keys=False)
        f.write("\n")

    md = _render_markdown(gate, left, right)
    md_path = args.report_dir / (
        f"diff-{gate['config']}-{gate['benchmark']}.md"
    )
    md_path.write_text(md)

    print(md)
    return 0 if gate["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
