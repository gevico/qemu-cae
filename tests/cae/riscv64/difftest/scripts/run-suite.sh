#!/usr/bin/env bash
#
# AC-4 suite driver. Takes a single <config> argument and orchestrates
# the full six-benchmark sweep end-to-end:
#
#   1. config-equivalence.py           (unsupported fields)
#   2. run-gem5.py + run-cae.py        per benchmark
#   3. diff.py                         per benchmark
#   4. determinism-check.sh --mode serial  per benchmark
#   5. ci-gate.py --stage suite        suite-wide max / geomean verdict
#
# Missing report, failing diff, or failing determinism = hard fail.
# Exit code:
#   0 = full suite passed (AC-4 suite gate green and every benchmark
#       has bit-identical determinism coverage)
#   1 = at least one benchmark's diff or determinism failed
#   2 = suite-wide ci-gate verdict failed (per-metric max/geomean)
#   >2 = driver-level error (missing scripts, malformed config, etc.)
#
# The suite is hardcoded to the six AC-4 gated benchmarks; adjust
# AC4_BENCHMARKS in tests/cae/difftest/scripts/ci-gate.py to match
# when the plan's suite changes.

set -euo pipefail

config="${1:?missing <config>}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../../../../.." && pwd)"
: "${CAE_BUILD_DIR:=$repo_root/build}"
export CAE_BUILD_DIR
reports_root="$CAE_BUILD_DIR/tests/cae/difftest/reports"
mkdir -p "$reports_root"

benchmarks=(alu mem-stream pointer-chase branchy matmul-small coremark-1c)

echo "== config-equivalence: $config =="
python3 "$here/config-equivalence.py" "$config" \
    --output "$reports_root/${config}-config-equivalence.json" \
    || {
        echo "run-suite: config-equivalence reported unsupported fields; aborting" >&2
        exit 3
    }

any_bench_fail=0
for bench in "${benchmarks[@]}"; do
    echo ""
    echo "== benchmark: $bench =="
    report_dir="$reports_root/${config}-${bench}"
    determ_dir="$reports_root/determinism-${config}-${bench}"

    # Clear any pre-existing artifacts so this run cannot silently
    # aggregate stale per-benchmark gate files. Round-3 shipped a
    # driver that mkdir -p'd the report dir without clearing it; a
    # benchmark that *failed* its diff could leave a passing
    # accuracy-gate.json from a prior run, and ci-gate.py --stage
    # suite would then consume the stale pass. The invariant is now:
    # a stage that does not complete leaves no file behind it.
    rm -rf "$report_dir" "$determ_dir"
    mkdir -p "$report_dir"

    python3 "$here/run-gem5.py" "$config" \
        --benchmark "$bench" \
        --output "$report_dir/gem5.json" || {
            echo "run-suite: run-gem5.py failed for $bench" >&2
            any_bench_fail=1
            continue
        }
    python3 "$here/run-cae.py" "$config" \
        --benchmark "$bench" \
        --output "$report_dir/cae.json" || {
            echo "run-suite: run-cae.py failed for $bench" >&2
            any_bench_fail=1
            continue
        }
    # diff.py produces $report_dir/accuracy-gate.json. Any failure here
    # (including the comparison itself reporting FAIL) must flag the
    # benchmark and remove the report so the suite-stage aggregator
    # cannot consume it. Non-zero exit from diff.py is authoritative.
    if ! python3 "$here/diff.py" \
            --cae "$report_dir/cae.json" \
            --gem5 "$report_dir/gem5.json" \
            --config "$config" \
            --report-dir "$report_dir"; then
        echo "run-suite: diff.py failed for $bench (see $report_dir)" >&2
        any_bench_fail=1
        rm -f "$report_dir/accuracy-gate.json"
    fi

    echo "-- determinism $bench --"
    if ! bash "$here/determinism-check.sh" --mode serial "$config" "$bench"; then
        echo "run-suite: determinism FAIL for $bench" >&2
        any_bench_fail=1
    fi
done

echo ""
echo "== ci-gate.py --stage suite =="
# If any per-benchmark stage failed, the suite stage must be judged
# on incomplete inputs. Surface that clearly: run ci-gate anyway
# (it will skip missing reports and/or fail on incomplete data) but
# override the driver's exit code with the per-benchmark failure.
suite_rc=0
python3 "$here/ci-gate.py" --stage suite --config "$config" || suite_rc=$?

echo ""
echo "== Summary =="
echo "config          : $config"
echo "benchmark-fail  : $any_bench_fail (per-benchmark diff or determinism)"
echo "suite-gate-rc   : $suite_rc (0 = all AC-4 metrics pass)"

if [[ $any_bench_fail -ne 0 ]]; then
    exit 1
fi
if [[ $suite_rc -ne 0 ]]; then
    exit 2
fi
exit 0
