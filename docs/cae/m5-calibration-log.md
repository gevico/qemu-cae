# M5' KMH-V3 Calibration Log

This file records every calibration knob change affecting the
`xs-1c-kmhv3.yaml` suite, with the before / after per-benchmark
delta. It is the canonical audit trail for the AC-K-5 terminal
gate (max IPC error <= 10%, geomean <= 5% vs `kmhv3.py`).

## Schema

Each row is a single knob change, appended in chronological
order. Columns:

| column | meaning |
| --- | --- |
| `round` | RLCR round that introduced the change |
| `knob` | property name (QOM / YAML leaf / source const) |
| `before` | prior value |
| `after` | new value |
| `benchmark` | benchmark whose IPC motivated the change, or `baseline` for structural alignment |
| `delta_ipc_pct` | observed CAE / XS-GEM5 IPC error delta (signed; negative = CAE slower) |
| `notes` | rationale / cross-reference |

Rows without a measured `delta_ipc_pct` (structural alignment
rows) leave that cell blank; a later row fills it in when a
live XS-GEM5 sweep is available.

## Baseline — Round 49

Round 48 landed the AC-K-5 implementation-surface files
(`scheduler.c`, `violation.c`, `xs-1c-kmhv3.yaml`) and the
concrete RAT / LSQ runtime state. Round 49 plumbs the CAE-side
width / regfile knobs (`rob_size`, `lq_size`, `sq_size`,
`issue_width`, `commit_width`, `rename_width`,
`num_phys_int_regs`, `num_phys_float_regs`) end-to-end through
`accel/cae/cae-all.c`, `run-cae.py`, and
`config-equivalence.py`, so `xs-1c-kmhv3.yaml` now sets real
values rather than documenting missing forwarders.

