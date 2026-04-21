#!/usr/bin/env bash
#
# Static check enforcing the CAE architecture-neutrality invariant.
#
# Scans the guest-arch-neutral CAE directories for any token or macro
# that leaks a specific guest architecture. Target-specific code lives
# under target/<arch>/cae/ and is not scanned here.
#
# Exit code 0 means the tree is clean. Non-zero means at least one
# leak was found; offending file/line/content is printed on stderr.
#
# Usage:
#   tools/check-cae-arch-neutrality.sh [SRC_ROOT]
#
# SRC_ROOT defaults to the repository root inferred from the script
# location.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src_root="${1:-$(cd "${script_dir}/.." && pwd)}"

# Scan roots: arch-neutral CAE code.
scan_roots=(
    "cae"
    "include/cae"
    "accel/cae"
    "hw/cae"
)

# Any substring that looks like it ties the code to a specific guest ISA
# belongs in this pattern. Case-insensitive so uppercase macros and
# mixed-case comments are both caught. \b word boundaries avoid
# incidental matches inside unrelated identifiers (e.g. "archive").
read -r -d '' leak_pattern <<'EOF' || true
\b(riscv[a-z0-9_]*|cpuriscv[a-z0-9_]*|target/riscv|target_riscv|target-riscv|rv(32|64|128)([a-z0-9_]*)|rv_(opcode|funct3|funct7|rd|rs1|rs2)|op_(lui|auipc|jal|jalr|branch|load|store|op_imm|op|op_imm32|op32|misc_mem|system|amo|load_fp|store_fp|fmadd|fmsub|fnmsub|fnmadd|op_fp))\b
EOF

if ! command -v rg >/dev/null 2>&1; then
    echo "check-cae-arch-neutrality.sh: 'rg' (ripgrep) is required" >&2
    exit 2
fi

missing=0
present=()
for root in "${scan_roots[@]}"; do
    if [[ -d "${src_root}/${root}" ]]; then
        present+=("${src_root}/${root}")
    else
        echo "check-cae-arch-neutrality.sh: missing scan root: ${root}" >&2
        missing=1
    fi
done

if (( missing )); then
    exit 2
fi

if (( ${#present[@]} == 0 )); then
    echo "check-cae-arch-neutrality.sh: no scan roots found" >&2
    exit 2
fi

# Run ripgrep: recursive, case-insensitive, show line numbers, match
# the leak pattern, search every file (-uu to include hidden; -- is a
# safety guard). Exit status 0 -> match found; 1 -> no match.
hits="$(rg -n -i -- "${leak_pattern}" "${present[@]}" || true)"

if [[ -n "${hits}" ]]; then
    echo "check-cae-arch-neutrality.sh: architecture leaks detected" >&2
    printf '%s\n' "${hits}" >&2
    exit 1
fi

echo "check-cae-arch-neutrality.sh: clean (no arch leaks in ${scan_roots[*]})"
exit 0
