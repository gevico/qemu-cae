#!/usr/bin/env bash
#
# Orchestrate a single <config> <benchmark> difftest:
#   1. config-equivalence.py  -- catch unsupported fields before simulating
#   2. run-gem5.py            -- produces gem5.json (takes longer)
#   3. run-cae.py             -- produces cae.json
#   4. diff.py                -- writes accuracy-gate.json + diff-*.md
#   5. ci-gate.py --stage all -- re-run every pipeline stage as a gate
#
# Reports land under tests/cae/difftest/reports/<config>-<benchmark>/.
# Returns the exit code of the final gate.
#
# For the AC-4 six-benchmark sweep, use run-suite.sh instead:
#
#   bash tests/cae/difftest/scripts/run-suite.sh inorder-1c
#
# run-suite.sh is the authoritative one-command AC-4 entry point; it
# hard-fails on any missing / failing per-benchmark report.

set -euo pipefail

config="${1:?missing <config>}"
benchmark="${2:?missing <benchmark>}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../../../../.." && pwd)"
: "${CAE_BUILD_DIR:=$repo_root/build}"
export CAE_BUILD_DIR
report_dir="$CAE_BUILD_DIR/tests/cae/difftest/reports/${config}-${benchmark}"
mkdir -p "$report_dir"

echo "== config-equivalence =="
python3 "$here/config-equivalence.py" "$config" \
    --output "$report_dir/config-equivalence.json" \
    || {
        echo "run-all: config-equivalence reported unsupported fields; aborting"
        exit 1
    }

echo "== run-gem5.py =="
python3 "$here/run-gem5.py" "$config" \
    --benchmark "$benchmark" \
    --output "$report_dir/gem5.json"

echo "== run-cae.py =="
python3 "$here/run-cae.py" "$config" \
    --benchmark "$benchmark" \
    --output "$report_dir/cae.json"

echo "== diff.py =="
diff_rc=0
python3 "$here/diff.py" \
    --cae "$report_dir/cae.json" \
    --gem5 "$report_dir/gem5.json" \
    --config "$config" \
    --report-dir "$report_dir" \
    || diff_rc=$?

echo
echo "== ci-gate.py --stage diff_threshold =="
gate_rc=0
python3 "$here/ci-gate.py" \
    --stage diff_threshold \
    --config "$config" \
    --benchmark "$benchmark" \
    --strict \
    || gate_rc=$?

echo
echo "== Report files =="
ls -1 "$report_dir"

# The gate's exit code is authoritative; diff.py's non-zero exit is
# already covered by the same accuracy-gate.json, so prefer the gate
# when both disagree.
if [[ $gate_rc -ne 0 ]]; then
    exit $gate_rc
fi
exit $diff_rc
