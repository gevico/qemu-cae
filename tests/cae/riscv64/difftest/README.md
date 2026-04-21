# CAE difftest pipeline

This directory hosts the cycle-approximate difftest harness. Two
calibration tracks run over the same framework:

- **In-order track** (original Phase-2): CAE `cpu-model=inorder-5stage`
  vs vanilla gem5 `MinorCPU` (`~/gem5`, pinned in
  `tools/cae/gem5-version.txt`). Contract:
  `docs/superpowers/specs/2026-04-17-cae-phase-2-plan.md`.
- **KMH-V3 OoO track** (Phase-2 revision, the primary KPI from this
  revision onward): CAE `cpu-model=ooo-kmhv3` vs XiangShan GEM5 KMH-V3
  (`~/xiangshan-gem5`, pinned in `tools/cae/xiangshan-gem5-version.txt`),
  with NEMU (`~/NEMU`, pinned in `tools/cae/nemu-version.txt`) supplying a
  functional-correctness reference via retired-instruction trace diff
  or periodic architectural-state checkpoint diff. Contract:
  `docs/superpowers/specs/2026-04-18-cae-phase-2-revision-kmhv3.md` +
  `docs/superpowers/plans/2026-04-18-cae-phase-2-revision-kmhv3-plan.md`.

Both tracks share: the paired-YAML schema, `diff.py` (N-side timing
comparison), `ci-gate.py` (stage dispatcher), `config-equivalence.py`
(field classifier), `determinism-check.sh`. Functional comparison for
the KMH-V3 track is **not** inside `diff.py`; it lives in
`nemu-difftest.py` and is a hard prerequisite for any timing-gate
evaluation (AC-K-2).

## Layout

```
tests/cae-difftest/
├── benchmarks/          # RISC-V microbenchmarks (alu, mem-stream, ...)
│   ├── src/             # Source .S / .c files + Makefile
│   ├── build/           # Compiled ELFs (gitignored)
│   └── MANIFEST.json    # Canonical sha256 / argv / measurement window
├── configs/             # Paired YAML (consumed by both runners)
│   ├── atomic-1c.yaml          # CPI=1 self-check (gem5 AtomicCPU)
│   ├── timing-1c.yaml          # CPI=1 self-check (gem5 TimingSimpleCPU)
│   ├── inorder-1c.yaml         # in-order track config (MinorCPU baseline)
│   └── xs-1c-functional.yaml   # KMH-V3 track — M3' functional oracle
│                               #   (xs-1c-realspec.yaml lands with M4'
│                               #   real speculation; xs-1c-kmhv3.yaml
│                               #   with M5' high-fidelity alignment)
├── scripts/             # Python/Bash drivers
│   ├── run-gem5.py          # vanilla gem5 (in-order track)
│   ├── run-cae.py           # CAE driver (both tracks)
│   ├── run-xs-gem5.py       # XiangShan GEM5 (KMH-V3 track)
│   ├── run-nemu-ref.sh      # NEMU functional reference
│   ├── nemu-difftest.py     # functional trace / checkpoint diff
│   ├── diff.py              # N-side timing comparison
│   ├── ci-gate.py           # staged pre-merge checks
│   ├── run-suite.sh         # in-order suite driver
│   └── run-xs-suite.sh      # KMH-V3 suite driver (functional then timing)
└── reports/             # Run output (gitignored; stats JSON + markdown)
```

**Pinned external checkouts:**

| Track            | Source           | Pin file                              | Check script                            |
|------------------|------------------|---------------------------------------|-----------------------------------------|
| in-order (gem5)  | `~/gem5`         | `tools/cae/gem5-version.txt`              | `tools/cae/check-gem5-version.sh`           |
| KMH-V3 (XS-GEM5) | `~/xiangshan-gem5` | `tools/cae/xiangshan-gem5-version.txt`  | `tools/cae/check-xiangshan-gem5-version.sh` |
| KMH-V3 (NEMU)    | `~/NEMU`         | `tools/cae/nemu-version.txt`              | `tools/cae/check-nemu-version.sh`           |

Every driver invokes its check script on startup so any run fails
closed when the reference checkout has drifted. `~/xiangshan-gem5` and
`~/NEMU` are read-only references; NEMU build artifacts go to
`<repo>/build/nemu-ref/` via `scripts/build-nemu-ref.sh` without
mutating `~/NEMU`.

