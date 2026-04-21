#!/usr/bin/env bash
#
# Run the CAE-side reference NEMU interpreter with binary itrace
# output, against a pinned ~/NEMU checkout.
#
# Wraps <repo>/build/nemu-ref/build/riscv64-nemu-interpreter (produced
# by build-nemu-ref.sh) with the right --itrace-out / --itrace-mode
# flags and a pre-flight pin check.
#
# Usage:
#   run-nemu-ref.sh --benchmark <name> --report-dir <path>
#                   [--mode trace|checkpoint]
#                   [--xs-binary <path>]
#
# Arguments:
#   --benchmark    tier-1 or tier-2 benchmark name (must exist in
#                  MANIFEST.json); resolves to benchmarks/build/
#                  <bench>.xs.bin for tier-1 or
#                  $CAE_TIER2_BINARIES_DIR/<tier2_relpath> for tier-2.
#   --report-dir   directory to write nemu-itrace.bin and
#                  nemu-itrace-stats.json (relative to repo root if
#                  not absolute).
#   --mode         'trace' (default) or 'checkpoint' (tier-2 picks up
#                  checkpoint automatically in run-xs-suite.sh).
#   --xs-binary    override the MANIFEST-resolved binary path (rare;
#                  useful for ad-hoc debugging).

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../../.." && pwd)"
pin_check="$repo_root/tools/cae/check-nemu-version.sh"

: "${NEMU_HOME:=$HOME/NEMU}"
: "${CAE_NEMU_BUILD_DIR:=$repo_root/build/nemu-ref}"
: "${CAE_TIER2_BINARIES_DIR:=$HOME/cae-ready-to-run}"

benchmark=""
report_dir=""
mode="trace"
xs_binary=""

while (( $# )); do
    case "$1" in
        --benchmark)  benchmark="$2"; shift 2 ;;
        --report-dir) report_dir="$2"; shift 2 ;;
        --mode)       mode="$2"; shift 2 ;;
        --xs-binary)  xs_binary="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "run-nemu-ref: unknown flag '$1'" >&2
            exit 2
            ;;
    esac
done

if [[ -z "$benchmark" || -z "$report_dir" ]]; then
    echo "run-nemu-ref: --benchmark and --report-dir are required" >&2
    exit 2
fi
case "$mode" in
    trace|checkpoint) ;;
    *)
        echo "run-nemu-ref: --mode must be 'trace' or 'checkpoint'" >&2
        exit 2
        ;;
esac

"$pin_check" "$NEMU_HOME"

interpreter="$CAE_NEMU_BUILD_DIR/build/riscv64-nemu-interpreter"
if [[ ! -x "$interpreter" ]]; then
    echo "run-nemu-ref: interpreter missing: $interpreter" >&2
    echo "run-nemu-ref: run scripts/build-nemu-ref.sh first" >&2
    exit 1
fi

