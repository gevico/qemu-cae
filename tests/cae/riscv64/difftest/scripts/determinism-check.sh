#!/usr/bin/env bash
#
# Phase-1 CAE determinism self-check.
#
# Runs run-cae.py twice for the same <config> <benchmark> and compares
# the two JSON reports byte-for-byte (modulo the `wallclock_seconds`
# field, which is inherently host-timing-dependent). Exit 0 on match,
# 1 otherwise.
#
# Usage:
#   determinism-check.sh [--mode serial|parallel] <config> <benchmark>
#
# Modes:
#   serial   -- hard gate (default); any divergence fails.
#   parallel -- warn-only; divergence triggers a non-zero print but the
#               exit code is still 0. Reserved for M7 bound-weave when
#               cycle-level determinism is not guaranteed; on Phase-1
#               single-threaded RR every run must match bit-for-bit.

set -euo pipefail

MODE="serial"
if [[ "${1:-}" == "--mode" ]]; then
    MODE="${2:?--mode requires serial|parallel}"
    shift 2
fi

config="${1:?missing <config>}"
benchmark="${2:?missing <benchmark>}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../../.." && pwd)"
reports_dir="$repo_root/tests/cae/riscv64/difftest/reports/determinism-${config}-${benchmark}"
mkdir -p "$reports_dir"
run1="$reports_dir/run1.json"
run2="$reports_dir/run2.json"

run_once() {
    local out="$1"
    python3 "$here/run-cae.py" "$config" \
        --benchmark "$benchmark" \
        --output "$out"
}

run_once "$run1"
run_once "$run2"

# Strip wallclock_seconds (and any other intentionally-volatile fields
# introduced later) before comparing. Use python to drop the key.
python3 - "$run1" "$run2" <<'PY'
import json, sys

def normalize(path):
    with open(path) as f:
        data = json.load(f)
    data.pop("wallclock_seconds", None)
    return json.dumps(data, sort_keys=True)

a = normalize(sys.argv[1])
b = normalize(sys.argv[2])
if a == b:
    print("determinism-check: OK (two runs byte-identical)")
    sys.exit(0)
import difflib
diff = "\n".join(difflib.unified_diff(a.splitlines(), b.splitlines(),
                                      lineterm="", n=2))
print("determinism-check: FAILED — two runs differ:")
print(diff)
sys.exit(1)
PY
rc=$?

if [[ "$MODE" == "parallel" && $rc -ne 0 ]]; then
    echo "determinism-check: parallel mode — divergence noted but not failing" >&2
    exit 0
fi
exit $rc
