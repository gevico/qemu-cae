#!/usr/bin/env python3
"""Shared loader + classifier for the paired CAE difftest YAMLs.

Schema (v1):

    common:            # fields mapped to every selected timing backend
      contract:
        schema_name: cae_gem5_difftest
        schema_version: 1
        config_name: <name>
        timing_pair: [<left_backend>, <right_backend>]   # optional;
            # defaults to ["cae", "gem5"] for the in-order track.
            # KMH-V3 configs set e.g. ["cae", "xs-gem5"]. Used by
            # diff.py + ci-gate.py to know which JSON reports to
            # expect and how to label them.
      benchmark: ...
      cpu: {...}
      memory: {...}
      reproducibility: {...}
      measurement:
        metrics: [...]
        thresholds: {...}
        noise_floor: {...}         # per-metric absolute magnitude floor
        suite:                     # optional; ci-gate.py --stage suite
          benchmarks: [...]        #   falls back to AC-4 defaults when
          metrics: [...]           #   omitted
        suite_thresholds:
          max_ipc_pct: ...
          geomean_ipc_pct: ...
          secondary_pct: ...
          secondary_relax: {bench: pct}
    gem5_only:         # fields only meaningful to vanilla gem5
      runtime: ...
      cpu: {...}
      memory: {...}
    xs_gem5_only:      # fields only meaningful to XiangShan GEM5 KMH-V3
      runtime: ...
      cpu: {...}
      memory: {...}
    nemu_only:         # fields only meaningful to the NEMU functional
      runtime: ...     #   reference (trace / checkpoint emission)
    cae_only:          # fields only meaningful to CAE
      cpu: {...}
      cache_config: ... (future)
      bpred_config:  ... (future)
      ooo_config:    ... (future)

Classification rules (consumed by config-equivalence.py and ci-gate.py):
  common.*                   -> must be honored by every launched backend
  gem5_only.*                -> backend_only on CAE / XS-GEM5 / NEMU;
                                mapped on gem5 if consumed
  xs_gem5_only.*             -> backend_only on CAE / gem5 / NEMU; mapped
                                on XS-GEM5 if consumed
  nemu_only.*                -> backend_only on CAE / gem5 / XS-GEM5;
                                mapped on NEMU if consumed
  cae_only.*                 -> backend_only on the reference backends;
                                mapped on CAE if consumed
  gem5_only vs cpu_type      -> variant-restricted knobs are
                                `unsupported` when the active cpu_type
                                cannot honor them
"""
from __future__ import annotations

import hashlib
import os
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

import yaml


REPO_ROOT = Path(__file__).resolve().parents[5]
DIFFTEST_SRC = REPO_ROOT / "tests" / "cae" / "riscv64" / "difftest"
CAE_BUILD_DIR = Path(os.environ.get("CAE_BUILD_DIR", REPO_ROOT / "build"))
BENCH_BUILD_DIR = CAE_BUILD_DIR / "tests" / "cae" / "difftest" / "benchmarks"
BENCH_DIR = DIFFTEST_SRC / "benchmarks"
MANIFEST = BENCH_DIR / "MANIFEST.json"


VALID_ROOTS: tuple[str, ...] = (
    "common",
    "gem5_only",
    "xs_gem5_only",
    "nemu_only",
    "cae_only",
)

DEFAULT_TIMING_PAIR: tuple[str, str] = ("cae", "gem5")


@dataclass
class PairedConfig:
    name: str
    path: Path
    common: dict[str, Any] = field(default_factory=dict)
    gem5_only: dict[str, Any] = field(default_factory=dict)
    xs_gem5_only: dict[str, Any] = field(default_factory=dict)
    nemu_only: dict[str, Any] = field(default_factory=dict)
    cae_only: dict[str, Any] = field(default_factory=dict)

    @property
    def benchmark(self) -> dict[str, Any]:
        return self.common.get("benchmark", {})

    @property
    def cpu(self) -> dict[str, Any]:
        return self.common.get("cpu", {})

    @property
    def memory(self) -> dict[str, Any]:
        return self.common.get("memory", {})

    @property
    def measurement(self) -> dict[str, Any]:
        return self.common.get("measurement", {})

    @property
    def timing_pair(self) -> tuple[str, str]:
        """Two timing-backend labels for this config.

        Read from ``common.contract.timing_pair`` if present, else
        defaults to ``("cae", "gem5")`` for backward compatibility
        with the Phase-2 in-order track. Consumed by diff.py to
        label the two JSON reports being compared and by
        config-equivalence.py to decide which *_only namespace gets
        mapped/backend_only semantics.
        """
        pair = (self.common.get("contract", {}) or {}).get("timing_pair")
        if pair is None:
            return DEFAULT_TIMING_PAIR
        if (not isinstance(pair, list) or len(pair) != 2
                or not all(isinstance(x, str) and x for x in pair)):
            raise ValueError(
                f"{self.path}: common.contract.timing_pair must be a "
                f"[left, right] list of two non-empty backend labels; "
                f"got {pair!r}"
            )
        return (pair[0], pair[1])

    @property
    def gem5_cpu_type(self) -> str:
        return self.gem5_only.get("cpu", {}).get("cpu_type", "AtomicCPU")

    @property
    def gem5_mem_mode(self) -> str:
        return self.gem5_only.get("memory", {}).get("mem_mode", "atomic")

    @property
    def xs_gem5_cpu_type(self) -> str:
        return self.xs_gem5_only.get("cpu", {}).get("cpu_type",
                                                    "DerivO3CPU")


