#!/usr/bin/env bash
#
# KMH-V3 track suite driver.
#
# Orchestrates a per-benchmark sequence for the xs-1c-* configs:
#
#   1. NEMU functional-reference run  (run-nemu-ref.sh)
#   2. CAE run                        (run-cae.py)
#   3. nemu-difftest byte-compare     (nemu-difftest.py)   <- binding
#   4. XS-GEM5 KMH-V3 timing run      (run-xs-gem5.py)     skipped on #3 fail
#   5. diff.py timing comparison      (diff.py)            skipped on #3 fail
#   6. ci-gate.py --stage diff_threshold                   skipped on #3 fail
#
# Steps 4-6 are ONLY invoked when step 3 exits 0 (AC-K-2: functional
# difftest is binding before any timing gate). After every benchmark
# the driver records per-bench wallclock + functional-verdict +
# timing-verdict into $REPORTS_DIR/<config>-suite.log. If any
# benchmark's functional diff failed, the suite driver exits non-
# zero without invoking ci-gate.py --stage suite.
#
# Usage:
#   run-xs-suite.sh <config> [--tier 1|2] [--benchmark <name>]
#                             [--full-trace] [--skip-trace-prereq]
#
# Arguments:
#   <config>              paired YAML name (e.g. xs-1c-functional)
#   --tier                1 (default, in-tree micros) or 2 (host-provided
#                         binaries under $CAE_TIER2_BINARIES_DIR)
#   --benchmark           restrict the run to one benchmark name (diagnosis)
#   --full-trace          tier-2 only: force trace mode instead of checkpoint
#   --skip-trace-prereq   (diagnosis only) tolerate a missing
#                         cae-itrace.bin and continue to the next
#                         benchmark without declaring functional
#                         failure. Defaults to off — AC-K-2 requires
#                         fail-closed behaviour in CI.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../../../../.." && pwd)"
: "${CAE_BUILD_DIR:=$repo_root/build}"

config=""
tier=1
only_bench=""
full_trace=0
skip_trace_prereq=0

if [[ $# -lt 1 ]]; then
    echo "Usage: run-xs-suite.sh <config> [--tier 1|2] " \
         "[--benchmark <name>] [--full-trace]" >&2
    exit 2
fi
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
fi
config="$1"; shift
while (( $# )); do
    case "$1" in
        --tier)               tier="$2"; shift 2 ;;
        --benchmark)          only_bench="$2"; shift 2 ;;
        --full-trace)         full_trace=1; shift ;;
        --skip-trace-prereq)  skip_trace_prereq=1; shift ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "run-xs-suite: unknown flag '$1'" >&2
            exit 2
            ;;
    esac
done

if [[ "$tier" != "1" && "$tier" != "2" ]]; then
    echo "run-xs-suite: --tier must be 1 or 2" >&2
    exit 2
fi

: "${CAE_TIER2_BINARIES_DIR:=$HOME/cae-ready-to-run}"

# Resolve the benchmark list from the config's suite.benchmarks,
# filtered by tier. Tier-2 requires CAE_TIER2_BINARIES_DIR to be
# present; tier-1 does not (AC-K-1 negative test).
if [[ "$tier" == "2" ]]; then
    if [[ ! -d "$CAE_TIER2_BINARIES_DIR" ]]; then
        echo "run-xs-suite: \$CAE_TIER2_BINARIES_DIR not a directory: " \
             "$CAE_TIER2_BINARIES_DIR" >&2
        exit 1
    fi
fi

bench_list="$(
    cd "$repo_root" && \
    PYTHONPATH="$here:${PYTHONPATH:-}" python3 - \
        "$config" "$tier" "$only_bench" <<'PY'
import sys
import _pairedyaml as p

cfg_name = sys.argv[1]
tier = int(sys.argv[2])
only = sys.argv[3]
cfg = p.load_config(cfg_name)
suite = (cfg.measurement.get("suite") or {})
benches = suite.get("benchmarks", [])
manifest = p.load_manifest()["benchmarks"]
out = []
for b in benches:
    entry = manifest.get(b)
    if entry is None:
        print(f"ERR: unknown benchmark {b!r}", file=sys.stderr)
        sys.exit(1)
    if int(entry.get("tier", 1)) != tier:
        continue
    if not entry.get("gated", True):
        continue
    if only and b != only:
        continue
    out.append(b)
print(" ".join(out))
PY
)" || exit 1

if [[ -z "$bench_list" ]]; then
    echo "run-xs-suite: no gated benchmarks at tier $tier in " \
         "config $config" >&2
    exit 1