## Tier-1 vs tier-2 benchmarks

Every `MANIFEST.json` entry carries a `tier` field.

- **Tier-1** — committed in-tree microbenchmarks built under
  `benchmarks/build/`. Every variant is sha256-pinned; the full
  byte-identical retired-trace difftest binds these (AC-K-2, tier-1).
  The six Phase-1 micros (`alu`, `mem-stream`, `pointer-chase`,
  `branchy`, `matmul-small`, `coremark-1c`) live here.
- **Tier-2** — host-provided release-gate binaries staged by the
  operator under `$CAE_TIER2_BINARIES_DIR` (default
  `~/cae-ready-to-run/`). Manifest entries hold `tier2_binary_relpath`
  (path relative to that directory), a sha256 (initially
  `PENDING_OPERATOR_STAGING`), and `reset_pc`. This repo does not
  clone the upstream XiangShan `ready-to-run` source; the operator is
  responsible for staging. Until staged, entries are tagged
  `gated: false`. `run-xs-suite.sh --tier 2` is responsible for
  resolving the path + sha256 at run time and for failing closed when
  a tier-2 binary is missing — tier-1 runs remain unaffected.

Per-benchmark metadata beyond sha256:

- `isa_subset` — `rv64gc`, `rv64gcb`, `rv64gc_zifencei`, ... . Gated
  workloads are restricted to RV64GC + B + scalar FP + AMO; RVV is
  out of scope until trace v2 (AC-K-1.1).
- `uses` — which extension families the workload actually exercises
  (`scalar` / `fp` / `amo` / `rvv`). Benchmarks with `rvv` in `uses`
  are tagged `gated: false` until trace v2 lands.
- `frontend_sensitive` — `true` when the benchmark's IPC depends on
  the frontend (BPred / I-cache) enough to make it ineligible for the
  M3' OoO KPI gate; such benchmarks become eligible only at M4' once
  DecoupledBPU + I-cache are in place (AC-K-3.3 / AC-K-4).
- `reset_pc` — architectural reset / entry PC (CAE virt baseline and
  XS baremetal both reset at `0x80000000`).
- `gated` — whether the benchmark participates in `ci-gate.py` /
  `diff.py` aggregation in the current revision.

## Invariants

- Paired YAML in `configs/` is the single source of truth for both sides; a
  CAE-only or gem5-only knob must carry an explicit `cae_only:` / `gem5_only:`
  field prefix so `config-equivalence.py` classifies it correctly.
- `benchmarks/MANIFEST.json` records sha256 over every committed ELF/input
  alongside argv, iteration count, and measurement window; the pipeline
  re-verifies hashes before each run.
- Accuracy results go to `accuracy-gate.json`, throughput results to
  `performance-report.json`. They are intentionally separate files with
  independent gate semantics (accuracy is always binding; throughput is only
  binding at the final bound-weave milestone).

## M1 framework self-check — what the two CPI=1 configs prove

`configs/atomic-1c.yaml` and `configs/timing-1c.yaml` are the
framework-plumbing self-check configs. Their purpose is to demonstrate
that `run-gem5.py`, `run-cae.py`, `diff.py`, `ci-gate.py`, and the
stats field mapping are wired correctly and deterministic; they are
**not** intended to close AC-3 on accuracy.

The CAE side of these configs selects the Phase-1 default
`cpu-model=cpi1` charge path. That hot path retires one engine cycle
per retired instruction, so CAE IPC is pinned at 1.0 regardless of
workload. gem5's `AtomicCPU` and `TimingSimpleCPU` each apply a
non-trivial per-instruction latency table (and, for
`TimingSimpleCPU`, memory access latency on top), so their reported
IPC is typically well below 1.0 (≈ 0.83 on `alu`, ≈ 0.64 on
`mem-stream`, lower on `pointer-chase`). The resulting IPC delta is
inherently large and the self-check gate's boolean verdict will read
`pass: false` on the CPI=1 side.