def load_config(path: str | Path) -> PairedConfig:
    p = Path(path)
    if p.is_file():
        pass
    else:
        # Try configs/ directory with and without .yaml suffix,
        # then CWD-relative with .yaml.
        search = [
            DIFFTEST_SRC / "configs" / p,
            DIFFTEST_SRC / "configs" / p.with_suffix(".yaml"),
            p.with_suffix(".yaml"),
        ]
        for cand in search:
            if cand.is_file():
                p = cand
                break
    if not p.is_file():
        raise FileNotFoundError(f"paired YAML not found: {path}")

    with p.open("r") as f:
        raw = yaml.safe_load(f)

    if not isinstance(raw, dict):
        raise ValueError(f"{p}: top-level must be a mapping")

    unknown_roots = set(raw.keys()) - set(VALID_ROOTS)
    if unknown_roots:
        raise ValueError(
            f"{p}: unknown root keys (must be one of "
            f"{list(VALID_ROOTS)}): {sorted(unknown_roots)}"
        )

    common = raw.get("common", {}) or {}
    gem5_only = raw.get("gem5_only", {}) or {}
    xs_gem5_only = raw.get("xs_gem5_only", {}) or {}
    nemu_only = raw.get("nemu_only", {}) or {}
    cae_only = raw.get("cae_only", {}) or {}

    if "contract" not in common:
        raise ValueError(f"{p}: common.contract is required")
    if "benchmark" not in common:
        raise ValueError(f"{p}: common.benchmark is required")

    schema_version = common.get("contract", {}).get("schema_version")
    if schema_version != 1:
        raise ValueError(
            f"{p}: common.contract.schema_version must be 1 (got {schema_version!r})"
        )

    return PairedConfig(
        name=p.stem,
        path=p,
        common=common,
        gem5_only=gem5_only,
        xs_gem5_only=xs_gem5_only,
        nemu_only=nemu_only,
        cae_only=cae_only,
    )


def load_manifest() -> dict[str, Any]:
    if not MANIFEST.is_file():
        raise FileNotFoundError(f"missing {MANIFEST}")
    with MANIFEST.open("r") as f:
        return json.load(f)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def _benchmark_variant(
    bench_name: str, variant: str
) -> tuple[Path, dict[str, Any], dict[str, Any]]:
    """Return (elf_path, variant_entry, top_entry) for `bench_name`."""
    if variant not in ("qemu", "se"):
        raise ValueError(f"unknown benchmark variant '{variant}'")
    entry = load_manifest()["benchmarks"].get(bench_name)
    if entry is None:
        raise ValueError(f"unknown benchmark '{bench_name}' in {MANIFEST}")
    variants = entry.get("variants", {})
    vinfo = variants.get(variant)
    if vinfo is None:
        raise ValueError(
            f"benchmark '{bench_name}' has no '{variant}' variant in "
            f"{MANIFEST}"
        )
    elf = (BENCH_BUILD_DIR / vinfo["binary"]).resolve()
    if not elf.is_file():
        elf = (BENCH_DIR / vinfo["binary"]).resolve()
    return elf, vinfo, entry