fi

reports_dir="$CAE_BUILD_DIR/tests/cae/difftest/reports"
suite_log="$reports_dir/${config}-suite.log"
mkdir -p "$reports_dir"
: > "$suite_log"

functional_failed=0
timing_failed=0

mode="trace"
if [[ "$tier" == "2" ]]; then
    if (( full_trace )); then
        mode="trace"
    else
        mode="checkpoint"
    fi
fi

for b in $bench_list; do
    bench_report_dir="$reports_dir/$config-$b"
    mkdir -p "$bench_report_dir"
    echo "=== $config :: $b ===" | tee -a "$suite_log"

    # 0. First-PC preflight (AC-K-10). For round 3 this is
    # manifest-driven: read reset_pc from the benchmark's MANIFEST
    # entry and emit it into the suite log so per-benchmark runs are
    # auditable. A missing or zero reset_pc aborts the benchmark
    # before expensive NEMU/CAE/XS-GEM5 runs fire — catching the
    # AC-K-2 reset-PC mismatch bug class that motivated BS-3.
    reset_pc="$(
        cd "$repo_root" && PYTHONPATH="$here:${PYTHONPATH:-}" python3 - \
            "$b" <<'PY'
import sys
import _pairedyaml as p
b = sys.argv[1]
m = p.load_manifest()["benchmarks"].get(b)
if m is None:
    print("")
    sys.exit(0)
pc = m.get("reset_pc")
print(pc if pc is not None else "")
PY
    )"
    if [[ -z "$reset_pc" ]]; then
        echo "  PREFLIGHT-FAIL: manifest has no reset_pc for $b " \
             "(AC-K-10 requires an anchor before timing)" \
             | tee -a "$suite_log"
        functional_failed=1
        continue
    fi
    echo "  preflight: reset_pc=$reset_pc" | tee -a "$suite_log"

    # 1. NEMU functional reference (binding).
    if ! "$here/run-nemu-ref.sh" \
            --benchmark "$b" \
            --report-dir "$bench_report_dir" \
            --mode "$mode"; then
        echo "  FUNCTIONAL-PRODUCE-FAIL: nemu-ref run failed" \
             | tee -a "$suite_log"
        functional_failed=1
        continue
    fi

    # 2. CAE run. run-cae.py takes the config NAME as a positional
    # argument (NOT --config — prior round shipped with the wrong
    # call form). Mode selection (round 6):
    #   mode=trace     -> --trace-out <rpt>/cae-itrace.bin
    #   mode=checkpoint -> --checkpoint-out <rpt>/cae-checkpoints.bin
    # The presence check, nemu-difftest invocation, and count
    # logic below all key off the mode-selected artifact name.
    if [[ "$mode" == "trace" ]]; then
        cae_functional_arg=(--trace-out
                            "$bench_report_dir/cae-itrace.bin")
        cae_functional_file="$bench_report_dir/cae-itrace.bin"
        nemu_functional_file="$bench_report_dir/nemu-itrace.bin"
    else
        cae_functional_arg=(--checkpoint-out
                            "$bench_report_dir/cae-checkpoints.bin")
        cae_functional_file="$bench_report_dir/cae-checkpoints.bin"
        nemu_functional_file="$bench_report_dir/nemu-checkpoints.bin"
    fi
    if ! python3 "$here/run-cae.py" \
            "$config" --benchmark "$b" \
            --output "$bench_report_dir/cae.json" \
            "${cae_functional_arg[@]}"; then
        echo "  CAE-RUN-FAIL: run-cae.py exited non-zero " \
             "(see $bench_report_dir for logs)" \
             | tee -a "$suite_log"
        functional_failed=1
        continue
    fi

    # 2.1. nemu-difftest byte-compare. Binding: skip timing gate on
    # failure. AC-K-2 requires fail-closed when the functional
    # producer is wired. --skip-trace-prereq keeps the lenient
    # round-2 behaviour behind an explicit opt-in for debugging
    # only. Artifact names are mode-resolved (trace vs
    # checkpoint).
    if [[ ! -f "$cae_functional_file" ]]; then
        if (( skip_trace_prereq )); then
            echo "  FUNCTIONAL-SKIP: $(basename "$cae_functional_file") " \
                 "not produced; --skip-trace-prereq set, continuing " \
                 "without diff" \
                 | tee -a "$suite_log"
            continue
        fi
        echo "  FUNCTIONAL-FAIL: $(basename "$cae_functional_file") " \
             "not produced by CAE (mode=$mode). Pass " \
             "--skip-trace-prereq to tolerate during diagnosis." \
             | tee -a "$suite_log"
        functional_failed=1
        continue
    fi

    # 2.2. AC-K-10 first-PC cross-check (REAL). The manifest reset_pc
    # is the contractual anchor. We cross-compare it against CAE's
    # /first-pc/ QOM latch (surfaced in cae.json as `first_pc`),
    # CAE's itrace first record pc, and NEMU's itrace first record
    # pc. Any DIVERGE among the known values fails the benchmark
    # before nemu-difftest.py fires; all-SKIP is tolerated so a
    # partial run (missing NEMU-side artifact, for example) still
    # reports the concrete divergence rather than masking as I/O.
    pc_verdict="$(
        cd "$repo_root" && python3 - \
            "$reset_pc" \
            "$bench_report_dir/cae.json" \
            "$cae_functional_file" \
            "$nemu_functional_file" \
            "$mode" \
            <<'PY'
