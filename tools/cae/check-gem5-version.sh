#!/usr/bin/env bash
#
# Verify the local gem5 checkout matches the pinned commit.
#
# Phase-2 difftest compares CAE against a specific gem5 revision. Drifting
# gem5 silently would invalidate all stored baselines, so this script is
# called by run-gem5.py (and ci-gate.py pre-stage) to fail closed when the
# checkout has moved.
#
# Usage:
#   tools/check-gem5-version.sh [/path/to/gem5]
#
# Defaults to GEM5_ROOT or ~/gem5 if no argument is given.
# Prints a diagnostic and exits 1 on mismatch; prints nothing and exits 0
# on match. The expected SHA is read from tools/gem5-version.txt.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pin_file="$here/gem5-version.txt"

if [[ ! -r "$pin_file" ]]; then
    echo "check-gem5-version: pin file not readable: $pin_file" >&2
    exit 2
fi

expected="$(tr -d '[:space:]' <"$pin_file")"
if [[ -z "$expected" ]]; then
    echo "check-gem5-version: pin file is empty: $pin_file" >&2
    exit 2
fi

gem5_dir="${1:-${GEM5_ROOT:-$HOME/gem5}}"

if [[ ! -d "$gem5_dir/.git" ]]; then
    echo "check-gem5-version: gem5 directory is not a git checkout: $gem5_dir" >&2
    exit 1
fi

actual="$(git -C "$gem5_dir" rev-parse HEAD)"

# Accept either a full 40-char SHA pin or an abbreviated prefix match.
if [[ "$actual" == "$expected" ]]; then
    exit 0
fi
if [[ "${#expected}" -lt 40 && "$actual" == "$expected"* ]]; then
    exit 0
fi

cat >&2 <<EOF
check-gem5-version: gem5 HEAD does not match the pinned commit.
  gem5 dir : $gem5_dir
  expected : $expected  (from $pin_file)
  actual   : $actual
Phase-2 difftest baselines are valid only at the pinned commit; refusing
to run. Either checkout $expected in $gem5_dir, or update the pin via an
explicit RFC before rerunning.
EOF
exit 1
