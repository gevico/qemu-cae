#!/usr/bin/env bash
#
# Verify the local XiangShan GEM5 checkout matches the pinned commit.
#
# The KMH-V3 calibration track compares CAE against a specific
# XiangShan GEM5 revision (kmhv3.py baseline). Silent drift would
# invalidate every stored IPC baseline, so this script is called by
# run-xs-gem5.py (and ci-gate.py pre-stage) to fail closed when the
# checkout has moved.
#
# Usage:
#   tools/check-xiangshan-gem5-version.sh [/path/to/xiangshan-gem5]
#
# Defaults to XIANGSHAN_GEM5_ROOT or ~/xiangshan-gem5 when no argument
# is given. Prints a diagnostic and exits 1 on mismatch; prints nothing
# and exits 0 on match. The expected SHA is read from
# tools/xiangshan-gem5-version.txt.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pin_file="$here/xiangshan-gem5-version.txt"

if [[ ! -r "$pin_file" ]]; then
    echo "check-xiangshan-gem5-version: pin file not readable: $pin_file" >&2
    exit 2
fi

expected="$(tr -d '[:space:]' <"$pin_file")"
if [[ -z "$expected" ]]; then
    echo "check-xiangshan-gem5-version: pin file is empty: $pin_file" >&2
    exit 2
fi

xs_dir="${1:-${XIANGSHAN_GEM5_ROOT:-$HOME/xiangshan-gem5}}"

if [[ ! -d "$xs_dir/.git" ]]; then
    echo "check-xiangshan-gem5-version: xiangshan-gem5 directory is not a git checkout: $xs_dir" >&2
    exit 1
fi

actual="$(git -C "$xs_dir" rev-parse HEAD)"

# Accept either a full 40-char SHA pin or an abbreviated prefix match.
if [[ "$actual" == "$expected" ]]; then
    exit 0
fi
if [[ "${#expected}" -lt 40 && "$actual" == "$expected"* ]]; then
    exit 0
fi

cat >&2 <<EOF
check-xiangshan-gem5-version: xiangshan-gem5 HEAD does not match the pinned commit.
  xs-gem5 dir : $xs_dir
  expected    : $expected  (from $pin_file)
  actual      : $actual
KMH-V3 calibration baselines are valid only at the pinned commit; refusing
to run. Either checkout $expected in $xs_dir, or update the pin via an
explicit RFC before rerunning.
EOF
exit 1