import json
import struct
import sys

(manifest_pc_raw, cae_json, cae_itrace,
 nemu_itrace, run_mode) = sys.argv[1:6]

def as_u64(x):
    if x is None:
        return None
    if isinstance(x, int):
        return x & 0xffffffffffffffff
    s = str(x).strip()
    if not s:
        return None
    base = 16 if s.lower().startswith("0x") else 0
    try:
        return int(s, base) & 0xffffffffffffffff
    except ValueError:
        return None

def first_record_pc(path, mode):
    """
    BS-16/BS-19: first-record pc reader for TRACE mode only.

    In trace mode (40-byte record) the first record IS the boot
    retire (pc at offset 0), so it's a valid boot-PC source.

    In checkpoint mode (608-byte record) the first record is
    emitted at retire 1,000,000 — it is NOT boot PC. Comparing
    it against MANIFEST.reset_pc would diverge spuriously on
    every tier-2 run. Return None so the preflight treats the
    column as SKIP.
    """
    if mode != "trace":
        return None
    try:
        with open(path, "rb") as f:
            header = f.read(16)
            if len(header) < 16:
                return None
            record = f.read(40)
            if len(record) < 40:
                return None
    except OSError:
        return None
    return struct.unpack_from("<Q", record, 0)[0]

def fmt(v):
    return "None" if v is None else f"0x{v:x}"

manifest_pc = as_u64(manifest_pc_raw)
try:
    with open(cae_json) as f:
        cae_pc = as_u64(json.load(f).get("first_pc"))
except (OSError, ValueError):
    cae_pc = None

cae_trace_pc = first_record_pc(cae_itrace, run_mode)
nemu_trace_pc = first_record_pc(nemu_itrace, run_mode)

def tag(v, mode_is_cp):
    """Report SKIP explicitly in checkpoint mode so the log is
    self-documenting about which columns are meaningful."""
    if mode_is_cp and v is None:
        return "SKIP (checkpoint mode — record 0 is retire 1M, " \
               "not boot)"
    return fmt(v)

mode_is_cp = (run_mode == "checkpoint")
print(f"manifest={fmt(manifest_pc)} cae_qmp={fmt(cae_pc)} "
      f"cae_trace={tag(cae_trace_pc, mode_is_cp)} "
      f"nemu_trace={tag(nemu_trace_pc, mode_is_cp)} "
      f"mode={run_mode}")

vals = [v for v in (manifest_pc, cae_pc, cae_trace_pc, nemu_trace_pc)
        if v is not None and v != 0]
if not vals:
    print("VERDICT: SKIP")
    sys.exit(2)
if any(v != vals[0] for v in vals):
    print(f"VERDICT: DIVERGE (reference={fmt(vals[0])})")
    sys.exit(1)