def verify_benchmark(cfg: PairedConfig, variant: str = "qemu") -> Path:
    """Confirm the benchmark referenced by `cfg` exists and hashes match.

    `variant` is 'qemu' (bare-metal, for QEMU -bios) or 'se' (user-space,
    for gem5 SE mode). Returns the absolute path to the ELF. Raises on
    drift.

    Cross-check: if `common.benchmark.{qemu,se}_binary` is set in the
    YAML, it must match the MANIFEST entry for the same variant. The
    YAML path is the source of truth for where the binary lives; the
    MANIFEST is the source of truth for what sha256 it should have.
    Disagreement between the two is a plan-drift symptom and fails
    closed.
    """
    bench_name = cfg.benchmark["name"]
    elf, vinfo, _ = _benchmark_variant(bench_name, variant)

    # Cross-check against the YAML-declared binary pin ONLY when the
    # current benchmark name still matches the YAML's original
    # benchmark.name. --benchmark override mutates `cfg.benchmark["name"]`
    # in-place, and the binary pin is only meaningful for the original
    # name; forcing it to match after an override would turn the pin
    # into an obstruction. The MANIFEST is the authoritative path for
    # the active benchmark either way.
    original_name = cfg.benchmark.get("_original_name",
                                      cfg.benchmark["name"])
    if bench_name == original_name:
        yaml_key = f"{variant}_binary"
        yaml_binary = cfg.benchmark.get(yaml_key)
        if yaml_binary is not None:
            yaml_elf = (BENCH_BUILD_DIR / Path(yaml_binary).name).resolve()
            if not yaml_elf.is_file():
                yaml_elf = (DIFFTEST_SRC / yaml_binary).resolve()
            if yaml_elf != elf:
                raise ValueError(
                    f"{cfg.path}: {yaml_key} disagrees with MANIFEST:\n"
                    f"  yaml     : {yaml_elf}\n"
                    f"  manifest : {elf}"
                )

    if not elf.is_file():
        raise FileNotFoundError(
            f"benchmark binary missing: {elf}; run `make -C "
            f"{BENCH_DIR/'src'}`"
        )

    expected_sha = vinfo["sha256"]
    actual_sha = sha256_of(elf)
    if actual_sha != expected_sha:
        raise ValueError(
            f"benchmark binary sha256 mismatch for {bench_name} "
            f"({variant} variant):\n"
            f"  file     : {elf}\n"
            f"  expected : {expected_sha}\n"
            f"  actual   : {actual_sha}\n"
            f"Rebuild the binary or update MANIFEST.json under RFC."
        )

    # Optional per-YAML pin (belt and braces for mixed-variant caller).
    yaml_sha = cfg.benchmark.get("expected_sha256")
    if yaml_sha and yaml_sha not in (expected_sha, actual_sha):
        raise ValueError(
            f"{cfg.path}: benchmark.expected_sha256 disagrees with MANIFEST"
            f"\n  yaml    : {yaml_sha}\n  actual  : {actual_sha}"
        )

    return elf


def manifest_entry(bench_name: str) -> dict[str, Any]:
    """Return the top-level manifest entry for `bench_name`."""
    entry = load_manifest()["benchmarks"].get(bench_name)
    if entry is None:
        raise ValueError(f"unknown benchmark '{bench_name}' in {MANIFEST}")
    return entry


def variant_entry(bench_name: str, variant: str) -> dict[str, Any]:
    """Return the variant sub-entry (binary/sha256/sentinel_addr)."""
    _, vinfo, _ = _benchmark_variant(bench_name, variant)
    return vinfo


# ---------------------------------------------------------------- flatten


def flatten(prefix: str, node: Any) -> Iterable[tuple[str, Any]]:
    """Yield (dotted_path, leaf_value) for every leaf in a nested mapping."""
    if isinstance(node, dict):
        if not node:
            yield prefix, {}
            return
        for k, v in node.items():
            key = f"{prefix}.{k}" if prefix else str(k)
            yield from flatten(key, v)
    elif isinstance(node, list):
        yield prefix, node
    else:
        yield prefix, node


# ---------------------------------------------------------------- main
if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: _pairedyaml.py <config.yaml>", file=sys.stderr)
        sys.exit(2)
    cfg = load_config(sys.argv[1])
    left, right = cfg.timing_pair
    print(f"Loaded {cfg.path} (timing_pair=({left},{right}), "
          f"gem5={cfg.gem5_cpu_type}, bench={cfg.benchmark.get('name')})")
    # Only tier-1 benchmarks have both 'qemu' and 'se' variants; skip
    # the loop for tier-2 entries (they reference host-provided
    # binaries via tier2_binary_relpath and do not live under
    # benchmarks/build/).
    bench_name = cfg.benchmark.get("name")
    if bench_name:
        entry = manifest_entry(bench_name)
        if entry.get("tier", 1) == 1:
            for v in ("qemu", "se"):
                elf = verify_benchmark(cfg, v)
                print(f"Benchmark {v} OK: {elf}")