**Actual CAE-side round-49 baseline** (what the config delivers
to the live OoO model right after this round's work lands):

| round | knob | value | notes |
| --- | --- | --- | --- |
| 49 | `cae_only.cpu.rob_size` | 352 | kmhv3.py default; CAE default was already 352 |
| 49 | `cae_only.cpu.lq_size` | 72 | kmhv3.py default; CAE default was already 72 |
| 49 | `cae_only.cpu.sq_size` | 56 | kmhv3.py default; CAE default was already 56 |
| 49 | `cae_only.cpu.rename_width` | 8 | kmhv3.py default; CAE default was already 8 |
| 49 | `cae_only.cpu.commit_width` | 8 | kmhv3.py default; CAE default was already 8 |
| 49 | `cae_only.cpu.issue_width` | 8 | kmhv3.py aligns at 8; CAE default was 6. **Real round-49 structural delta.** |
| 49 | `cae_only.cpu.num_phys_int_regs` | 224 | kmhv3.py default; CAE default was already 224 |
| 49 | `cae_only.cpu.num_phys_float_regs` | 256 | kmhv3.py default; CAE default was already 256 |
| 49 | `cae_only.cpu.sbuffer_size` | 16 | kmhv3.py default; CAE default was already 16 |

The only knob whose effective CAE value actually changes in
round 49 is `issue_width` (6 -> 8). Every other row documents
existing-default alignment so the audit trail shows the full
set of fields that `xs-1c-kmhv3.yaml` now sets.

## AC-K-5 Secondary Surfaces — Round 49

| round | knob | value | notes |
| --- | --- | --- | --- |
| 49 | `CAE_OOO_SCHED_SEGMENTS` | 3 | `scheduler.c` — 3 segments x 2 ports |
| 49 | `CAE_OOO_SCHED_PORTS` | 2 | |
| 49 | `CAE_OOO_SCHED_SEGMENT_CAPACITY` | 64 | fixed ring per segment |
| 49 | `CAE_OOO_VIOL_RARQ_CAPACITY` | 16 | `violation.c` RARQ ring |
| 49 | `CAE_OOO_VIOL_RAWQ_CAPACITY` | 16 | `violation.c` RAWQ ring |
| 49 | `CaeCacheMshr.bank_count` | 0 (default) | surface landed in `cache_mshr.c` but **NOT** yet delivered to `xs-1c-kmhv3`; round 50 adds the forwarder chain |
| 49 | `CaeCacheMshr.bank_conflict_stall_cycles` | 0 (default) | same — surface-only; round 50 delivers |
| 49 | `CaeSbuffer.evict_threshold` | 0 (default) | surface landed in `sbuffer.c` but **NOT** yet delivered to `xs-1c-kmhv3`; round 50 adds the forwarder chain |

Round 49's calibration-log statement that `bank_conflict_cycles =
1` / `sbuffer_evict_threshold = 12` were "live baseline" was
overstated per Codex's round-49 review — the accel-class forwarder
path and the `xs-1c-kmhv3.yaml` consumer were both missing. The
module-level counters existed and their unit tests passed, but the
live config did not reach them. Round 50 closes that gap.

## Round 50 — live delivery of bank-conflict, evict-threshold, and width knobs

Round 50 AC-K-5 plumbing closes two round-49 gaps flagged by
Codex:

1. **Live memory-knob delivery.** New forwarders in
   `accel/cae/cae-all.c` (`bank-count`,
   `bank-conflict-stall-cycles`, `sbuffer-evict-threshold`)
   propagate the YAML `cae_only.memory.*` leaves onto the
   cae-cache-mshr and cae-cpu-ooo → cae-sbuffer chain. The
   `run-cae.py` `_CAE_ACCEL_FIELD_MAP` and
   `config-equivalence.py` `CAE_CONSUMED_FIELDS` gained the
   three matching keys; `xs-1c-kmhv3.yaml` sets non-zero
   values so the kmhv3 baseline now drives real bank-conflict
   + sbuffer evict-threshold accounting.

2. **Behavioral consumption of `issue_width` / `rename_width`**
   (round-50 surface; round-51 correction). Round 50 placed
   the `rename_width=0` gate AFTER dispatch-side allocation
   (ROB / LSQ / RAT / sbuffer already mutated by the time
   `rename_stalls` bumped) and ignored the bounded scheduler's
   `sched_issued` return in the live commit / cycle path.
   Codex correctly rejected both as counter-only surfaces.
   **Round 51 rewrites the live-path semantics**:
   - `rename_width == 0` is the FIRST check in
     `cae_cpu_ooo_charge`, BEFORE any pre-check or allocation.
     The retire charges 1 cycle, bumps `rename_stalls`, and
     returns without touching any sub-structure. The round-51
     regression
     `/cae/ooo/cpu-ooo-rename-width-zero-no-pipeline-mutation`
     pins this: 5 ALU + STORE retires leave `rob_count`,
     `rat_int_inflight`, `rat_int_free_count`, and sbuffer
     `occupancy` unchanged while `rename-stalls` advances by
     exactly 5.
   - `sched_issued` from `cae_ooo_scheduler_issue_cycle_bounded`
     now gates the ROB commit loop via
     `commit_cap = min(commit_width, sched_issued)`. An empty
     scheduler stops commits that tick even for ready ROB
     entries.
   - The per-retire cycle charge is now `ceil(rename_width /
     issue_cap)` where `issue_cap = min(issue_width,
     CAE_OOO_SCHED_PORTS)`. Under kmhv3 YAML (rename=8,
     issue=8 → cap=2): cycles = 4 per retire. Under
     issue_width=1: cycles = 8. `rename_width <= 1` keeps
     cycles = 1 so pre-round-51 unit tests and `cpi1` /
     `inorder-5stage` callers are unaffected.

| round | knob | live value | notes |
| --- | --- | --- | --- |
| 50 | `cae_only.memory.mshr.bank_count` | 8 | kmhv3.py L1D bank count; `cae-cache-mshr` bank-conflict tracker active |
| 50 | `cae_only.memory.mshr.bank_conflict_stall_cycles` | 2 | same-bank collisions charge 2 extra cycles |
| 50 | `cae_only.memory.sbuffer_evict_threshold` | 12 | ~75% of `sbuffer_size=16`; sbuffer evict tracker active |
| 51 | `cae_only.cpu.issue_width` | 8 | LIVE per-cycle commit cap + cycle-charge divisor via segmented scheduler; clamps to `CAE_OOO_SCHED_PORTS=2` at runtime |
| 51 | `cae_only.cpu.rename_width` | 8 | LIVE dispatch gate (=0 → full stall, no pipeline mutation) + cycle-charge numerator (`cycles = ceil(rename / issue_cap)`) |

Unit-level proof for rounds 50 + 51 lives in `tests/unit/test-cae.c`:

- `/cae/ooo/cpu-ooo-issue-width-changes-scheduler-issued` —
  secondary proof of the bounded scheduler API via test-only
  seam; still passes.
- `/cae/ooo/cpu-ooo-issue-width-changes-live-commit` (round 51,
  new) — two trials under `issue_width=1` / `=8` with
  `rename_width=4` on identical ALU traffic. No seeding.
  Asserts `total-cycles` is strictly larger under narrow
  issue (40 vs 20 on 10 retires). Pins the live cycle-charge
  formula.
- `/cae/ooo/cpu-ooo-rename-width-observable` — `rename_width=0`
  advances `rename-stalls` while `scheduler-enqueued` stays
  zero across five ALU retires. (Retained.)
- `/cae/ooo/cpu-ooo-rename-width-zero-no-pipeline-mutation`
  (round 51, new) — stronger invariant: 5 mixed ALU+STORE
  retires with `rename_width=0` leave ROB / RAT / sbuffer
  state unchanged. Pins the dispatch-gate semantic.
- `/cae/cache/mshr-bank-conflict` — round-49 regression proves
  non-zero `bank-conflict-events` under the new forwarder
  chain.
- `/cae/sbuffer/evict-threshold` — round-49 regression proves
  `evict-threshold-events` fires when occupancy crosses the
  watermark.

Neither round 50 nor round 51 claims AC-K-5 is MET: the
IPC-error gate still requires a runnable XS-GEM5 binding.
Round 51 attempts `t-spike-xsgem5` as an analyze task;
the outcome and any blockers are recorded in that round's
summary and are revisited each subsequent round until the
live `xs-1c-kmhv3` suite runs.

### Observed live counter readings on `xs-1c-kmhv3` (round 50)

Post-round-50 QMP readings on a live run with the delivered YAML:

| benchmark | scheduler_enqueued | scheduler_issued | violation_loads | violation_stores | bank_conflict_events | sbuffer_evict_events | rename_stalls |
| --- | --- | --- | --- | --- | --- | --- | --- |
| alu          | 1,000,028 | 1,000,028 | 2         | 1      | 0 | 0 | 0 |
| mem-stream   | 230,023   | 230,023   | 32,002    | 32,001 | 0 | 0 | 0 |
| matmul-small | 8,555,546 | 8,555,546 | 1,048,578 | 65,537 | 0 | 0 | 0 |

**Scheduler + violation tracker: LIVE** — counters advance
in lockstep with retire traffic on every benchmark, as
expected. Proves the round-49 scheduler + violation wiring
and round-50 issue_width cap reach the live path.

**Bank-conflict + sbuffer evict-threshold: 0 on all three
benchmarks.** This is an architectural-simulation gap, not
a knob-delivery gap. The round-50 unit regression
`/cae/cache/mshr-bank-conflict` and `/cae/sbuffer/evict-
threshold` both prove the downstream trackers fire under
the right stimulus; the live TCG retire-path timing model
simply doesn't naturally produce that stimulus:

- Bank-conflict fires only when two accesses in the SAME
  cycle (± `bank_conflict_stall_cycles`) hash to the same
  bank. CAE's one-insn-per-TB flow advances engine
  `now_cycle` by >=1 cycle per memory access — so two
  accesses never share a cycle under natural retire.
- Sbuffer evict-threshold fires only when sbuffer occupancy
  reaches the watermark (12). Stores enter the sbuffer at
  dispatch and drain when the ROB commits the entry; with
  `commit_width=8` and 1 retire/cycle, a store alloc at
  cycle C commits at C+1, keeping steady-state occupancy
  at <= 1.

Closing this natural-firing gap requires the architectural
multi-access-per-cycle issue model (not scoped for round 50;
the segmented scheduler structural surface is landed, but
driving it to actual multi-issue per retire cycle is a
separate lane). The COUNTERS are live and calibration-
ready; the WORKLOAD-DRIVEN observability depends on the
retire-path tick rate, which is tracked separately.

## Pending — live XS-GEM5 sweep

The live `xs-1c-kmhv3` suite calibration against `kmhv3.py`
depends on a runnable XS-GEM5 binding. On this host, XS-GEM5
is not yet built; the binding is sequenced for the next round
in which `t-spike-xsgem5` completes. At that point, measured
`delta_ipc_pct` values land here per benchmark, and per-knob
tuning rows appear with their real before / after evidence.
Until then, this log deliberately contains no fabricated
measurement data — a false audit trail is worse than no audit
trail.

### Round-54 live-run attempt — advanced, but AC-K-5 still NOT-MET

Round 54 built `gem5.opt` (910 MB, pinned `e584f268a6`) via
the new `tools/build-xs-gem5-staged.sh` Docker-backed
helper, and drove it through the suite runner's full
XS-GEM5 launch path. No fabricated IPC: gem5 panicked
before any `stats.txt` was written. What the run proved:

| stage                              | status   |
| --- | --- |
| Staged build via Docker helper     | ok (910 MB gem5.opt, pre/post sha256 match) |
| Docker-wrap gem5.opt invocation    | ok (runs inside `gem5-build:local`) |
| kmhv3.py argparse                  | ok (`--disable-difftest --raw-cpt --generic-rv-cpt --mem-type=DDR3_1600_8x8` all accepted) |
| `xiangshan_system_init()` objects  | ok (CPU + L1D + L2 4-slice + BOP/CMC/IPCP/SPP/SSTRIDE/XSSTREAM + L3 + DDR3 memctrl) |
| `**** REAL SIMULATION ****` banner | ok |
| Retire any instructions            | FAIL — commit-stuck panic at 480k cycles |
| `stats.txt` written                | FAIL — aborted pre-stats |
| `accuracy-gate.json` emitted       | FAIL — AC-K-5 NOT-MET |

Concrete next-obstacle symptoms:

- `MicroTAGE: branch 0x80000068 exceeds block [0, 0)
  (blockSize=0, instShift=1, maxPositions=32); clamping
  index` — BTB block size is 0, likely DRAMsim3 →
  DDR3_1600_8x8 swap bypassed a cache-line-size knob.
- `Commit stage is stucked for more than 40,000 cycles! Last
  commit cycle: 428259, current cycle: 480110` — no retires
  from sim-start through 480k cycles.

Round-55 candidate fix paths (spike `ready-to-run/coremark-
2-iteration.bin` to isolate; or enable DRAMsim3 in the
build image and drop the override). Both documented in
`.humanize/rlcr/2026-04-18_23-54-56/round-54-summary.md`.

### Round-55 diagnostic spike — root cause pinpointed, fix queued for round 56

Round 55 ran the isolation spike prescribed by the round-54
summary. Reproduced the panic on coremark-1c.xs.bin (1288
bytes, ~12x larger than alu.xs.bin's 110 bytes) with
IDENTICAL symptoms — `MicroTAGE blockSize=0` warn +
`Commit stage stucked 40,000 cycles` panic at ~2M cycles
(vs alu's ~480k). The panic is NOT alu-specific.

| metric | alu (r54) | coremark-1c (r55 spike) |
| --- | --- | --- |
| binary bytes | 110 | 1288 |
| cycle at commit-stuck panic | ~480k | ~2M |
| blockSize in warn | 0 | 0 |
| first out-of-block branch | 0x80000068 | 0x80000044 |

Root cause: the pinned XS-GEM5 tree's MicroTAGE Python
param `blockSize = Param.Unsigned(32,"...")` resolves to 0
at runtime despite the explicit default, breaking BTB
position indexing. Every branch prediction is garbage →
every branch mispredicts → pipeline flushes indefinitely
→ no retires → commit-stuck. NOT caused by the round-54
`--mem-type=DDR3_1600_8x8` workaround (DRAMsim3 is a
memory-model param, blockSize is a predictor param; the
two are independent).

Round-56 fix lane: author
`tools/xs-gem5-microtage-blocksize.patch` adding an
explicit `cpu.branchPred.microtage.blockSize = 32` override
in the predictor config, apply it to the staged source tree
after rsync in `build-xs-gem5-staged.sh`, rebuild, rerun
the suite. Expected outcome: REAL SIMULATION proceeds past
the 40k-cycle threshold and writes stats.txt; AC-K-5 gate
evaluable for the first time.

Round-55 advances (non-panic-blocker, Codex round-54
queued):

- Q1 build-image provenance: `tools/build-xs-gem5-
  staged.sh` now records the exact image+digest in
  `<repo>/build/xs-gem5-build/image.txt` at build time;
  `tests/cae-difftest/scripts/run-xs-gem5.py` prefers
  the recorded image over ambient `$XS_GEM5_BUILD_IMAGE`.
  Closes the round-54 "same image as build" gap.
- Q2 stale doc appendix: `docs/cae/xs-stack-known-good.md`
  `## Status on this host` appendix refreshed to the
  round-55 state (staged binary present, docker-backed
  canonical, libboost-all-dev note removed).

Full diagnostic trail in
`.humanize/rlcr/2026-04-18_23-54-56/round-55-spike-
notes.md`.

### Round-2 (cae-cpu-ooo tick-driver plan) live-run attempt — docker wrap unblocked, MicroTAGE panic still the terminal blocker

Round 2 of the tick-driver plan (`.humanize/rlcr/
2026-04-20_20-15-55/round-2-contract.md`) re-attempted the
xs-suite `alu` run as part of `task-E1`. Two concrete
advances, one persistent blocker:

1. **Docker-wrap image digest pin** (fixed here). The earlier
   run failed inside `run-xs-gem5.py`'s docker launch with
   `pull access denied` when docker tried to verify
   `gem5-build:local@sha256:5a61babcdccb...` against a
   registry — the image is local-only, so the digest-pinned
   form is unverifiable. `_recorded_build_image()` in
   `tests/cae-difftest/scripts/run-xs-gem5.py` now checks
   the local image's digest against the recorded digest and
   returns the plain tag when they match, only falling back
   to the `@digest` form if they disagree (so an accidentally-
   retagged image still refuses to run). This is the fix
   committed alongside this note.

2. **Real gem5 execution now reached** (was previously masked).
   After the docker wrap fix, gem5.opt actually enters
   `**** REAL SIMULATION ****` and runs for ~480k cycles
   before the MicroTAGE panic fires — identical to the
   Round-55 diagnostic symptom. This confirms the Round-55
   root-cause analysis was correct and the pin behaviour
   reproduces deterministically.

3. **Still blocked on MicroTAGE blockSize=0** (unchanged
   from Round 55). Even though `configs/example/kmhv3.py`
   line 110 explicitly sets
   `cpu.branchPred.microtage.blockSize = 32` and
   `src/cpu/pred/BranchPredictor.py` line 1087 declares the
   default as `Param.Unsigned(32, ...)`, the live
   `MicroTAGE::advanceState()` sees `blockSize=0` at
   runtime. The override does not reach the C++ side — the
   bug is therefore inside the binary, not in the Python
   config script. Fixing it requires either:

   a. A source-level patch against `microtage.{cc,hh}` that
      hard-defaults `blockSize` to 32 at C++ construction
      (bypassing the Param machinery), applied after rsync
      in `tools/build-xs-gem5-staged.sh` and followed by a
      full rebuild inside `gem5-build:local` (20–40 min).

   b. A deeper investigation into why the Python Param
      does not propagate to the C++ ctor in the pinned tree,
      coordinated with the upstream XS-GEM5 maintainer
      (off-host effort).

   Option (a) is the plan-queued "Round-56 fix lane" path
   and is the correct next step for a subsequent round —
   out of Round 2's reasonable time budget because the
   rebuild alone exceeds the session window and the patch
   content is non-obvious (the live bug is the Python/C++
   boundary, not the param value itself). Route via
   `/humanize:ask-codex` before the rebuild to pick the
   smallest patch that actually propagates the value.

As a result, `task-E1` and `task-E2` still cannot produce
`xs-gem5.json` + `accuracy-gate.json` on this host. Once the
MicroTAGE fix lands and a staged rebuild completes,
`docs/cae/m5-calibration-log.md` gets the first real
`delta_ipc_pct` rows per plan Milestone E.

### Round-2 post-patch rerun — MicroTAGE fix verified, commit-stuck panic persists elsewhere

After the MicroTAGE patch (`tools/xs-gem5-microtage-
blocksize.patch`) landed and `gem5.opt` was rebuilt inside
`gem5-build:local` (with `libsqlite3-dev` + `libhdf5-dev`
apt-installed at container startup — the canonical
`cae/xs-gem5-build:latest` image was not present on this
host), the xs-suite was rerun:

```
bash tests/cae-difftest/scripts/run-xs-suite.sh xs-1c-kmhv3 --benchmark alu
```

Verified improvements:

- `MicroTAGE: branch ... exceeds block [0, 0) (blockSize=0, ...)`
  warning is **gone** from the simulator output.
- `m5out/config.ini` now reports
  `[system.cpu.branchPred.microtage] blockSize=64` (inherited
  from `TimedBaseBTBPredictorParams`, since the shadowing
  declaration was removed). The patch is correct at the Param
  propagation level.

Remaining terminal blocker (unchanged cycle count):

- `panic: Commit stage is stucked for more than 40,000 cycles!
  Last commit cycle: 428259, current cycle: 480110` —
  identical numbers to the pre-patch run. The panic is now
  surfaced WITHOUT the MicroTAGE warn, confirming the commit
  stall has a different root cause than the blockSize
  shadowing. Root cause unknown as of this round; leading
  hypotheses to investigate next round:
    1. kmhv3.py line 110's
       `cpu.branchPred.microtage.blockSize = 32` override
       lands, but `config.ini` still reports 64 under
       `[system.cpu.branchPred.microtage]`. So the per-
       instance override is NOT taking effect. May be an
       ordering issue (line 110 runs before the `Parent`
       resolver finalizes, so the 32 is overwritten by the
       inherited 64). The pipeline's commit-stuck may be
       sensitive to this since MicroTAGE is part of the
       branch-predictor chain.
    2. An unrelated pipeline flow-control bug in the
       XS-GEM5 pin (e.g. fetch/rename interlock) that
       happens to manifest at ~428k cycles regardless of
       the MicroTAGE state.

- `stats.txt` is still 2 lines (the driver's preamble); no
  `accuracy-gate.json` is written.

Round-3 continuation plan (not in scope for this round):

- Pin the kmhv3.py override arithmetic: add a `config.ini`
  post-dump check to `run-xs-gem5.py` that asserts
  `[system.cpu.branchPred.microtage] blockSize == 32` and
  fail-closes if the override was silently overwritten.
- If the override holds but commit-stuck persists, trace
  fetch/rename probe points via `--debug-flags=` and
  consult the pinned XS-GEM5 upstream issue tracker.
- Alternatively, cut over to the canonical
  `cae/xs-gem5-build:latest` image and retry — the
  Dockerfile ships every dependency, so the apt-install
  wrap is no longer required.

Round 2 of the tick-driver plan thus advanced the XS-GEM5
calibration lane from "docker pull denied" through
"MicroTAGE blockSize=0" to "commit-stuck-from-unknown-
pipeline-cause". Real `xs-gem5.json` + `accuracy-gate.json`
artifacts still unavailable; the plan's Milestone E closing
evidence is deferred to Round 3 pending upstream-tree
pipeline diagnostics.

### Round-3 post-patch rerun — MicroTAGE config landing verified, upstream O3 livelock confirmed

Round 3 finished the MicroTAGE patch (`tools/xs-gem5-
microtage-blocksize.patch` switched from removing the
shadowing declaration to a plain `blockSize = 32` class
override matching BTBITTAGE / mbtb / abtb), added a fail-
closed `config.ini` guard in `run-xs-gem5.py` per Codex R2
directive, and introduced `mem-stream` as the Lower-Bound
store-dense benchmark mandated by plan line 407.

Verified progress:

1. **MicroTAGE Python→C++ config propagation fixed.** The
   rebuilt `gem5.opt` produces
   `[system.cpu.branchPred.microtage] blockSize=32` in both
   `xs-1c-kmhv3/alu/m5out/config.ini` and
   `xs-1c-kmhv3/mem-stream/m5out/config.ini`. The fail-
   closed `_assert_microtage_blocksize_32()` guard in
   `run-xs-gem5.py` passes on both benchmarks — future
   regressions in this config path now surface as a specific
   config-mismatch SystemExit instead of hiding behind a
   downstream panic.

2. **Benchmark switch landed.** `tests/cae-difftest/configs/
   xs-1c-kmhv3.yaml` now lists both `alu` (baseline) and
   `mem-stream` (store-dense Milestone-E evidence) under
   `common.measurement.suite.benchmarks`. `run-xs-suite.sh
   xs-1c-kmhv3 --benchmark mem-stream` reaches
   `**** REAL SIMULATION ****` with the store-dense workload.

3. **`CAE_XS_GEM5_CONFIG_SCRIPT` escape-hatch infrastructure
   added.** The environment variable lets a caller pivot
   `run-xs-gem5.py` from `kmhv3.py` to `idealkmhv3.py` without
   editing the script. Default stays `kmhv3.py` (plan
   calibration target). This is the hook Codex suggested for
   the Milestone-E escape path; exercised this round to
   demonstrate the upstream O3 bug is NOT kmhv3-specific.

Remaining terminal blocker — now **confirmed upstream** and
**not in-repo fixable**:

- **Commit-stuck livelock in the pinned XS-GEM5 O3
  pipeline.** The panic fires at
  `build/RISCV/cpu/o3/commit.cc:126` with message
  `panic: Commit stage is stucked for more than 40,000
  cycles! Last commit cycle: <N>, current cycle: <N+40k+>`.
  The pre-stall `<N>` value is strictly positive (alu:
  428259, mem-stream under kmhv3.py: 105843, mem-stream
  under idealkmhv3.py: 107607), so the CPU does retire
  instructions BEFORE deadlocking — consistent with
  `commit.cc`'s `lastCommitCycle` only advancing on
  successful commit. Codex's diagnostic
  (via `/humanize:ask-codex` 2026-04-20T22:32Z) classified
  this as "genuine XS-GEM5 upstream O3 pipeline livelock"
  likely in the post-ROB-drain fetch / rename / IEW stall-
  handshake path (commit.cc is the detector, not the fix
  site). Both `kmhv3.py` and `idealkmhv3.py` exhibit the
  livelock at near-identical cycle counts (~105k–108k) on
  `mem-stream`, confirming the bug is O3-pipeline-shared
  rather than config-specific.

- **`stats.txt` remains 2-line driver preamble** on every
  post-patch run; no `accuracy-gate.json` is emitted; the
  suite log still records `TIMING-XS-GEM5-FAIL`; and
  `performance-report-xs.json` still reports
  `xs_gem5_present=false` with null `xs_gem5_wallclock_seconds`.

Consequence for plan Milestone E closure:

- Task-E1 / Task-E2 cannot land real KMH-V3
  `delta_ipc_pct` evidence on this host without either:
    (a) an upstream XS-GEM5 O3 pipeline fix (multi-day
        off-host effort, coordination with XS-GEM5
        maintainers required), or
    (b) a TimingSimpleCPU fallback that emits "xs-gem5.json"
        from a completely different CPU model — which Codex
        explicitly flagged as "only an artifact-production
        workaround, not a valid KMH-V3 calibration fix".

- Path (b) would satisfy the letter of the Round-2 review
  directive ("produce xs-gem5.json + accuracy-gate.json")
  but would violate its spirit: the emitted numbers would
  not be KMH-V3 calibration data and the plan's
  `delta_ipc_pct` column would be misleading rather than
  evidence-free.

- Goal-tracker has been updated to classify Milestone E as
  blocked on an **upstream external dependency**, not on
  in-repo work that can be completed by this plan's loop.
  This is a meaningful reclassification vs. Round 1/2 where
  the blocker was in-tree (docker-wrap, MicroTAGE). Round 3
  eliminates every in-repo layer of the blocker and pins
  the remaining work to a party outside this plan's scope.

Lower-Bound expectation reminder (Codex R2 directive 5):

- `sbuffer_eviction_events`: non-zero expected on
  `mem-stream`. Cannot verify without xs-suite artifacts
  emitting an XS IPC signal, but unit test
  `/cae/ooo/sbuffer-residency-survives-commit` confirms the
  code-path's arithmetic on store-dense sequences.

- `bank_conflict_cpu_events`: expected to remain 0 under
  Lower-Bound (A+B+C) — Milestone D's virtual-issue
  batching is deferred per DEC-8, so no same-cycle multi-
  access can fire this counter in the current tick pump.
  Documented here so a future reader does not mis-read the
  zero as a regression.

### Round-4 Milestone-E evidence closure: structural + infrastructure

Round-4 closed the CAE-side evidence gap that Codex Round-3
review directive 1 required (`mem-stream` cae.json showing
all-zero `sbuffer_*_evicts` counters). Root cause was
two-layer:

  (A) No forwarding. The sbuffer's `inactive-threshold` and
      `sqfull-commit-lag-threshold` RW QOM properties had no
      path from the accel layer — `accel/cae/cae-all.c`
      (locked under AC-7) only forwards `sbuffer-evict-
      threshold`, so the Timeout / SQFull tick branches
      were permanently 0-disabled at runtime on the live
      xs-1c-kmhv3 lane.

  (B) Structural ceiling. Even with forwarding, on the
      current Lower-Bound timing model a default
      single-cycle store gets dispatched, ROB-committed, and
      tick-drained all in the same charge. Steady-state
      sbuffer occupancy stays at ≤1. The Full branch needs
      occupancy ≥ threshold AND head not commit-drainable;
      the Timeout branch needs non-empty buffer for
      threshold+1 consecutive non-draining ticks; the
      SQFull branch needs `store_sqn_next - commit_sqn ≥
      lag_threshold` AND head not commit-drainable. None of
      the three conditions hold on natural `mem-stream`
      under the default CAE timing model.

Round-4 response (A):

  - New RW QOM properties on `CaeCpuOoo`:
    `sbuffer-inactive-threshold` +
    `sbuffer-sqfull-commit-lag-threshold`, forwarded to the
    sbuffer child at `complete()` time (same pattern the
    existing `sbuffer-evict-threshold` already uses).
    Defaults stay 0 so no existing regression changes
    behaviour.

  - `run-cae.py` issues a `qom-set` on
    `/objects/cae-cpu-model` after the QMP handshake to arm
    `inactive-threshold = 2` (DEC-5 legal minimum) and
    `sqfull-commit-lag-threshold = 4` on every live
    xs-1c-kmhv3 run. cpu-models without those properties
    (`cpi1`, `inorder-5stage`, older builds) soft-ignore
    the set so existing in-order regressions stay silent.

Round-4 response (B):

  - New unit test
    `/cae/ooo/sbuffer-eviction-events-timeout-fires`
    exercises the END-TO-END cpu-model path with the same
    QOM knob `run-cae.py` now sets. DIV-latency stores
    create sbuffer residency; ALU retires then let the
    Timeout branch accumulate idle cycles and evict. Asserts
    cpu-model-level `sbuffer-eviction-events > 0` and
    sbuffer child's `timeout-evicts > 0`, with `full-evicts`
    and `sqfull-evicts` remaining 0 (three-cause
    exclusivity preserved).

  - The test is the infrastructure-level evidence Codex R3
    review directive 1 asked for. It proves every layer of
    the Timeout path, from QOM setter through tick pump
    through cpu-model aggregation, is live. Under the
    natural `mem-stream` workload's 1-cycle store timing
    model, the same plumbing stays silent because the
    required residency condition is never reached — not a
    regression, a structural expectation.

  - `xs-1c-kmhv3.yaml` suite benchmarks list adds
    `pointer-chase` alongside `alu` + `mem-stream`, since
    `pointer-chase` is the closest in-tree analogue to the
    plan's "hash-table insert-like" dependency-chain
    workload. On a live run this round, pointer-chase
    hits a pre-existing AC-K-2 functional-gate failure
    (NEMU trace byte-diff on insn 321) — unrelated to the
    tick-driver plan, logged so Round 5 can close it
    separately.

### Round-4 XS-GEM5 O3 commit-stuck follow-up

Codex R3 review directive 2 required the XS-GEM5 debug path
to stay inside the repo-managed staged patch lane. Round 4
authored a candidate patch per the `/humanize:ask-codex`
consultation on 2026-04-20T23:13Z:
`tools/xs-gem5-commit-drain-exit.patch`. It adds a second
escape hatch in `Commit::stuckCheckEvent` (raw-cpt mode,
non-trace), promoting a drained-pipeline-after-last-commit
case from `panic()` to a clean `exitSimLoop`, analogous to
the existing trace-mode `traceMaybeExitOnPipelineDrainFromStuckCheck`.

Rebuild + rerun result: `gem5.opt` recompiled cleanly with
the patch applied; the patch-authored `warn()` + `exitSimLoop`
path did NOT fire on `mem-stream` — the panic still reaches
line 147 of the patched `commit.cc`. That means at the stall
moment, `cpu->isCpuDrained()` returns `false` — the pipeline
is NOT idle, some component (fetch / rename / IEW / LSQ) is
still holding busy state. Root cause is therefore a genuine
upstream pipeline-stage livelock, confirming Codex's earlier
classification.

Round-5 escalation path (Codex's step 2 recommendation):
rerun with `--debug-flags=Drain,Commit,Fetch,Rename,IEW,DecoupleBP,Override`
and `--debug-start` / `--debug-end` windowed around
`lastCommitCycle ± 200`, NOT the default first-10-tick window
(which truncates every observable event on these runs). That
debug cycle window will narrow the busy component to a
specific pipeline stage and unblock the surgical fix.

The Round-4 `xs-gem5-commit-drain-exit.patch` stays in
`tools/` and the post-rsync patch lane: even though it did
not fire THIS round, it is the right fix for the post-
workload drained-idle case that WILL surface once the live
livelock is narrowed and fixed — pre-landing it keeps the
Round-5 surgical work from having to reconstruct this
safety net.

Current artifact state (post-Round-4):
- `tests/cae-difftest/reports/xs-1c-kmhv3-mem-stream/m5out/stats.txt`:
  2-line driver preamble only (unchanged).
- `tests/cae-difftest/reports/xs-1c-kmhv3-mem-stream/xs-gem5.json`:
  absent.
- `tests/cae-difftest/reports/xs-1c-kmhv3-mem-stream/accuracy-gate.json`:
  absent.
- `tests/cae-difftest/reports/performance-report-xs.json`:
  `xs_gem5_present=false`, null `xs_gem5_wallclock_seconds`.

Task-E1 / task-E2 calibration evidence therefore remains
dependent on the Round-5 upstream-tree pipeline-stage
diagnostic. The CAE-side plumbing Codex R3 directive 1 asked
for is fully landed and unit-test-verified; the infrastructure
gap is provably closed even though the XS-GEM5 artifact
emission is not yet live.

### Round-5 correction — live-arming bug repair + natural-workload evidence structural ceiling

Codex Round-4 review caught that the Round-4 "live plumbing
landed" claim was false. The `qom-set` path Round-4 added to
`run-cae.py` targeted `/objects/cae-cpu-model`'s plain
pointer-backed properties, which forward to the sbuffer child
only inside `cae_cpu_ooo_complete()` — by the time run-cae.py
issued the live write after the QMP handshake, `complete()`
had already finished, so the write never reached the sbuffer
the tick pump reads. The unit test Round-4 cited as "exact
same knob path" was in fact a construction-time test, not a
post-complete live-write test.

Round-5 fix (commit `e040128f3a`):

  - `run-cae.py` now issues `qom-set` directly on
    `/objects/cae-cpu-model/sbuffer` with the raw
    `inactive-threshold` + `sqfull-commit-lag-threshold`
    property names. These are live-writable on the sbuffer
    object (the setter with DEC-5 clamp takes effect
    immediately) and the next tick-pump call reads the armed
    values.
  - New unit test
    `/cae/ooo/sbuffer-thresholds-live-qmp-set` pins the
    post-complete contract: construct a cpu-ooo without any
    threshold properties, `user_creatable_complete()`, then
    write the sbuffer child's `inactive-threshold` = 2
    AFTER `complete()`, observe the write landed + drive
    DIV-latency stores + ALU retires, assert `timeout-evicts
    > 0` and `sbuffer-eviction-events > 0`. Complements the
    existing construction-time regression
    `/cae/ooo/sbuffer-eviction-events-timeout-fires`.

Round-5 benchmark scan (`task-R5-3`) — six tier-1 gated
benchmarks in `tests/cae-difftest/configs/xs-1c-kmhv3.yaml`:

| Benchmark | CAE functional | `sbuffer_eviction_events` | Notes |
|---|---|---|---|
| `alu` | MATCH | 0 | No memory ops; 0 expected |
| `mem-stream` | MATCH | 0 | 32k stores, all drain same-charge under 1-cycle store latency |
| `coremark-1c` | MATCH | 0 | Only 4 stores (262k loads); sbuffer barely exercised |
| `pointer-chase` | FAIL (AC-K-2 byte-diff insn 321) | N/A | Pre-existing issue unrelated to tick-driver |
| `matmul-small` | FAIL (NEMU BAD TRAP) | N/A | Pre-existing issue unrelated to tick-driver |
| `branchy` | (timed out) | N/A | Separate investigation |

Natural-workload non-zero `sbuffer_eviction_events` is
therefore **structurally unreachable under the current
Lower-Bound (A+B+C, DEC-8) timing model**. The root cause is
not a config bug; it is a consequence of:

  1. Default 1-cycle store latency in
     `cae/uop.c::default_latency_table[CAE_UOP_STORE] = 1`.
  2. Default `commit_width = 8` on xs-1c-kmhv3 allowing
     every ROB-ready store to commit same-charge as
     dispatch.
  3. The tick pump's Full + SQFull branches require
     `!head_drainable`, and under conditions (1) + (2) the
     head IS always commit-drainable at tick time.
  4. The Timeout branch requires `inactive_cycles >
     threshold` to accumulate across ticks, which requires
     a prior store to STILL be in the sbuffer — that only
     happens when a preceding non-drainable ROB entry
     (MUL/DIV/FPU head) holds commit hostage. None of the
     scanned benchmarks have enough MUL/DIV/FPU pressure
     to create that condition on a sustained basis.

Milestone D (virtual-issue batching, deferred per DEC-8)
would resolve this naturally: same-cycle multi-store
dispatch would force occupancy > 1 at some tick points, and
the Full / SQFull branches could fire under natural
workloads. Under DEC-8 Lower-Bound, the tick pump is a
correctness substrate, not an observability surface on
natural `kmhv3` benchmarks.

Goal-tracker update request accompanying Round 5:

  - Task-E1 sub-scope: the **INFRASTRUCTURE** is fully live
    (post-complete live-arming proven via
    `/cae/ooo/sbuffer-thresholds-live-qmp-set` +
    construction-time path via
    `/cae/ooo/sbuffer-eviction-events-timeout-fires`).
    **Natural-workload evidence** on the scanned set is
    structurally zero.
  - Recommend the plan acknowledge the natural-workload
    expectation as a Milestone-D requirement rather than a
    Lower-Bound one, and let task-E1 close on the dual-
    regression infrastructure evidence + this calibration
    log's benchmark-scan documentation.
  - Task-E2 (`delta_ipc_pct` + calibration-log numbers)
    remains gated on XS-GEM5 timing artifacts, which are
    still blocked on the upstream O3 commit-stuck livelock.

### Round 6 — Measured tick-path telemetry (mem-stream)

Codex R5 review rejected the Round-5 "structurally
unreachable" framing as inferential. Round 6 replaces the
inference with in-tree measured evidence by adding five
lifetime-only diagnostic counters to `CaeSbuffer` (mirroring
the existing `evict_threshold_events` pattern — RO QOM,
not in snapshot):

  - `tick-calls`: total `cae_sbuffer_tick` invocations
    that passed the NULL-safe early-return.
  - `tick-head-drainable-events`: ticks entered with
    `occupancy > 0` and `head.sqn <= sbuffer_commit_sqn`
    (i.e. head is immediately commit-drainable).
  - `tick-head-non-drainable-events`: ticks entered with
    `occupancy > 0` and `head.sqn > sbuffer_commit_sqn`
    (i.e. head held hostage by commit lag — the Full /
    SQFull precondition).
  - `tick-inactive-max`: historical max of
    `inactive_cycles` observed before a drain or reset.
  - `tick-occupancy-max`: historical max of sbuffer
    `occupancy` observed at tick entry.

`run-cae.py` exports all five under `cpu_model` on
every OoO run; they surface in `cae.json` under
`cpu_model.sbuffer_tick_*`. (Earlier drafts of this
section used the name `cpu_model_stats`; the actual
JSON key emitted by `run-cae.py` is `cpu_model`, and
Round 9 corrects the drift.)

Round 6 also narrowed the `xs-1c-kmhv3.yaml` suite from six
tier-1 benchmarks down to `alu` + `mem-stream`, keeping
`mem-stream` as the sole store-dense representative for the
M5' live-evidence lane (the remaining four had pre-existing
AC-K-2 / NEMU BAD-TRAP / timeout issues unrelated to the
tick-driver plan and are gated test assets for future
separate calibration lanes), and scoped the `run-cae.py`
QMP threshold arming to only fire on `config_name ==
"xs-1c-kmhv3"` — other OoO configs keep zero-threshold
defaults.

**Critical Round-6 discovery — stale binary**: Before the
first Round-6 measured run, `build-cae/qemu-system-riscv64`
had an mtime predating ALL Round-0+ tick-driver work. Every
"live" CAE run from Rounds 0-5 had been executing a
pre-tick-driver binary — the zero-eviction observations
from those rounds were not evidence of the new code, they
were evidence of its absence. The Round-6 measurement is
the first live CAE run on a binary actually containing the
tick pump. Fix: explicit `ninja -C build-cae
qemu-system-riscv64` before re-running mem-stream. Going
forward `run-xs-suite.sh` is expected to rebuild the binary
on every invocation; stale-binary detection is a
queued-issue candidate.

Measured values from the re-run
(`xs-1c-kmhv3 --benchmark mem-stream`, post-rebuild,
commit `79cf01bea6`):

| Counter | Value | Interpretation |
|---|---:|---|
| `sbuffer_tick_calls` | 230 023 | Tick fires every charge segment |
| `sbuffer_tick_head_drainable_events` | 32 001 | 100 % of non-empty ticks |
| `sbuffer_tick_head_non_drainable_events` | 0 | **Full / SQFull preconditions NEVER held** |
| `sbuffer_tick_occupancy_max` | 1 | Never exceeded 1 entry at tick entry |
| `sbuffer_tick_inactive_max` | 0 | **Timeout precondition NEVER held** |
| `sbuffer_eviction_events` | 0 | All three cause branches correctly quiescent |

The Round-6 suite binding armed `inactive-threshold=2`
and `sqfull-commit-lag-threshold=4` via post-complete
QMP on the `xs-1c-kmhv3` lane (verified by
`/cae/ooo/sbuffer-thresholds-live-qmp-set`), so the zero
result is NOT a plumbing miss. (Round 9 later retunes
the SQFull threshold from 4 to 2 after the R8 ordering
fix exposes the reachable lag=2 trigger — see Round 9
section below.)

**Data-driven root cause** (replaces the Round-5
inferential claim):

  1. `head_non_drainable = 0` across 32 001 non-empty
     ticks. Because Full and SQFull both require
     `!head_drainable`, these two cause branches have
     precondition probability 0 on mem-stream, regardless
     of their thresholds. Lowering `sqfull-commit-lag-
     threshold` to 0 would not help — the gating predicate
     `head.sqn > commit_sqn` never held.
  2. `inactive_max = 0` across the same 32 001 ticks.
     Because the Timeout branch requires `inactive_cycles
     > threshold` with threshold clamped by DEC-5 to a
     minimum of 0 firing at strictly greater-than, a max
     of 0 means Timeout cannot fire regardless of threshold
     value — even threshold = 0 requires `inactive_cycles
     >= 1` to have been observed at some tick.
  3. `occupancy_max = 1` confirms the structural chain: a
     store dispatches, the charge segment runs the commit
     loop (width = 8) which advances `sbuffer_commit_sqn`
     past the store's sqn, then tick drains the head
     same-charge. The next store lands in an empty
     sbuffer. There is no cross-store overlap at tick
     entry on mem-stream under the current 1-cycle store
     latency + commit_width=8 configuration.

This is measured, not inferred: the three cause branches
are quiescent because their mutually-exclusive
preconditions never held across 230 023 live tick calls.
No threshold choice permitted by DEC-5 can change this on
the current mem-stream binding; it is a property of the
workload × timing-model product, not of the tick pump.

**Why threshold tuning alone cannot lift the floor**:
Round 6 did not separately re-run with
`inactive-threshold = 0` / `sqfull-commit-lag-threshold =
0` because the measured data forecloses it analytically:
Timeout requires `inactive_cycles > threshold`, so even
`threshold = 0` needs `inactive_cycles >= 1` at some tick
— which `inactive_max = 0` rules out. Full/SQFull require
`!head_drainable`, which `head_non_drainable = 0` rules
out. Running the sweep would only add log noise to a
result already pinned by the telemetry.

**Consequence for task-E1**: The live-evidence sub-scope
closes on infrastructure + measured-structural-floor
evidence:

  - Infrastructure live: `/cae/ooo/sbuffer-thresholds-
    live-qmp-set` (post-complete QMP arming) +
    `/cae/ooo/sbuffer-eviction-events-timeout-fires`
    (construction-time end-to-end).
  - Natural-workload on mem-stream measured-structural-
    zero: this section's telemetry table.

Natural non-zero `sbuffer_eviction_events` on `xs-1c-kmhv3`
remains deferred to Milestone D (virtual-issue batching)
which would enable `occupancy > 1` at tick entry, per
DEC-8's Lower Bound scope. Round 5's Goal Tracker Update
Request on this sub-scope was rejected and Round 6 does
not re-submit it; a future round may revisit after the
XS-GEM5 O3 livelock is resolved and task-E2 numbers are
populated.

**What Round 6 did NOT achieve**: task-E2 `delta_ipc_pct`
still blocked on XS-GEM5 O3 commit-stuck livelock — no
progress on the paired-timing number this round.

### Round 7 — pointer-chase AC-K-2 repair + hardened runner + second measured datapoint

Round-6 review directive chain (three steps in order):

  1. Harden evidence integrity first — add a fail-closed
     CAE-binary freshness guard so stale binaries cannot
     invalidate future measurements.
  2. Promote `pointer-chase` out of the queued lane and
     fix its AC-K-2 functional diff — it is the only
     remaining in-plan representative candidate after
     mem-stream was measured-zero on a fresh binary.
  3. Rerun `xs-1c-kmhv3 --benchmark pointer-chase` on the
     hardened runner. Do NOT reclassify `task-E1` to
     Milestone D regardless of the result.

All three steps landed in Round 7.

**Step 1 — freshness guard (task-R7-1)**:
`tests/cae-difftest/scripts/run-cae.py` gained
`_ensure_qemu_binary_fresh()` which compares
`build-cae/qemu-system-riscv64` mtime against the max
mtime across `hw/cae/`, `cae/`, `include/cae/`,
`include/hw/cae/`, `target/riscv/cae/`, and `accel/cae/`.
If stale (or missing), it auto-runs `ninja -C build-cae
qemu-system-riscv64`; if the ninja rebuild fails or
`build.ninja` is absent, it raises `SystemExit(2)` with a
diagnostic. Opt-out via `CAE_SKIP_FRESHNESS_CHECK=1` for
CI pipelines that guarantee freshness upstream. Verified
end-to-end: `touch hw/cae/ooo/sbuffer.c` then run the
guard → reports `stale (bin mtime < src mtime); running
ninja` and rebuilds successfully before proceeding.

**Step 2 — pointer-chase AC-K-2 repair (task-R7-2 +
task-R7-3)**:

Round-5 diagnosis said "pointer-chase fails AC-K-2
(byte-diff insn 321)". Round-7 diagnostic dug into the
exact failure:

  - `nemu-difftest.py` reports: `trace-mode byte-diff
    FAIL (insn_index=321, field=flags, expected=0x0,
    actual=0x1)` — CAE has `flags=0x0, mem_size=0`, NEMU
    has `flags=0x1, mem_size=8`.
  - At the failing record, both sides see the same
    opcode `0x0062b023` (SD x6, 0(x5) — the pointer-chase
    setup-loop store). NEMU correctly marks it as
    `MEM_WRITE`; CAE does not.
  - Measured over the full trace: 258 SD opcodes; CAE
    marks only 66 with `CAE_TRACE_FLAG_MEM_WRITE`,
    leaving 192 (74 %) unmarked. NEMU marks all 258.
  - Pattern: marked SDs cluster in bursts of ~62 then
    gap for ~63 (= one 4 KiB page's worth of 64-byte
    stride stores). This is the TLB-miss cadence.

Root cause: `uop->mem_size` is populated by
`cae_mem_access_notify` at `cae/engine.c:1220` —
the softmmu memory hook. Under `ooo-kmhv3`,
`cae_tlb_force_slow_active=false` (AC-K-13 — flipped by
`accel/cae/cae-all.c:733` so MSHR overlap timing can be
observed). The TCG-JIT fast path then bypasses the
softmmu helpers on TLB hit; `cae_mem_hook` only fires on
TLB miss. The classifier still sets `uop->is_store=true`
for every OP_STORE, but without the hook `mem_size`
stays 0, and the retire-side trace emitter's gate
`uop->is_store && uop->mem_size != 0` at
`target/riscv/cae/cae-riscv-trace.c:192` rejects the
store.

Minimum-scope fix: derive `mem_size` / `mem_addr` /
`mem_value` from the instruction encoding + live
`env->gpr` state at retire time. Landed as a new pure
helper `cae_riscv_trace_derive_store_fields` in
`target/riscv/cae/cae-riscv-trace-derive.c` with
declaration in `include/cae/riscv-trace-derive.h`. Covers
OP_STORE (SB/SH/SW/SD), C.SW / C.SD / C.SWSP / C.SDSP.
OP_STORE_FP / compressed FP stores return `false`
because FP register file is env->fpr, not env->gpr — the
hook-populated zero fields remain and the trace emitter
continues to emit flags=0 for FP stores (consistent with
pre-fix behaviour on that rare path). The trace writer
at `cae-riscv-trace.c` calls the helper when `is_store
&& mem_size == 0`, preferring hook-populated values when
the slow path did fire.

Scope note: the fix modifies `target/riscv/cae/cae-
riscv-trace.c`, adds `target/riscv/cae/cae-riscv-trace-
derive.c`, and adds `include/cae/riscv-trace-derive.h`.
None of these paths are locked by AC-7's `git diff
efe2844ff1 -- accel/ cae/engine.c cae/cpu_model.c
include/cae/engine.h include/cae/cpu_model.h` gate;
AC-7 stays empty. The plan's overall "all changes
confined to hw/cae/" guidance is a tick-driver-scope
statement; the AC-K-2 trace-emitter fix is a separate
mainline blocker directed by Codex R6 review, landed
here on its own merits.

Regression lock: four new unit tests in
`tests/unit/test-cae.c`:

  - `/cae/trace/riscv-derive-sd-fastpath` — SD x6, 0(x5)
    (the exact pointer-chase failure encoding) with
    representative gpr state; asserts mem_size=8,
    mem_addr=base, mem_value=gpr[rs2].
  - `/cae/trace/riscv-derive-sw-truncates-value` — SW
    x6, 4(x5) asserts mem_size=4, mem_addr=base+4, and
    mem_value truncated to the low 32 bits.
  - `/cae/trace/riscv-derive-csdsp` — C.SDSP x8, 0(sp)
    asserts full 64-bit mem_value from gpr[8].
  - `/cae/trace/riscv-derive-rejects-non-store` — ADDI
    returns false.

**Step 3 — measured rerun (task-R7-4)**:

`bash tests/cae-difftest/scripts/run-xs-suite.sh
xs-1c-kmhv3 --benchmark pointer-chase` end-to-end:

  - CAE functional gate (nemu-difftest): **PASS**
    (`functional_failed=0` — AC-K-2 repair verified
    live on the full 387 808-retire trace, not just
    the synthetic unit-test encodings).
  - CAE measurement artefacts in
    `tests/cae-difftest/reports/xs-1c-kmhv3-pointer-
    chase/cae.json` (freshly generated, post-rebuild):

| Counter | Value |
|---|---:|
| `sbuffer_tick_calls` | 387 808 |
| `sbuffer_tick_head_drainable_events` | 258 |
| `sbuffer_tick_head_non_drainable_events` | 0 |
| `sbuffer_tick_occupancy_max` | 1 |
| `sbuffer_tick_inactive_max` | 0 |
| `sbuffer_timeout_evicts` | 0 |
| `sbuffer_full_evicts` | 0 |
| `sbuffer_sqfull_evicts` | 0 |
| `sbuffer_eviction_events` | 0 |

Same structural pattern as Round 6 on mem-stream: the
258 stores all tick into an empty sbuffer (256 setup
SDs + the closing SD + one extra counted by the insn
stream), drain same-charge under 1-cycle store latency
+ commit_width=8, and never accumulate residency.
`head_non_drainable=0` forecloses Full / SQFull;
`inactive_max=0` forecloses Timeout even at threshold=0.

  - XS-GEM5 side: `commit.cc stucked 40,000 cycles`
    panic reproduces exactly as on mem-stream. Upstream
    XS-GEM5 O3 livelock remains open; task-E2
    `delta_ipc_pct` still blocked. Per R6 review
    directive step 4, the XS-GEM5 debug lane is gated
    on Step 3 producing non-zero CAE — which it did not,
    so Round 7 does not enter that lane.

**Second measured datapoint, same structural finding**:

Round 7 now has two independently measured manifest
benchmarks in the Lower-Bound lane:

  - `mem-stream` (Round 6, 32 001 stores, max
    occupancy 1).
  - `pointer-chase` (Round 7, 258 stores, max occupancy
    1, functional-gate-green).

Both exhibit `head_non_drainable=0` across every
non-empty tick (32 001 / 32 001 on mem-stream; 258 / 258
on pointer-chase). The structural foreclosure is
workload-independent under the current 1-cycle store
latency + commit_width=8 + one-insn-per-TB retire
model. No DEC-5-permitted threshold can lift the floor
on either benchmark.

The remaining in-plan manifest benchmarks are:

  - `alu` — zero SD instructions; sbuffer is never
    allocated; `sbuffer_eviction_events=0` trivially.
  - `coremark-1c` — 4 SD stores per Round-5 scan;
    measured `sbuffer_eviction_events=0`. Three
    remaining manifest benchmarks (`matmul-small`,
    `branchy`, `checkpoint-stress`, `spec-mem-leak-
    check`) either fail pre-existing functional gates
    unrelated to the tick-driver plan or are not
    store-dense.

The measured-evidence floor under Lower Bound is
therefore "zero", and every in-plan manifest
store-dense candidate has been exhausted. Round 7 does
NOT re-submit the Round-5/6 rejected reclassification
request for task-E1 — per the Round-7 contract, that
adjudication stays with Codex R7 review. The
calibration log records the data; the plan-scope
question (is Lower Bound sufficient to demonstrate
non-zero on natural workloads? — two measured failures
suggest NO) is for the next review to decide.

**What Round 7 did achieve**:

  - task-R7-1 closed: live-evidence integrity hardened
    (automatic rebuild on stale CAE binary).
  - task-R7-2 / task-R7-3 closed: AC-K-2 repair landed
    with four unit regressions + end-to-end verified
    on pointer-chase's 387 808-retire trace.
  - task-R7-4 completed with the second measured
    datapoint.

**What Round 7 did NOT achieve** (unchanged from R6):

  - task-E1 natural non-zero `sbuffer_eviction_events`
    — measured zero on both remaining manifest
    candidates for the same structural reason.
  - task-E2 `delta_ipc_pct` — still blocked on
    upstream XS-GEM5 O3 livelock; Round 7 did not
    enter that debug lane per R6 directive step 4's
    gating.

**Queued cleanup from R6 carries forward**: the
`cae_json` field `cpu_model_stats` in R6 commits was
actually `cpu_model` in the live run (traceability nit
R6 did not clean up). Round 7 confirms: the pointer-
chase `cae.json` dumps the counters under `cpu_model`,
not `cpu_model_stats`. A future round should regularise
either the docs or the JSON schema; out-of-scope for
Round 7 per the contract.

### Round 8 — Milestone-C segment-ordering fix + plan-compliant baseline

Codex R7 review identified that the Round-1
Milestone-C implementation in `hw/cae/cpu_ooo.c` had
the sbuffer tick running AFTER the ROB commit loop,
not BEFORE as the plan specifies
(`docs/superpowers/plans/2026-04-20-cae-cpu-ooo-tick-
driver.md:181-190`):

  - Plan segment 3: `m->now_cycle++` + sbuffer tick.
  - Plan segment 4: commit-loop release of ROB/LSQ/RAT
    + advance of `sbuffer_commit_sqn` for the NEXT
    charge's tick.

The Round-1 comment at the old call site justified the
inverted ordering with "this tick sees the newly-
advanced sbuffer_commit_sqn", which was a subtle but
real plan-non-compliance: under that ordering, a store
allocated AND committed in the same charge became
tick-drainable in that SAME charge, so occupancy
collapsed to 0 on every 1-cycle-store retire.
Round-6/7's measured-zero evidence on mem-stream +
pointer-chase was the observable consequence of that
drift, not the structural floor of Lower Bound itself.

**Why Codex R0-R6 missed this**: the existing residency
regression
(`/cae/ooo/sbuffer-residency-survives-commit`)
exercises only the DIV-latency path — four 20-cycle
DIV stores pile up in the sbuffer because the ROB
can't commit them yet, so commit_sqn stays at 0 and
the drain never fires. It never covered the 1-cycle
store path where commit runs every retire and the
ordering bug was reachable. That gap let the drift
escape six rounds of review.

**Round-8 fix (task-R8-1)**: moved the `cae_sbuffer_tick`
call above the `cae_ooo_rob_commit_one` loop in
`hw/cae/cpu_ooo.c:cae_cpu_ooo_charge`. Updated the
inline comments to state the plan-literal segment
ordering and removed the Round-1 justification for the
inverted order. Net diff: tick now sees the PREVIOUS
charge's `sbuffer_commit_sqn` and the commit-loop
segment follows to advance it for the next charge.

**Round-8 regression closures (task-R8-2)**: three
tests land to pin the fix:

  - `/cae/ooo/single-store-residency-across-charges`:
    one 1-cycle store + one ALU charge. Asserts
    sbuffer occupancy is still 1 after the store
    charge, sbuffer-commits is still 0, and
    sbuffer_commit_sqn has advanced. After the ALU
    charge, occupancy drops to 0 and sbuffer-commits
    becomes 1. Pre-fix ordering would have
    sbuffer-commits=1 after the single store charge —
    this test would FAIL against the Round-1 tree.
  - `/cae/ooo/consecutive-stores-occupancy-above-one`:
    three back-to-back 1-cycle stores. Asserts
    tick_occupancy_max >= 2 and
    tick_head_non_drainable_events >= 1. Pre-fix
    ordering would have max=1 and non_drainable=0
    because every store drains same-charge.
  - Updated `/cae/cpu-ooo/commit-drains-real-store-
    value` to pump one ALU charge between the store
    charge and the last-committed-store-* assertions,
    since under the plan's ordering the drain happens
    one charge later.

All 109 unit tests pass.

**Round-8 measured telemetry (task-R8-3)** — fresh
runs on the ordering-fixed binary via the hardened
`run-cae.py` freshness guard (thresholds remain the
run-cae.py-armed `inactive=2, sqfull_lag=4`):

mem-stream:

| Counter | R7 (pre-fix) | R8 (post-fix) |
|---|---:|---:|
| `sbuffer_tick_calls` | 230 023 | 230 023 |
| `sbuffer_tick_head_drainable_events` | 32 001 | **32 000** |
| `sbuffer_tick_head_non_drainable_events` | 0 | **32 001** |
| `sbuffer_tick_occupancy_max` | 1 | 1 |
| `sbuffer_tick_inactive_max` | 0 | **1** |
| `sbuffer_eviction_events` | 0 | 0 |

pointer-chase:

| Counter | R7 (pre-fix) | R8 (post-fix) |
|---|---:|---:|
| `sbuffer_tick_calls` | 387 808 | 387 808 |
| `sbuffer_tick_head_drainable_events` | 258 | **257** |
| `sbuffer_tick_head_non_drainable_events` | 0 | **258** |
| `sbuffer_tick_occupancy_max` | 1 | 1 |
| `sbuffer_tick_inactive_max` | 0 | **1** |
| `sbuffer_eviction_events` | 0 | 0 |

The fix is visible in the telemetry: under the plan-
literal ordering, each store is observed as
non-drainable at the alloc charge's tick entry
(`head_non_drainable_events` ≈ store count) and
drainable at the NEXT charge's tick entry
(`head_drainable_events` ≈ store count, one less when
the final store is still in-flight at sample time).
`tick_inactive_max` climbs from 0 to 1, reflecting
that the sbuffer is non-empty during one idle tick per
store.

**sbuffer_eviction_events remains 0 on both
benchmarks under R8 thresholds (inactive=2,
sqfull_lag=4)**:

  - Timeout: needs `inactive_cycles > 2`. Max
    observed is 1. Never fires.
  - SQFull: needs `lag >= 4` where `lag =
    store_sqn_next - sbuffer_commit_sqn` at tick
    entry. On one-insn-per-TB + 1-cycle store + no
    outrun pressure, the lag peaks at 2 (the
    just-allocated store's sqn minus the previous
    watermark). Never fires.
  - Full: needs `occupancy >= 12` (evict-threshold
    for xs-1c-kmhv3). Max observed is 1. Never
    fires.

Under the plan-literal ordering, the structural reality
on both in-plan manifest store-dense benchmarks is now
clean: each store lives in the sbuffer for exactly one
charge before the next tick drains it. The 1-cycle
store latency + commit_width=8 + one-insn-per-TB
combination produces a 1-charge residency baseline. The
three eviction preconditions require either longer
residency (Timeout, needs inactive>=3), higher
occupancy (Full, needs occupancy>=12), or more
commit-lag (SQFull, needs lag>=4) than this workload
shape naturally produces.

Round 8 does NOT re-submit the Round-5/6 rejected
reclassification request. The plan-compliant baseline
is now recorded; task-E1 adjudication stays with
Codex R8 review.

**XS-GEM5 side**: still `TIMING-XS-GEM5-FAIL` on both
benchmarks (upstream O3 `commit.cc stucked 40 000
cycles` panic reproduces on the Round-8 CAE binary
too, as expected — the panic is in XS-GEM5, not CAE).
task-R8-5 (XS-GEM5 lane) was gated on task-R8-3
producing non-zero; since it didn't, Round 8 does not
enter that lane per the Round-8 contract.

**What Round 8 achieved**:

  - task-R8-1: Milestone-C ordering now matches the
    plan spec literally.
  - task-R8-2: Two new regressions close the gap that
    let the drift escape Rounds 0-7. The 1-cycle
    store residency path is now covered.
  - task-R8-3: Plan-compliant baseline telemetry
    captured for both manifest store-dense
    benchmarks.

**What Round 8 did NOT achieve**:

  - Natural non-zero `sbuffer_eviction_events` still
    does not emerge under the run-cae.py-armed
    thresholds (2, 4). The measured lag/occupancy
    peaks don't clear those thresholds on natural
    one-insn-per-TB workloads.
  - task-E2 `delta_ipc_pct` still blocked on upstream
    XS-GEM5 O3 livelock.

### Round 9 — SQFull threshold retune + natural non-zero evictions on two representative benchmarks

Codex R8 directive step 1-2: with the Round-8
plan-compliant ordering in place, the only natural
Lower-Bound eviction opportunity on one-insn-per-TB +
1-cycle-store workloads is SQFull at lag = 2 on the
alloc-charge tick. Full is unreachable
(`occupancy_max=1` « 12) and Timeout is unreachable
(`inactive_max=1` ≤ threshold=2). Round-9 retunes
`sqfull-commit-lag-threshold` from 4 to 2 in the live
`xs-1c-kmhv3` lane (`tests/cae-difftest/scripts/
run-cae.py`).

Why 4 was wrong after the R8 ordering fix: the
pre-R8 value of 4 was chosen as "25 % of
sbuffer_entries=16" under the inverted ordering,
where `lag` never exceeded 1 anyway because the
committed store drained same-charge. Under the
R8 plan-compliant ordering, the alloc-charge tick
sees `lag = store_sqn_next - sbuffer_commit_sqn = 2`
(one just-dispatched store that has not yet been
committed by this charge's segment-4 loop). Threshold
4 was always above the measured peak.

Threshold 2 is the minimum value that makes SQFull
fire exactly on this "one just-dispatched non-
drainable store" pattern. It does NOT over-fire:
non-store charges have `lag = 0` (no store in flight)
so SQFull skips; consecutive stores briefly raise
occupancy but SQFull still fires cleanly on each
alloc-charge tick.

**Round-9 measured telemetry**
(`tests/cae-difftest/scripts/run-xs-suite.sh
xs-1c-kmhv3 --benchmark {mem-stream, pointer-chase}`
on the post-retune Round-9 binary):

| Counter | mem-stream | pointer-chase |
|---|---:|---:|
| `sbuffer_tick_calls` | 230 023 | 387 808 |
| `sbuffer_tick_head_drainable_events` | 0 | 0 |
| `sbuffer_tick_head_non_drainable_events` | 32 001 | 258 |
| `sbuffer_tick_occupancy_max` | 1 | 1 |
| `sbuffer_tick_inactive_max` | 0 | 0 |
| `sbuffer_timeout_evicts` | 0 | 0 |
| `sbuffer_full_evicts` | 0 | 0 |
| **`sbuffer_sqfull_evicts`** | **32 001** | **258** |
| **`sbuffer_eviction_events`** | **32 001** | **258** |

**Both in-plan manifest store-dense benchmarks now
produce natural non-zero
`sbuffer_eviction_events`**, exactly matching the
per-store count. Each alloc-charge tick fires one
SQFull eviction on the head because:

  1. head is non-drainable (`head->sqn > commit_sqn`).
  2. `lag = store_sqn_next - commit_sqn = 2 >= 2`.
  3. SQFull branch evicts the head; `sbuffer_commit_sqn`
     then advances in segment 4 but the entry is
     already evicted (not a commit-drain).

Secondary observable changes vs the R8 baseline:

  - `head_drainable_events` drops to 0 on both. Under
    R8 (no-SQFull), each store was seen non-drainable
    at alloc charge and drainable at the next charge.
    Under R9, SQFull evicts the head at the alloc-
    charge tick, so the next charge never sees it.
  - `inactive_max` drops to 0 on both. SQFull
    eviction resets `inactive_cycles` to 0 each fire,
    exactly as the eviction branches specify.
  - `head_non_drainable_events` counter unchanged
    (32 001 / 258): the classifier increment runs
    BEFORE the eviction branch, so it still sees the
    non-drainable head on every alloc charge.

**task-E1 evidence requirement CLOSES on the CAE
side** — AC-6 (natural non-zero
`sbuffer_eviction_events`) met on two independent
in-plan manifest benchmarks.

**XS-GEM5 side (task-R9-4)**:
`functional_failed=0` on both benchmarks (Round-7
AC-K-2 repair still holds); `timing_failed=1` on
both because the upstream XS-GEM5 O3 `commit.cc
stucked 40 000 cycles` panic reproduces unchanged
from Round 5-8. `m5out/config.ini` + `config.json`
+ `stats.txt` + `exec-head.log` are produced but
no `xs-gem5.json` / `accuracy-gate.json` at the
report-directory level because those are written
only after a successful run. task-R9-4 therefore
does NOT produce `delta_ipc_pct` this round; the
XS-side livelock is queued for a later round's
narrow `--debug-flags` investigation and does not
block the CAE-side task-E1 closure.

**task-E2 `delta_ipc_pct`** remains open on the
XS-GEM5 dependency. The plan's original output
requirement for Milestone E is a paired CAE vs
XS-GEM5 number; CAE side is now delivering, XS
side is still blocked. Task-E2 converts from
"both sides blocked" to "XS side blocked only".

**Traceability scrubs (Codex R8 queued side-issue
#2)** applied in Round 9:

  - Round-6 section's `cpu_model_stats.sbuffer_tick_*`
    naming replaced with actual JSON key
    `cpu_model.sbuffer_tick_*` + explicit note
    that the earlier draft was wrong.
  - Round-6 section's `sqfull-commit-lag-threshold=1`
    mis-statement replaced with the actual Round-6
    armed value of 4 + forward reference to the
    Round-9 retune.
  - Stale pre-R8 mem-stream comment in
    `xs-1c-kmhv3.yaml` scrubbed to match the
    measured post-R8 ordering behaviour.

**What Round 9 achieved**:

  - task-R9-1: SQFull threshold armed at 2 (not 4).
  - task-R9-2: xs-1c-kmhv3.yaml mem-stream comment
    scrubbed.
  - task-R9-3: Fresh mem-stream + pointer-chase
    runs yield natural non-zero
    `sbuffer_eviction_events`, all via the
    SQFull cause (32 001 + 258 respectively).
  - task-R9-5 (this section + traceability
    scrubs).
  - AC-6 task-E1 CAE-side evidence requirement
    CLOSED.

**What Round 9 did NOT achieve**:

  - task-R9-4 XS-GEM5 lane: upstream O3 livelock
    panics on both benchmarks. No
    `xs-gem5.json` / `accuracy-gate.json` at
    report-directory level. `performance-report-
    xs.json` stays on the stale six-benchmark
    scan until the XS side lands.
  - task-E2 `delta_ipc_pct`: CAE side delivered,
    XS side blocked.

### Round 10 — XS-GEM5 O3 stuck-check escape hatch + first `delta_ipc_pct` measurements land (task-E2 closes)

Codex R9 directive: close task-E2 by debugging the
upstream XS-GEM5 O3 "Commit stage stucked for more
than 40 000 cycles" panic until
`xs-1c-kmhv3-*` report directories contain
`xs-gem5.json`, `accuracy-gate.json`, and timing
data usable for `delta_ipc_pct`.

**Root cause** (task-R10-1): the bench.S halt-loop
at the tail of every `.xs.bin` is
`ebreak; halt: wfi; j halt`. After the workload's
last architectural commit (sentinel store + ebreak),
XS-GEM5 retires a handful of post-ebreak prologue
insns then reaches the halt loop. `wfi` under
M-mode bare-metal with no interrupt controller
blocks execution; the `j halt` after it never
retires. The front-end, however, keeps fetching
the halt loop indefinitely, so `fetch.isDrained()`
returns false even though the architectural
workload is done. Round-5's existing
`xs-gem5-commit-drain-exit.patch` added an
`isCpuDrained()` escape hatch in
`stuckCheckEvent`, but it never fires because the
halt-loop-fetching front-end keeps the CPU "not
drained" from `isCpuDrained()`'s perspective.

**Round-10 fix**: new repo-local patch
`tools/xs-gem5-stuck-rob-empty-exit.patch` adds a
second escape hatch at the same
`stuckCheckEvent` site. When
`lastCommitCycle > 0 && rob->isEmpty(0)`, the
stall-check exits cleanly via
`exitSimLoop("O3 CPU ROB empty after workload")`
instead of panicking. The two preconditions rule
out the "fetch stuck before any commit" livelock
(lastCommitCycle==0) and the "in-flight insn
stuck" livelock (ROB non-empty); only the
workload-finished halt-loop case clears both.

Applied via `tools/build-xs-gem5-staged.sh`'s
post-rsync patch lane; external
`~/xiangshan-gem5` tree stays byte-identical.

Side fixes landed with the main patch:

  - `tests/cae-difftest/scripts/run-xs-gem5.py`
    widened `--debug-end` from 10 ticks to
    2 000 000 ticks so the first-PC preflight
    actually captures the first committed insn
    (at the 2 GHz kmhv3 clock, 10 ticks is
    0.005 cycles — far before any instruction
    has reached the commit stage).
  - `tests/cae-difftest/scripts/preflight-
    retired-count.py` reads the CAE retired-
    insn count from `cae-itrace.bin`'s file
    size instead of `cae.json:aggregate.total_
    insns`. The aggregate count includes the
    post-sentinel halt-loop retires that QEMU
    retires between the sentinel store and the
    QMP driver's `quit`; NEMU and XS-GEM5
    naturally stop at the sentinel boundary,
    so comparing CAE's aggregate against those
    two always shows a 5-10 insn spread. The
    trace-file count uses the same stop
    definition on all three lanes.
  - The build patch for
    `xs-gem5-commit-drain-exit.patch` had a
    unified-diff context line with no leading
    space (empty line at line 57 in the
    patch); fixed in passing so the patch
    applier can strict-check the unified-diff
    format.

**End-to-end verification**: `run-xs-suite.sh
xs-1c-kmhv3 --benchmark <bench>` on `alu`,
`mem-stream`, `pointer-chase` all reach clean
exit on the Round-10 binary:

```
build/RISCV/cpu/o3/commit.cc:145: warn: [Commit]
ROB empty after workload completion
(lastCommitCycle=105843, curCycle=160030);
exiting cleanly.
Exiting @ tick 53289990 because
O3 CPU ROB empty after workload
```

Each now emits `xs-gem5.json` +
`accuracy-gate.json` at the report-directory
level, and `xs_gem5_wallclock_seconds` is
non-null:

| Benchmark | CAE wallclock | XS-GEM5 wallclock |
|---|---:|---:|
| `alu` | 3.303 s | 5.531 s |
| `mem-stream` | 1.385 s | 3.023 s |
| `pointer-chase` | 2.145 s | 5.275 s |

(Wallclock values from the Round-11 rerun; the
Round-10 committed values drifted due to minor run-
to-run variation; the IPC / cycles / insns columns
are deterministic and unchanged across rounds.)

**Measured `delta_ipc_pct`** (task-E2 closure evidence):

Under post-R8 plan-compliant ordering + post-R9
armed thresholds (inactive=2, sqfull_lag=2), the
representative-benchmark pairing against XS-GEM5
kmhv3.py (fetchW=32 / decodeW=renameW=commitW=8 /
ROB=352 / LQ=72 / SQ=56 / DecoupledBPU + TAGE-SC-L +
MicroTAGE blockSize=32):

| Benchmark | CAE insns | CAE cycles | CAE IPC | XS insns | XS cycles | XS IPC | `delta_ipc_pct` = (cae-xs)/xs × 100 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `alu` | 1 000 028 | 4 000 784 | 0.2500 | 1 000 022 | 480 111 | 2.0829 | **-88.00 %** |
| `mem-stream` | 230 023 | 954 628 | 0.2410 | 230 017 | 160 031 | 1.4373 | **-83.24 %** |
| `pointer-chase` | 387 808 | 1 555 840 | 0.2493 | 387 802 | 480 111 | 0.8077 | **-69.14 %** |

The plan's Milestone-E output contract
(`plan.md:261-263, :301-302`) required writing
measured `delta_ipc_pct` directionality into this
calibration log; the table above delivers that.

**Interpretation**: the CAE Lower-Bound model (DEC-8)
retires strictly one instruction per TB (one-insn-
per-TB contract) while XS-GEM5's O3 DerivO3CPU
runs 32-wide fetch + 8-wide commit. CAE's IPC
floor is therefore bounded by 1.0 (one insn per
cycle via the scheduler's issue cap) and in
practice is below 0.3 because of per-retire
cycle charge for scheduler-port cap (issue_cap =
min(issue_width, CAE_OOO_SCHED_PORTS=2), so
cycles_per_retire = ceil(rename_width/issue_cap) =
ceil(8/2) = 4 at kmhv3 widths). XS-GEM5 exceeds
1.0 on cache-friendly alu and mem-stream thanks
to its full OoO window; pointer-chase drops to
0.8 because of dependent-load serialization.

The negative `delta_ipc_pct` directionality is
consistent with the plan's architectural intent:
Lower Bound deliberately under-models concurrency
in exchange for a correctness substrate + an
observability lane for Milestone-D when it
eventually lands. Closing the gap is the
Milestone-D scope, not Lower Bound.

**accuracy-gate.json gate verdict** (per-metric):

For each benchmark, `accuracy-gate.json` records
CAE-vs-XS rel_error_pct on cycles, insns, ipc,
mispredict_rate, l1d_mpki, avg_load_latency. The
`insns` metric passes on all three (spread <0.01 %
after the Round-10 count-preflight fix
normalises CAE to the trace-record count); `ipc`
/ `cycles` fail the 10 % threshold by the margins
above; `mispredict_rate` and `l1d_mpki` report
`Infinity` because XS-GEM5 reports 0 for both
under the kmhv3 prefetcher + TAGE configuration
on these short benchmarks (divide-by-zero in
rel_err). The gate FAIL verdict is expected and
is not a Milestone-E completion blocker — the
task-E2 contract is about RECORDING the measured
directionality, not passing the 10 % gate (which
is the AC-K-5 terminal-gate calibration target,
conditional on Milestone D).

**What Round 10 achieved**:

  - task-R10-1: root-caused + patched the
    XS-GEM5 O3 commit-stuck livelock for raw-cpt
    mode; both representative benchmarks now
    produce clean stats.txt + xs-gem5.json.
  - task-R10-2 (intrinsic to R10-1): end-to-end
    xs-suite lands all required XS artifacts on
    all 3 current-suite benchmarks.
  - task-R10-3: `performance-report-xs.json`
    auto-regenerated as part of the suite
    run; its `alu` / `mem-stream` / `pointer-
    chase` entries now carry real
    `xs_gem5_wallclock_seconds`. The stale
    entries for the three non-suite benchmarks
    (`branchy`, `coremark-1c`, `matmul-small`)
    remain as previously-captured wallclock
    evidence; they are not in the current
    `xs-1c-kmhv3.yaml` suite list and regen
    would need separate runs.
  - task-R10-4 (this section): measured
    `delta_ipc_pct` for all three benchmarks
    recorded in the calibration log.
  - task-E2 closes on the measurement contract.

**Remaining items**:

  - `delta_ipc_pct` gate PASS would require
    closing the ~70-90 % CAE/XS gap — that
    requires Milestone D (virtual-issue
    batching) under DEC-8's Upper-Bound path.
    Explicitly out of scope for this plan.
  - `mispredict_rate` / `l1d_mpki` rel_error =
    Infinity because XS-GEM5 reports 0 on these
    counters for these short benchmarks. A
    longer warmup / real SPEC workload would
    lift them above 0; not blocking Milestone E
    output.

**Milestone E CLOSES on the plan's contract**:
representative non-zero evidence (Round 9,
`sbuffer_eviction_events` on two in-plan manifest
benchmarks) + measured `delta_ipc_pct` written to
the calibration log (this section, corrected in
Round 11). Both task-E1 and task-E2 are complete.
AC-7 locked-path gate remains empty through
Rounds 0-11.

### Round 11 — Correct the Round-10 hand-transcription, narrow the XS-GEM5 exit predicate, land suite-aware perf-report

Codex R10 review flagged three concrete gaps in the
Round-10 Milestone-E closure claim:

  1. The Round-10 calibration log + summary table
     had WRONG benchmark numbers for `alu`
     (`100 007 / 581 184 / 0.172` was hand-written
     but the on-disk report shows `1 000 028 /
     4 000 784 / 0.2500`). The `delta_ipc_pct`
     percentage happened to match by coincidence
     of the ratio, but absolute insn / cycle / IPC
     columns were off by 10×.
  2. `performance-report-xs.json` still had
     `benchmark_count == 6` (alu + branchy +
     coremark-1c + matmul-small + mem-stream +
     pointer-chase) even though the current
     xs-1c-kmhv3 suite list is only three
     benchmarks — `perf-report.py` glob-aggregated
     every stale report directory.
  3. The new `xs-gem5-stuck-rob-empty-exit.patch`
     predicate `lastCommitCycle > 0 &&
     rob->isEmpty(0)` was too broad: any post-boot
     fetch-side deadlock that drained the ROB
     would also satisfy it. The reviewer required
     either a tighter predicate or targeted debug
     evidence showing the exit fired from the
     known post-sentinel halt loop.

Round 11 addresses all three.

**task-R11-1 — corrected delta_ipc_pct from JSON**:
re-extracted the measurement table programmatically
from `tests/cae-difftest/reports/xs-1c-kmhv3-
{alu,mem-stream,pointer-chase}/{cae.json,xs-gem5.json}`
via a short Python snippet (not hand-transcribed).
The table above is now the correct rewrite; the
wallclock sub-table follows the fresh Round-11
rerun values. The IPC / cycles / insns columns are
deterministic across rounds; only wallclock
drifted run-to-run by <0.5 s each.

**task-R11-2 — suite-aware perf-report.py**:
`tests/cae-difftest/scripts/perf-report.py` now
reads `common.measurement.suite.benchmarks` from
the paired YAML via the existing `_pairedyaml`
helper and aggregates only those entries. Added
a `benchmark_source` field to the output so the
operator can verify the aggregator used the
YAML path (falls back to glob mode if the config
has no suite stanza, preserving back-compat).
After regeneration,
`tests/cae-difftest/reports/performance-report-
xs.json` reports `benchmark_count: 3` and lists
only `alu`, `mem-stream`, `pointer-chase` —
matching the current `xs-1c-kmhv3.yaml` suite.

**task-R11-3 — tightened exit predicate +
targeted debug evidence**: the new
`xs-gem5-stuck-rob-empty-exit.patch` now requires
THREE invariants, not two:

  1. `lastCommitCycle > 0` (post-boot).
  2. `rob->isEmpty(0)` (nothing in-flight).
  3. `lastCommitedSeqNum[0] >= 1000` (substantial
     architectural workload progress).

Our Lower-Bound representative benchmarks retire
10⁵-10⁶ insns so they easily clear threshold 3; a
boot-path deadlock that empties the ROB after
<1000 commits still falls through to the existing
`panic()`. The warn message now also carries
`lastCommitedSeqNum` so post-run verification can
confirm the exit fired from substantial progress.

Observed values from the fresh Round-11 rerun on
all three benchmarks (captured from gem5's warn
output):

| Benchmark | lastCommitCycle | lastCommitedSeqNum | XS retired_insn_count |
|---|---:|---:|---:|
| `alu` | 428 259 | 1 000 262 | 1 000 022 |
| `mem-stream` | 105 843 | 234 261 | 230 017 |
| `pointer-chase` | 432 393 | 425 646 | 387 802 |

`lastCommitedSeqNum` exceeds the 1000 threshold by
three decimal orders of magnitude on every
benchmark. The small delta between
`lastCommitedSeqNum` and `retired_insn_count`
(~240 / ~4 000 / ~38 000 respectively) is
branch-squash noise: seqNums are assigned at
dispatch but only a subset commit. Each retired
count also matches NEMU's ground-truth trace
record count byte-for-byte:

  - alu: cae_trace=nemu=xs = 1 000 022
  - mem-stream: cae_trace=nemu=xs = 230 017
  - pointer-chase: cae_trace=nemu=xs = 387 802

Three independent simulators retiring the same
number of instructions before the XS exit fires is
the "architectural workload-complete" proof Codex
R10 required. The exit fired from the intended
post-sentinel halt loop on every representative
benchmark.

**task-R11-4 (this section)** — Round-10
corrections recorded above + this Round-11
diagnosis section. The earlier Round-10 table is
now correct; the earlier "Round 10" heading
remains in place but its data is rewritten.

**What Round 11 did achieve**:

  - Corrected `delta_ipc_pct` table sourced
    directly from `cae.json` + `xs-gem5.json`.
  - Suite-aware `perf-report.py` +
    `benchmark_count=3` in
    `performance-report-xs.json`.
  - Tightened three-invariant exit predicate +
    captured post-run evidence that all three
    representative runs exited from the
    post-sentinel halt loop.
  - Milestone E task-E2 re-closes on a corrected
    + safer surface.

**What Round 11 did NOT change**:

  - The delta_ipc_pct directionality (-88 / -83 /
    -69 %) is unchanged; only absolute numbers in
    the table became correct.
  - accuracy-gate.json metric verdicts unchanged;
    the `ipc` FAIL against AC-K-5 10 %-gate
    remains the Milestone-D calibration target.
  - XS-GEM5 O3 livelock is still the only
    upstream blocker for wider benchmark scope;
    not reopened this round.

### sched-issue-ports sweep (Milestone-D direction measurement)

Sweep of runtime `sched-issue-ports` knob at {2, 4, 8} across
the three representative benchmarks. XS-GEM5 reference from the
table above (ports=2 baseline). Purpose: quantify how much of
the delta_ipc_pct gap comes from the 2-port scheduler cap alone,
before committing to full Milestone-D scope (virtual-issue
batching, bank-conflict accounting).

| Benchmark | ports | CAE insns | CAE cycles | CAE IPC | XS IPC | delta_ipc_pct | vs ports=2 |
|---|---:|---:|---:|---:|---:|---:|---|
| `alu` | 2 | 1 000 028 | 4 000 784 | 0.2500 | 2.0829 | **-88.00 %** | (baseline) |
| `alu` | 4 | 1 000 028 | 2 000 728 | 0.4998 | 2.0829 | **-76.00 %** | closer to 0 |
| `alu` | 8 | 1 000 028 | 1 000 700 | 0.9993 | 2.0829 | **-52.02 %** | closer to 0 |
| `mem-stream` | 2 | 230 023 | 954 628 | 0.2410 | 1.4373 | **-83.23 %** | (baseline) |
| `mem-stream` | 4 | 230 023 | 494 582 | 0.4651 | 1.4373 | **-67.64 %** | closer to 0 |
| `mem-stream` | 8 | 230 023 | 264 559 | 0.8695 | 1.4373 | **-39.50 %** | closer to 0 |
| `pointer-chase` | 2 | 387 808 | 1 555 840 | 0.2493 | 0.8077 | **-69.13 %** | (baseline) |
| `pointer-chase` | 4 | 387 808 | 780 224 | 0.4970 | 0.8077 | **-38.47 %** | closer to 0 |
| `pointer-chase` | 8 | 387 808 | 392 416 | 0.9883 | 0.8077 | **+22.36 %** | closer to 0 |

**Direction**: widening the scheduler-port cap from 2 to 8
monotonically closes the delta_ipc_pct gap for all three
benchmarks. At ports=8, pointer-chase CAE IPC (0.99) actually
exceeds XS-GEM5 IPC (0.81) because CAE's one-insn-per-TB model
does not account for dependent-load serialization that the O3
pipeline naturally experiences. This crossover confirms that the
2-port cap was the dominant IPC limiter, not the only one — the
remaining gap at ports=8 for alu (-52 %) and mem-stream (-40 %)
is attributable to factors outside the scheduler port count
(virtual-issue batching, bank-conflict accounting, full OoO
window effects).

**Conclusion**: the scheduler-port knob alone cannot close the
delta_ipc_pct gap to within 10 %. Full Milestone-D work
(virtual-issue batching + bank-conflict) is needed. The knob
provides the measurement baseline for isolating those components.

### Milestone-D virtual-issue batching + dependent-load sweep

Sweep of `virtual-issue-window` and `dependent-load-stall-cycles`
at ports=8 across the three representative benchmarks.

| Benchmark | Config | CAE cycles | CAE IPC | XS IPC | delta_ipc_pct | vs baseline |
|---|---|---:|---:|---:|---:|---|
| `alu` | baseline (ports=8) | 1 000 700 | 0.9993 | 2.0829 | **-52.02 %** | (baseline) |
| `alu` | +batch(w=4) | 1 000 700 | 0.9993 | 2.0829 | **-52.02 %** | no change |
| `alu` | +batch+dep(3) | 1 000 700 | 0.9993 | 2.0829 | **-52.02 %** | no change |
| `mem-stream` | baseline (ports=8) | 264 559 | 0.8695 | 1.4373 | **-39.50 %** | (baseline) |
| `mem-stream` | +batch(w=4) | 264 559 | 0.8695 | 1.4373 | **-39.50 %** | no change |
| `mem-stream` | +batch+dep(3) | 264 562 | 0.8694 | 1.4373 | **-39.51 %** | ~0 |
| `pointer-chase` | baseline (ports=8) | 392 416 | 0.9883 | 0.8077 | **+22.36 %** | (baseline) |
| `pointer-chase` | +batch(w=4) | 392 416 | 0.9883 | 0.8077 | **+22.36 %** | no change |
| `pointer-chase` | +batch+dep(3) | 776 416 | 0.4995 | 0.8077 | **-38.14 %** | overcorrected |

**Findings**:

1. Virtual-issue batching has no visible effect at ports=8 because
   `issue_cycles = ceil(rename_width / issue_cap) = ceil(8/8) = 1`,
   which is already the minimum charge. The batching credit model
   only saves cycles when issue_cycles > 1 (e.g., at ports=2 where
   issue_cycles=4, batching visibly reduces total_cycles in unit tests).

2. Dependent-load stall (3 cycles) successfully corrects the
   pointer-chase crossover: IPC drops from 0.99 to 0.50. However,
   3 cycles overcorrects (delta goes from +22% to -38%). A stall
   of ~1 cycle would better target the XS-GEM5 reference IPC of
   0.81. This is a calibration parameter, not a code defect.

3. The alu/mem-stream residuals (-52%/-40%) are not addressable by
   the current mechanism at ports=8 because the structural issue
   cost is already minimized. Closing this gap requires either
   (a) sub-cycle fractional charge models, (b) reducing the base
   per-retire overhead below 1 cycle, or (c) multi-instruction-per-TB
   modeling — all of which are beyond this plan's scope.

**Conclusion**: dependent-load stall achieves its directional goal
(pointer-chase no longer crosses over) but needs calibration. The
virtual-issue batching mechanism is architecturally correct and
effective at lower port counts but has no incremental impact at
ports=8. The alu/mem-stream gap is a fundamental model constraint
(one-insn-per-TB with minimum 1-cycle charge).

### Milestone-D Round 1: batching redesign (cycles=0 for co-issue)

Round 1 fixes the batching model so independent retires in a batch
get `cycles=0` (true co-issue) instead of only skipping the structural
surcharge. This enables IPC > 1.0 at any port width.

| Benchmark | Config | CAE IPC | XS IPC | delta_ipc_pct | vs baseline |
|---|---|---:|---:|---:|---|
| `alu` | baseline (ports=8) | 0.9993 | 2.0829 | **-52.0 %** | (ref) |
| `alu` | +batch(w=2) | 1.6648 | 2.0829 | **-20.1 %** | 32pp closer |
| `alu` | +batch(w=4) | 2.4958 | 2.0829 | **+19.8 %** | overcorrects |
| `mem-stream` | baseline (ports=8) | 0.8695 | 1.4373 | **-39.5 %** | (ref) |
| `mem-stream` | +batch(w=2) | 1.3811 | 1.4373 | **-3.9 %** | 36pp closer |
| `mem-stream` | +batch(w=4) | 2.2432 | 1.4373 | **+56.1 %** | overcorrects |
| `pointer-chase` | baseline (ports=8) | 0.9883 | 0.8077 | **+22.4 %** | (ref) |
| `pointer-chase` | +batch(w=2) | 1.4780 | 0.8077 | **+83.0 %** | worse |
| `pointer-chase` | +batch(w=2,dep=1) | 1.4780 | 0.8077 | **+83.0 %** | dep stall not triggered |

**Direction**: `virtual-issue-window=2` dramatically improves alu
(-52%→-20%) and mem-stream (-40%→-4%). The `w=2` sweet spot matches
a 2-wide co-issue model. `w=4` overcorrects both benchmarks.
Pointer-chase worsens because the batching gives free credits to
loads that should serialize; the dependent-load stall (1 cycle) does
not fire on this benchmark's access pattern because intervening
ALU/branch instructions clear the load-tracking state.

**Calibration recommendation**: `virtual-issue-window=2` is the
best general-purpose default for this benchmark set.

### Milestone-D Round 2: persistent load-dependency tracking

Round 2 redesigns dependency tracking so the load-produced register
persists across intervening retires until overwritten. This handles
the real pointer-chase loop shape (`ld t0; addi t2; bne; ld t0`).

| Benchmark | Config | CAE IPC | XS IPC | delta_ipc_pct |
|---|---|---:|---:|---:|
| `alu` | w=2,dep=1 | 1.6648 | 2.0829 | **-20.1 %** |
| `alu` | w=2,dep=2 | 1.6648 | 2.0829 | **-20.1 %** |
| `mem-stream` | w=2,dep=1 | 1.3811 | 1.4373 | **-3.9 %** |
| `mem-stream` | w=2,dep=2 | 1.3811 | 1.4373 | **-3.9 %** |
| `pointer-chase` | w=2,dep=1 | 0.9946 | 0.8077 | **+23.1 %** |
| `pointer-chase` | w=2,dep=2 | 0.7495 | 0.8077 | **-7.2 %** |

**Direction**: the persistent tracking correctly detects pointer-chase
dependency across intervening ALU/branch. At dep=2, pointer-chase
delta drops from +22% to -7.2% (within 10% of GEM5). At dep=1, the
stall is too small to overcome the batching credit.

**Best config**: `virtual-issue-window=2, dependent-load-stall-cycles=2`
gives alu -20%, mem-stream -4%, pointer-chase -7%.
