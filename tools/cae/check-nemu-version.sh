#!/usr/bin/env bash
#
# Verify the local NEMU checkout matches the pinned commit.
#
# NEMU serves as the functional difftest reference for the KMH-V3
# calibration track. The retired-instruction trace or architectural
# checkpoint produced by NEMU must be bit-reproducible, so the checkout
# is pinned and this script is called by run-nemu-ref.sh and
# build-nemu-ref.sh to fail closed on drift. ~/NEMU itself is never
# modified; the build copy lives under <repo>/build/nemu-ref/.
#
# Usage:
#   tools/check-nemu-version.sh [/path/to/NEMU]
#
# Defaults to NEMU_HOME or ~/NEMU when no argument is given. Prints a
# diagnostic and exits 1 on mismatch; prints nothing and exits 0 on
# match. The expected SHA is read from tools/nemu-version.txt.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pin_file="$here/nemu-version.txt"

if [[ ! -r "$pin_file" ]]; then
    echo "check-nemu-version: pin file not readable: $pin_file" >&2
    exit 2
fi

expected="$(tr -d '[:space:]' <"$pin_file")"
if [[ -z "$expected" ]]; then
    echo "check-nemu-version: pin file is empty: $pin_file" >&2
    exit 2
fi

nemu_dir="${1:-${NEMU_HOME:-$HOME/NEMU}}"

if [[ ! -d "$nemu_dir/.git" ]]; then
    echo "check-nemu-version: NEMU directory is not a git checkout: $nemu_dir" >&2
    exit 1
fi

actual="$(git -C "$nemu_dir" rev-parse HEAD)"

# Accept either a full 40-char SHA pin or an abbreviated prefix match.
if [[ "$actual" == "$expected" ]]; then
    exit 0
fi
if [[ "${#expected}" -lt 40 && "$actual" == "$expected"* ]]; then
    exit 0
fi

cat >&2 <<EOF
check-nemu-version: NEMU HEAD does not match the pinned commit.
  NEMU dir : $nemu_dir
  expected : $expected  (from $pin_file)
  actual   : $actual
Functional difftest baselines are valid only at the pinned commit; refusing
to run. Either checkout $expected in $nemu_dir, or update the pin via an
explicit RFC before rerunning.
EOF
exit 1