print("VERDICT: MATCH")
sys.exit(0)
PY
    )"
    pc_rc=$?
    echo "  preflight-pc: $pc_verdict" | tee -a "$suite_log"
    if (( pc_rc == 1 )); then
        echo "  PREFLIGHT-PC-DIVERGE: first PCs disagree across " \
             "manifest / CAE / NEMU; refusing to diff" \
             | tee -a "$suite_log"
        functional_failed=1
        continue
    fi

    if ! python3 "$here/nemu-difftest.py" \
            --cae "$cae_functional_file" \
            --nemu "$nemu_functional_file" \
            --mode "$mode"; then
        echo "  FUNCTIONAL-FAIL: nemu-difftest.py exited non-zero" \
             | tee -a "$suite_log"
        functional_failed=1
        # AC-K-2: no timing invocation when functional diff failed.
        continue
    fi

    # 3. XS-GEM5 timing run. Pass the same --benchmark override the
    # CAE side received so both backends resolve their respective
    # artifacts for the current suite entry — without the override,
    # run-xs-gem5.py would resolve `common.benchmark.name` for every
    # iteration and diff.py would trip its benchmark-mismatch guard
    # on anything after the config default.
    # BS-26 (round 10) AC-K-10: always request the XS-GEM5
    # runtime first-PC via --first-pc-debug. The flag appends
    # gem5's Exec debug to a tiny log (`--debug-end=10` ticks,
    # ~a few KB), and run-xs-gem5.py parses the first committed
    # PC out of it. Round 9 left this opt-in, so the default
    # suite path kept seeing first_pc=null and SKIPping the XS
    # column of the 4-way preflight. Enabling by default makes
    # AC-K-10 a real 4-way comparison on every suite run.
    if ! python3 "$here/run-xs-gem5.py" \
            --config "$config" \
            --benchmark "$b" \
            --output "$bench_report_dir/xs-gem5.json" \
            --outdir "$bench_report_dir/m5out" \
            --first-pc-debug; then
        echo "  TIMING-XS-GEM5-FAIL" | tee -a "$suite_log"
        timing_failed=1
        continue
    fi

    # 3.1. Cross-check that CAE and XS reports agree on the
    # benchmark name before invoking diff.py. If a caller (or a
    # future refactor) drops the --benchmark override on either
    # side, this guard fires before diff.py emits a misleading gate
    # verdict. Matches the "pre-flight boots both sides and asserts
    # same first PC" contract in AC-K-10 at the per-benchmark-
    # identity granularity.
    if ! python3 - "$bench_report_dir/cae.json" \
                   "$bench_report_dir/xs-gem5.json" "$b" <<'PY'
import json
import sys
cae_path, xs_path, expected = sys.argv[1], sys.argv[2], sys.argv[3]
with open(cae_path) as f:
    cae = json.load(f)
with open(xs_path) as f:
    xs = json.load(f)
cae_b = cae.get("benchmark")
xs_b = xs.get("benchmark")
if cae_b != expected or xs_b != expected:
    print(f"benchmark-mismatch: expected={expected!r} "
          f"cae={cae_b!r} xs-gem5={xs_b!r}", file=sys.stderr)
    sys.exit(1)