This is the **expected structural gap** that motivates the M2
`configs/inorder-1c.yaml` config: once `cpu-model=inorder-5stage` and
`bpred-model=2bit-local` replace the CPI=1 charge path with a real
per-uop latency model + mispredict penalties, the IPC gap should
close to within the plan's AC-4 tolerance (max ≤ 10 %, geomean ≤ 5 %
vs `MinorCPU`). The CPI=1 configs remain as framework-plumbing
regression probes — if `run-all.sh` or `config-equivalence.py` breaks,
their `accuracy-gate.json` and `diff-*.md` artifacts still need to be
shape-correct regardless of IPC numbers.

Treat a `pass: false` on `atomic-1c` / `timing-1c` as "expected while
CAE is on the CPI=1 charge path". Treat a missing or shape-wrong
`accuracy-gate.json` as a framework regression. AC-3 proper is closed
by the `inorder-1c` 6-benchmark run under M2.

## AC-4 entry point

For the six-benchmark AC-4 sweep under `inorder-1c`, use the suite
driver as the single authoritative entry point:

```
bash tests/cae-difftest/scripts/run-suite.sh inorder-1c
```

`run-suite.sh` clears stale report directories, runs config-
equivalence once, then for each of the six benchmarks (`alu`,
`mem-stream`, `pointer-chase`, `branchy`, `matmul-small`,
`coremark-1c`) sequentially invokes `run-gem5.py`, `run-cae.py`,
`diff.py`, and `determinism-check.sh --mode serial`. Any per-stage
failure marks the benchmark as failed and the suite stage
(`ci-gate.py --stage suite`) runs only against fresh artifacts — a
stale `accuracy-gate.json` cannot satisfy a new run. The driver
returns 0 only when every benchmark passes diff + determinism and
the suite gate's max/geomean thresholds hold.

`run-all.sh <config> <benchmark>` remains the one-benchmark
entry-point for targeted diagnosis; it does not gate the whole suite.

## KMH-V3 track suite entry point

For the KMH-V3 OoO calibration track, the suite driver is
`run-xs-suite.sh`, which orchestrates functional-then-timing
comparisons end-to-end. Per-benchmark sequence:

```
NEMU functional run   (run-nemu-ref.sh)
  └─ nemu-difftest.py (binding; skips timing if FAIL)
       └─ XS-GEM5 timing run (run-xs-gem5.py, --disable-difftest)
            └─ CAE timing run (run-cae.py)
                 └─ diff.py (timing_pair=[cae, xs-gem5])
                      └─ ci-gate.py --stage suite
```

Entry points:

```
bash tests/cae-difftest/scripts/build-nemu-ref.sh
bash tests/cae-difftest/scripts/run-xs-suite.sh xs-1c-functional \
    [--tier 1|2] [--benchmark <name>] [--full-trace]
```

`build-nemu-ref.sh` stages `~/NEMU` into `<repo>/build/nemu-ref/` and
applies `tools/cae/nemu-itrace.patch` idempotently — `~/NEMU` itself is
never mutated (AC-K-2.5). The resulting
`build/nemu-ref/build/riscv64-nemu-interpreter` is what
`run-nemu-ref.sh` invokes.

`run-xs-suite.sh` refuses to invoke the timing gate (`diff.py` +
`ci-gate.py`) for any benchmark whose functional diff returned
non-zero. Tier-2 benchmarks default to checkpoint-mode trace
(1M-retire interval) unless `--full-trace` is passed (AC-K-2).
`--tier 1` runs the in-tree micros regardless of whether
`$CAE_TIER2_BINARIES_DIR` exists; `--tier 2` requires the operator
to have staged the binaries under that directory.

### KMH-V3 track known-missing pieces

The following files are on the plan's KMH-V3 path but have NOT yet
landed; `run-xs-suite.sh` surfaces each as a structural skip:

- `hw/cae/cpu_ooo.c` + `hw/cae/ooo/*` + `hw/cae/cache_mshr.c` +
  `hw/cae/bpred/tournament.c` — until these land,
  `cpu-model=ooo-kmhv3` cannot be selected and `run-cae.py` under
  `xs-1c-functional.yaml` will refuse to launch.
- Retire-side trace emitter in `target/riscv/cae/cae-cpu.c` — until
  this lands, CAE does not produce `cae-itrace.bin` and
  `run-xs-suite.sh` falls through to a structural skip before the
  timing gate.
- `xs-1c-realspec.yaml` (M4' real-speculation gate) and
  `xs-1c-kmhv3.yaml` (M5' high-fidelity gate) — land with their
  corresponding CAE implementations.