if [[ ! "$report_dir" = /* ]]; then
    report_dir="$repo_root/$report_dir"
fi
mkdir -p "$report_dir"

# Resolve the binary to run. Honor an explicit --xs-binary override;
# otherwise look up the benchmark in MANIFEST.json. Prefer the `xs`
# variant for tier-1 (raw binary loaded at 0x80000000) and the
# tier2_binary_relpath under CAE_TIER2_BINARIES_DIR for tier-2.
#
# `$repo_root` is derived from $BASH_SOURCE[0] above and passed
# directly into the Python heredoc as argv[2], so the resolver does
# NOT depend on any environment round-trip. (The prior version
# exported SCRIPT_HERE AFTER the heredoc, which made the resolver
# fall back to Path(".") and crash on repo_root.parents[2].)
if [[ -z "$xs_binary" ]]; then
    xs_binary="$(python3 - "$benchmark" "$repo_root" <<'PY'
import json
import os
import sys
from pathlib import Path

name = sys.argv[1]
repo = Path(sys.argv[2])

with (repo / "tests" / "cae" / "riscv64" / "difftest" / "benchmarks" /
      "MANIFEST.json").open() as f:
    mani = json.load(f)
entry = mani["benchmarks"].get(name)
if entry is None:
    print(f"ERR: unknown benchmark {name!r}", file=sys.stderr)
    sys.exit(1)
tier = entry.get("tier", 1)
if tier == 1:
    xs = entry.get("variants", {}).get("xs")
    if xs is None:
        print(f"ERR: tier-1 {name} has no 'xs' variant; "
              f"run make xs-bins", file=sys.stderr)
        sys.exit(1)
    p = repo / "tests" / "cae" / "riscv64" / "difftest" / "benchmarks" / xs["binary"]
    print(p)
else:
    rel = entry.get("tier2_binary_relpath")
    if rel is None:
        print(f"ERR: tier-2 {name} missing tier2_binary_relpath",
              file=sys.stderr)
        sys.exit(1)
    staged = Path(os.environ["CAE_TIER2_BINARIES_DIR"]) / rel
    if not staged.is_file():
        print(f"ERR: tier-2 binary not staged: {staged}",
              file=sys.stderr)
        sys.exit(1)
    print(staged)
PY
    )" || {
        echo "run-nemu-ref: binary resolution failed for $benchmark" >&2
        exit 1
    }
fi

# BS-15 round 7: mode-aware artifact name. --mode trace writes
# nemu-itrace.bin (40-byte per-retire records); --mode checkpoint
# writes nemu-checkpoints.bin (608-byte per-1M-retire snapshots).
# The suite preflight + difftest + count gate all key off the
# mode-resolved name, so a checkpoint run with the old
# nemu-itrace.bin name would mis-pair with CAE's
# cae-checkpoints.bin.
if [[ "$mode" == "checkpoint" ]]; then
    itrace_out="$report_dir/nemu-checkpoints.bin"
else
    itrace_out="$report_dir/nemu-itrace.bin"
fi
stats_json="$report_dir/nemu-itrace-stats.json"

t0="$(date +%s.%N)"
set +e
# BS-20 AC-K-3.1: capture stderr so we can parse the
# `NEMU-ITRACE-FINAL-COUNT: N` line the patch's flush path
# emits at interpreter exit. That value is the NEMU side's
# exact retired-insn count — the suite's count gate reads it
# from the stats JSON instead of inferring from checkpoint
# file size (which loses up to one interval of precision).
nemu_stderr_log="$report_dir/nemu-stderr.log"
# Round 45 BS: without --batch (-b), NEMU enters its
# interactive `(nemu)` prompt on start, reads EOF from the
# (non-tty) stdin and immediately exits with state=NEMU_STOP,
# producing only a 16-byte header in itrace_out and no retired
# records. The binding functional diff then fails because the
# CAE retired trace has hundreds of thousands of records while
# the NEMU one is empty. `-b` makes the interpreter run the
# loaded image to completion straight from boot.
"$interpreter" -b \
    --itrace-out="$itrace_out" \
    --itrace-mode="$mode" \
    "$xs_binary" 2> >(tee "$nemu_stderr_log" >&2)
rc=$?
set -e
t1="$(date +%s.%N)"
wallclock="$(awk -v a="$t0" -v b="$t1" 'BEGIN{print b-a}')"

final_count="$(grep -oE 'NEMU-ITRACE-FINAL-COUNT: [0-9]+' \
    "$nemu_stderr_log" 2>/dev/null | tail -1 | awk '{print $2}')"
: "${final_count:=0}"

# The interpreter's exit code depends on HIT_GOOD_TRAP (non-zero)
# on some configs; accept 0 and 127 (HIT_GOOD_TRAP) as success, and
# treat anything else as a hard failure.
case "$rc" in
    0|127) ;;
    *)
        echo "run-nemu-ref: nemu exited $rc (expected 0 or 127 " \
             "for HIT_GOOD_TRAP)" >&2
        exit 1
        ;;
esac

if [[ ! -f "$itrace_out" ]]; then
    echo "run-nemu-ref: itrace output missing at $itrace_out" >&2
    echo "run-nemu-ref: was the build configured with CONFIG_ITRACE_BINARY=y?" >&2
    exit 1
fi

python3 - "$benchmark" "$itrace_out" "$stats_json" "$wallclock" \
        "$mode" "$final_count" <<'PY'
import json, os, sys
from pathlib import Path

bench = sys.argv[1]
itrace = Path(sys.argv[2])
out = Path(sys.argv[3])
wallclock = float(sys.argv[4])
mode = sys.argv[5]
final_count = int(sys.argv[6])
size = itrace.stat().st_size
# BS-13 SSOT: checkpoint record is 608 bytes (matches
# CaeTraceCheckpointRecord in cae-qemu include/cae/trace.h:
# 8 retire_index + 8 pc + 32*8 gpr + 32*8 fpr + 8*8 csrs + 8
# memory_hash + 4 flags + 4 reserved). Trace record stays 40.
record_size = 40 if mode == "trace" else 608
header_size = 16
records = (size - header_size) // record_size if size >= header_size else 0
# BS-20 AC-K-3.1: exact retired-insn count. In trace mode the
# per-retire record count equals the total retired count (each
# retire emits one 40-byte record). In checkpoint mode the
# per-retire records are dropped but the NEMU-side emitter
# prints the final counter to stderr at exit; we parse it above
# and pass it in as `final_count`. Fall back to
# records * interval as a sanity check when final_count is 0.
if mode == "trace":
    retired_insn_count = records
elif final_count > 0:
    retired_insn_count = final_count
else:
    retired_insn_count = records * 1000000
out.write_text(json.dumps({
    "backend": "nemu-ref",
    "benchmark": bench,
    "retired_insn_count": retired_insn_count,
    "itrace_out": str(itrace),
    "itrace_mode": mode,
    "itrace_records": records,
    "itrace_bytes": size,
    "wallclock_seconds": wallclock,
}, indent=2) + "\n")
PY

echo "run-nemu-ref: OK"
echo "  benchmark : $benchmark"
echo "  itrace    : $itrace_out"
echo "  stats     : $stats_json"
echo "  mode      : $mode"
echo "  wallclock : ${wallclock}s"