PY
    then
        echo "  TIMING-BENCH-MISMATCH: refusing to diff" \
             | tee -a "$suite_log"
        timing_failed=1
        continue
    fi

    # 3.2. AC-K-10 full 4-way first-PC preflight. Rerun the
    # comparison now that xs-gem5.json is on disk. The earlier
    # 3-way preflight at step 2.2 ran before XS-GEM5 could be
    # consulted; this step extends it with the XS-GEM5 first_pc
    # column. Any DIVERGE fails the benchmark with a concrete
    # four-column diagnostic before diff.py is invoked.
    # Round 11 extracts the 4-way preflight into a standalone
    # script so tests/cae/run-cae-test.sh can exercise the same
    # authoritative path with synthetic fixtures. The block
    # previously embedded here moved verbatim to
    # scripts/preflight-first-pc.py (exit-code semantics are the
    # same: 0 MATCH / 1 DIVERGE / 2 SKIP).
    #
    # Round 12 BS-29: under `set -euo pipefail`, the previous
    # `pc_verdict_xs="$(...)"; pc_rc_xs=$?` pattern exited the
    # shell on any Python `sys.exit(1)` before the rc could be
    # read. The fail-closed DIVERGE verdict therefore killed the
    # whole suite instead of setting `timing_failed=1` and
    # continuing to the next benchmark. The `if out="$(...)";
    # then rc=0; else rc=$?; fi` idiom keeps the shell alive
    # (see .humanize/bitlesson.md
    # BL-20260418-bash-set-e-cmd-substitution-exit).
    if pc_verdict_xs="$(cd "$repo_root" && \
            python3 "$here/preflight-first-pc.py" \
            "$reset_pc" \
            "$bench_report_dir/cae.json" \
            "$cae_functional_file" \
            "$nemu_functional_file" \
            "$bench_report_dir/xs-gem5.json" \
            "$mode")"; then
        pc_rc_xs=0
    else
        pc_rc_xs=$?
    fi
    echo "  preflight-pc (4-way): $pc_verdict_xs" | tee -a "$suite_log"
    if (( pc_rc_xs == 1 )); then
        echo "  PREFLIGHT-PC-DIVERGE (4-way): first PCs disagree; " \
             "refusing to diff" | tee -a "$suite_log"
        timing_failed=1
        continue
    fi

    # 3.3. AC-K-3.1 retired-insn-count alignment. CAE, NEMU, and
    # XS-GEM5 must agree within ±1 retired instruction on how many
    # instructions actually retired for this benchmark. A larger
    # drift usually means the sentinel fired on a different
    # architectural moment on each side — diffing IPCs on top of
    # that would be noise.
    #
    # Round 8 BS-20: source the NEMU-side count from
    # nemu-itrace-stats.json's `retired_insn_count` field, which
    # run-nemu-ref.sh parses from the patch's
    # `NEMU-ITRACE-FINAL-COUNT:` stderr line. This is the exact
    # counter, not the file-size inference that loses up to one
    # checkpoint interval of precision.
    nemu_stats_json="$bench_report_dir/nemu-itrace-stats.json"
    #
    # Round 12 BS-29 part B: the retired-count gate was embedded
    # as a heredoc Python block here. It is now extracted to
    # scripts/preflight-retired-count.py (same exit-code
    # semantics: 0 MATCH / 1 DIVERGE / 2 SKIP). The extraction
    # enables harness coverage from tests/cae/run-cae-test.sh and
    # makes the call site use the same `if ...; then rc=0; else
    # rc=$?; fi` idiom as the first-PC preflight above. Without
    # this round-12 fix, `set -euo pipefail` killed the shell on
    # a DIVERGE before `count_rc=$?` could be read.
    #
    if count_verdict="$(cd "$repo_root" && \
            python3 "$here/preflight-retired-count.py" \
            "$bench_report_dir/cae.json" \
            "$nemu_stats_json" \
            "$nemu_functional_file" \
            "$bench_report_dir/xs-gem5.json" \
            "$mode")"; then
        count_rc=0
    else
        count_rc=$?
    fi
    echo "  preflight-count: $count_verdict" | tee -a "$suite_log"
    if (( count_rc == 1 )); then
        echo "  COUNT-DIVERGE: retired_insn_count spread > 1; " \
             "AC-K-3.1 fails; refusing to diff" | tee -a "$suite_log"
        timing_failed=1
        continue
    fi

    # 4. diff.py timing comparison (cae vs xs-gem5).
    if ! python3 "$here/diff.py" \
            --left "$bench_report_dir/cae.json" \
            --right "$bench_report_dir/xs-gem5.json" \
            --config "$config" \
            --report-dir "$bench_report_dir"; then
        echo "  TIMING-DIFF-FAIL" | tee -a "$suite_log"
        timing_failed=1
        continue
    fi

    # 5. ci-gate single-benchmark threshold.
    if ! python3 "$here/ci-gate.py" --stage diff_threshold \
            --config "$config" --benchmark "$b"; then
        echo "  TIMING-GATE-FAIL" | tee -a "$suite_log"
        timing_failed=1
        continue
    fi

    echo "  PASS" | tee -a "$suite_log"
done

echo "=== suite summary ==="         | tee -a "$suite_log"
echo "  config        : $config"      | tee -a "$suite_log"
echo "  tier          : $tier"        | tee -a "$suite_log"
echo "  functional_failed=$functional_failed" | tee -a "$suite_log"
echo "  timing_failed=$timing_failed"         | tee -a "$suite_log"

if (( functional_failed )); then
    echo "run-xs-suite: functional-gate failure(s) present; refusing " \
         "to invoke ci-gate.py --stage suite (AC-K-2)" >&2
    exit 1
fi
if (( timing_failed )); then
    echo "run-xs-suite: per-benchmark timing failure(s) present; " \
         "suite-stage gate will reject" >&2
    # Fall through to the suite gate so its log is produced, then
    # exit non-zero.
fi

# Emit performance-report-xs.json regardless of the suite gate
# outcome (AC-K-7: report-only until M6' lands). Individual per-
# benchmark wallclocks are populated by run-cae.py, run-xs-gem5.py,
# and run-nemu-ref.sh; missing entries show up as null and do not
# fail the aggregator.
python3 "$here/perf-report.py" \
    --config "$config" \
    --reports-dir "$reports_dir" \
    --output "$reports_dir/performance-report-xs.json" \
    || echo "run-xs-suite: perf-report aggregation warning (non-fatal)" \
         >&2

python3 "$here/ci-gate.py" --stage suite --config "$config"
rc=$?
if (( timing_failed )); then
    # A per-benchmark failure already flagged; mirror the suite
    # gate's rc but keep the diagnostic message.
    exit "$rc"
fi
exit "$rc"
