#!/usr/bin/env bash
#
# Build the CAE functional-reference copy of NEMU under <repo>/build/nemu-ref/.
#
# ~/NEMU is read-only; this script never mutates it. Instead:
#
#   1. Verify ~/NEMU (or $NEMU_HOME) HEAD matches tools/cae/nemu-version.txt
#      via tools/cae/check-nemu-version.sh.
#   2. Copy ~/NEMU contents into $CAE_NEMU_BUILD_DIR (default
#      <repo>/build/nemu-ref/), skipping any existing build output.
#   3. Apply tools/cae/nemu-itrace.patch idempotently into the build copy.
#      Already-applied state is detected and silently accepted; a
#      divergent local edit inside the build copy aborts with an
#      explicit file path.
#   4. Populate configs/riscv64-gem5-ref_defconfig with
#      CONFIG_ITRACE_BINARY=y appended (kept minimal so the rest of
#      the defconfig's difftest-for-gem5 knobs remain intact).
#   5. Run `make riscv64-gem5-ref_defconfig && make -j$(nproc)`
#      inside the build copy to produce
#      $CAE_NEMU_BUILD_DIR/build/riscv64-nemu-interpreter.
#   6. Write $CAE_NEMU_BUILD_DIR/nemu-itrace-patch.id containing the
#      sha256 of the applied patch so later invocations can detect
#      out-of-band patch changes.
#
# On success, prints the interpreter path and exits 0. Every failure
# mode exits non-zero with a diagnostic; tier-2 regressions are
# allowed to skip-build only by the caller, not here.
#
# Usage:
#   tests/cae/riscv64/difftest/scripts/build-nemu-ref.sh [--force]
#
# --force: rebuild even when nemu-itrace-patch.id is current (defaults
#          to incremental rebuild; `make` itself skips unchanged work).
# --clean: wipe $CAE_NEMU_BUILD_DIR before starting (forces full rebuild).

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../../.." && pwd)"
pin_check="$repo_root/tools/cae/check-nemu-version.sh"
patch_file="$repo_root/tools/cae/nemu-itrace.patch"

: "${NEMU_HOME:=$HOME/NEMU}"
: "${CAE_NEMU_BUILD_DIR:=$repo_root/build/nemu-ref}"

force=0
clean=0
for arg in "$@"; do
    case "$arg" in
        --force) force=1 ;;
        --clean) clean=1 ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "build-nemu-ref: unknown flag '$arg'" >&2
            exit 2
            ;;
    esac
done

if [[ ! -r "$pin_check" ]]; then
    echo "build-nemu-ref: missing $pin_check" >&2
    exit 2
fi
if [[ ! -r "$patch_file" ]]; then
    echo "build-nemu-ref: missing $patch_file" >&2
    exit 2
fi
if [[ ! -d "$NEMU_HOME/.git" ]]; then
    echo "build-nemu-ref: $NEMU_HOME is not a git checkout" >&2
    exit 1
fi

"$pin_check" "$NEMU_HOME"

# Advisory: ~/NEMU must be clean on disk so the staged copy actually
# reflects the pinned commit. Not fatal (operator may have an
# untracked experiment file unrelated to this build), but worth
# surfacing.
dirty="$(git -C "$NEMU_HOME" status --porcelain 2>/dev/null || true)"
if [[ -n "$dirty" ]]; then
    echo "build-nemu-ref: NOTE: ~/NEMU has uncommitted changes; " \
         "staged copy will include them. File list:" >&2
    echo "$dirty" >&2
fi

if (( clean )); then
    echo "build-nemu-ref: cleaning $CAE_NEMU_BUILD_DIR"
    rm -rf "$CAE_NEMU_BUILD_DIR"
fi

mkdir -p "$CAE_NEMU_BUILD_DIR"

# Stage: rsync ~/NEMU into the build dir without .git and without
# any prior build output. We keep .git for patch tooling (git apply
# --check uses it) but re-copy it from a clean state each time.
#
# Using rsync (not cp) so repeated invocations are incremental and
# exact on file timestamps. The --delete on the per-tree side keeps
# the staged tree in lockstep with the source.
rsync_opts=(-a --delete --exclude='/build/' --exclude='/build-*/')
if command -v rsync >/dev/null 2>&1; then
    rsync "${rsync_opts[@]}" "$NEMU_HOME"/ "$CAE_NEMU_BUILD_DIR"/
else
    # Fallback: plain cp -r. Loses incrementality but works on
    # stripped-down hosts.
    echo "build-nemu-ref: rsync not available; falling back to cp -r"
    rm -rf "$CAE_NEMU_BUILD_DIR.tmp"
    cp -r "$NEMU_HOME" "$CAE_NEMU_BUILD_DIR.tmp"
    rm -rf "$CAE_NEMU_BUILD_DIR"
    mv "$CAE_NEMU_BUILD_DIR.tmp" "$CAE_NEMU_BUILD_DIR"
fi

patch_sha="$(sha256sum "$patch_file" | awk '{print $1}')"
patch_id_file="$CAE_NEMU_BUILD_DIR/nemu-itrace-patch.id"
prior_sha=""
if [[ -r "$patch_id_file" ]]; then
    prior_sha="$(cat "$patch_id_file")"
fi

# Idempotent patch apply. `git apply --check` succeeds only when the
# patch applies cleanly; if it fails we try --reverse to detect an
# already-applied state, which we accept silently. Anything else
# (divergent edit in the staged copy) aborts.
apply_needed=1
pushd "$CAE_NEMU_BUILD_DIR" >/dev/null
if git apply --check "$patch_file" >/dev/null 2>&1; then
    # Fresh tree, patch applies cleanly.
    apply_needed=1
elif git apply --reverse --check "$patch_file" >/dev/null 2>&1; then
    # Already applied. No-op.
    apply_needed=0
    if (( force )); then
        echo "build-nemu-ref: --force: reverting + re-applying patch"
        git apply --reverse "$patch_file"
        apply_needed=1
    fi
else
    echo "build-nemu-ref: staged copy in $CAE_NEMU_BUILD_DIR has " \
         "diverged from what the patch expects. Either:" >&2
    echo "  - re-run with --clean to wipe and restage," >&2
    echo "  - or inspect the staged tree for local edits." >&2
    exit 1
fi

if (( apply_needed )); then
    git apply "$patch_file"
    echo "build-nemu-ref: applied $patch_file"
fi
popd >/dev/null

echo "$patch_sha" > "$patch_id_file"

# BS-6 (round 4): NEMU's top-level Makefile unconditionally includes
# scripts/repos.mk, which itself includes softfloat.mk, nanopb.mk,
# Libcheckpoint.mk, and LibcheckpointAlpha.mk. Each of those four
# fragments fires a `git clone` at Makefile-parse time whenever its
# target resource directory is absent. Those clones hit GitHub. In
# offline review environments (Codex's review shell, air-gapped CI)
# the clones fail silently at Makefile-parse time and the build
# aborts with a misleading "target not found" error.
#
# Mitigation: create sentinel files at each guarded wildcard path so
# the `ifeq ($(wildcard ...), )` short-circuits. This prevents the
# clones. LibCheckpoint* features are then turned off explicitly via
# sed on the staged defconfig (CONFIG_LIBCHECKPOINT_ALPHA_RESTORER
# would otherwise force the library in at link time even after the
# clone was skipped). softfloat is genuinely needed (CONFIG_FPU_SOFT
# = y in the defconfig), so if `~/NEMU/resource/softfloat/` is also
# absent the script fails fast with an explicit diagnostic rather
# than attempting a clone.
#
# Operator action when the diagnostic fires: pre-populate the
# upstream ~/NEMU/resource/softfloat/repo/ with a local
# berkeley-softfloat-3 checkout (the pin is upstream of NEMU; NEMU
# doesn't version it). Re-run this script.
pushd "$CAE_NEMU_BUILD_DIR" >/dev/null
mkdir -p resource/nanopb
mkdir -p resource/LibCheckpoint/src
touch    resource/LibCheckpoint/src/checkpoint.proto
mkdir -p resource/gcpt_restore/src
# src/checkpoint/serializer.cpp unconditionally includes this header
# and references ~12 address macros (BOOT_FLAG_ADDR, PC_CPT_ADDR, ...
# CSR_REG_DONE, VECTOR_REG_DONE, etc.) regardless of
# CONFIG_LIBCHECKPOINT_ALPHA_RESTORER. A 0-byte stub here makes
# serializer.cpp fail to compile. Ship the macro values inline from
# OpenXiangShan/LibCheckpointAlpha @ src/restore_rom_addr.h (Mulan
# PSL v2, same license as NEMU) so the standalone interpreter build
# succeeds without network in the offline-review environment.
cat > resource/gcpt_restore/src/restore_rom_addr.h <<'EOF'
#ifndef __RESTORE_ROM_ADDR__
#define __RESTORE_ROM_ADDR__

#define CPT_MAGIC_BUMBER        0xbeef
#define BOOT_CODE               0x0

#define BOOT_FLAG_ADDR          0xECDB0
#define PC_CPT_ADDR             0xECDB8
#define MODE_CPT_ADDR           0xECDC0
#define MTIME_CPT_ADDR          0xECDC8
#define MTIME_CMP_CPT_ADDR      0xECDD0
#define MISC_DONE_CPT_ADDR      0xECDD8
#define MISC_RESERVE            0xECDE0

#define INT_REG_CPT_ADDR        0xEDDE0
#define INT_REG_DONE            0xEDEE0

#define FLOAT_REG_CPT_ADDR      0xEDEE8
#define FLOAT_REG_DONE          0xEDFE8

#define CSR_REG_CPT_ADDR        0xEDFF0
#define CSR_REG_DONE            0xF5FF0
#define CSR_RESERVE             0xF5FF8

#define VECTOR_REG_CPT_ADDR     0xFDFF8
#define VECTOR_REG_DONE         0xFFFF8

#define GCPT_CHECKPOINT_VERSION 0xFFFFC

#define CLINT_MMIO              0x38000000
#define CLINT_MTIMECMP          0x4000
#define CLINT_MTIME             0xBFF8

#define RESTORE_GOOD            0x0
#define RESTORE_MODE_BAD        0x1
#define GCPT_INCOMPLETE         0x2
#define VERSION_NOT_MATCH       0x3

#define COMPLETE_FLAG           0xcaff
#define GCPT_VERSION            0x20231222

#define MISA_V 0x200000
#define MISA_H 0x80

#endif /* __RESTORE_ROM_ADDR__ */
EOF

if [[ ! -f resource/softfloat/repo/COPYING.txt ]]; then
    if [[ -f "$NEMU_HOME/resource/softfloat/repo/COPYING.txt" ]]; then
        echo "build-nemu-ref: rsyncing softfloat from \$NEMU_HOME"
        mkdir -p resource/softfloat
        rsync -a "$NEMU_HOME/resource/softfloat/" resource/softfloat/
    else
        echo "build-nemu-ref: missing offline-prepared softfloat at " \
             "resource/softfloat/repo/COPYING.txt" >&2
        echo "build-nemu-ref: prepare it on the author's host before " \
             "running this script in offline mode, e.g.:" >&2
        echo "  git clone --depth=1 " \
             "https://github.com/ucb-bar/berkeley-softfloat-3 " \
             "~/NEMU/resource/softfloat/repo" >&2
        exit 1
    fi
fi
popd >/dev/null

# Configure. The defconfig ships with every difftest-for-gem5 knob
# enabled, plus our new CONFIG_ITRACE_BINARY after patch apply.
# `make <config>_defconfig` writes .config at the repo root; the
# patch adds a Kconfig item that defaults n, so we flip it on
# explicitly via olddefconfig+sed afterwards.
#
# NEMU's top-level Makefile verifies NEMU_HOME points at a NEMU
# repo (fatal otherwise — "Makefile:18: *** NEMU_HOME= is not a NEMU
# repo"). The operator-facing NEMU_HOME is the read-only source at
# ~/NEMU; for the staged build copy we need to rebind the variable
# so the Makefile's self-check recognizes this tree. Export inside a
# subshell so the host-shell NEMU_HOME the caller set for
# check-nemu-version.sh is preserved for any downstream steps.
pushd "$CAE_NEMU_BUILD_DIR" >/dev/null
(
    export NEMU_HOME="$CAE_NEMU_BUILD_DIR"

    # Edit the staged defconfig to:
    #   (a) flip CONFIG_SHARE from y to n — the stock
    #       riscv64-gem5-ref_defconfig builds a .so for XS-GEM5's
    #       in-process difftest reference, but the CAE pipeline uses
    #       NEMU as a standalone process (no .so linking; AC-K-2
    #       contract + plan "Can NOT use: linking NEMU .so into CAE
    #       process"). Standalone build produces the
    #       riscv64-nemu-interpreter binary we need for
    #       `run-nemu-ref.sh --itrace-out`.
    #   (b) append CONFIG_ITRACE_BINARY=y so the patch's emit path
    #       is enabled at build time.
    #   (c) Repoint CONFIG_MMIO_RESET_VECTOR from 0x10000000 to
    #       0x80000000 so cpu.pc matches the XS raw binary's link
    #       address (BS-3 / AC-K-2 reset-PC contract). The stock
    #       defconfig keeps RESET_FROM_MMIO=y for XS-GEM5 difftest,
    #       where a bootrom stub at 0x10000000 jumps into MBASE; the
    #       CAE difftest path skips the bootrom and boots the raw
    #       *.xs.bin (linked at 0x80000000) directly, so the reset
    #       vector must land inside MBASE.
    # Idempotent (grep-guarded). NEMU's Kconfig wrapper does not
    # ship a configs/olddefconfig fragment, so we avoid the
    # `make olddefconfig` path entirely.
    defconfig=configs/riscv64-gem5-ref_defconfig
    if grep -q '^CONFIG_SHARE=y' "$defconfig"; then
        sed -i 's/^CONFIG_SHARE=y$/CONFIG_SHARE=n/' "$defconfig"
    fi
    if grep -q '^CONFIG_MMIO_RESET_VECTOR=0x10000000' "$defconfig"; then
        sed -i 's|^CONFIG_MMIO_RESET_VECTOR=0x10000000$|CONFIG_MMIO_RESET_VECTOR=0x80000000|' \
            "$defconfig"
    fi
    # BS-6 part 2: turn off the LibCheckpointAlpha link dependency
    # so the build succeeds even when the resource stub we created
    # above contains only the guard-file. CAE tier-1 difftest is
    # trace-based, not checkpoint-restore-based, so the restorer is
    # not functionally required on this path.
    if grep -q '^CONFIG_LIBCHECKPOINT_ALPHA_RESTORER=y' "$defconfig"; then
        sed -i 's/^CONFIG_LIBCHECKPOINT_ALPHA_RESTORER=y$/# CONFIG_LIBCHECKPOINT_ALPHA_RESTORER is not set/' \
            "$defconfig"
    fi
    if ! grep -q '^CONFIG_ITRACE_BINARY=y' "$defconfig"; then
        printf '\n# Appended by scripts/build-nemu-ref.sh for CAE difftest.\n' \
            >>"$defconfig"
        printf 'CONFIG_ITRACE_BINARY=y\n' >>"$defconfig"
    fi
    # Round 45: our benchmarks end with `ebreak` (the shared
    # prologue in tests/cae/riscv64/difftest/benchmarks/src/bench.S),
    # which QEMU bare-metal and gem5 SE both treat as the
    # program terminator. NEMU's standalone interpreter
    # otherwise loops forever on ebreak because the binaries
    # don't emit NEMU's custom `nemu_trap` opcode. Enable
    # CONFIG_EBREAK_AS_TRAP so the staged interpreter treats
    # ebreak the same as nemu_trap and stops cleanly on the
    # benchmark's sentinel-write + ebreak.
    #
    # EBREAK_AS_TRAP depends on !RV_DEBUG per
    # src/isa/riscv64/Kconfig:631 — flip RV_DEBUG off so the
    # Kconfig dependency is satisfied and the ebreak->trap
    # mapping actually compiles into the interpreter. CAE
    # difftest has no use for NEMU's hardware-debug-mode
    # stub anyway.
    if grep -q '^CONFIG_RV_DEBUG=y' "$defconfig"; then
        sed -i 's/^CONFIG_RV_DEBUG=y$/# CONFIG_RV_DEBUG is not set/' "$defconfig"
    fi
    if ! grep -q '^CONFIG_EBREAK_AS_TRAP=y' "$defconfig"; then
        printf 'CONFIG_EBREAK_AS_TRAP=y\n' >>"$defconfig"
    fi
    # Round 46 AC-K-2.4: NEMU's RVV vector-store path in
    # src/isa/riscv64/instr/rvv/vldst_impl.c:604 carries a stale
    # 3-arg extern declaration of store_commit_queue_push, while
    # paddr.c's real definition has 4 args. The mismatch is
    # latent under CONFIG_SHARE=y (ifdef-different path), but
    # fires at link time when we enable CONFIG_DIFFTEST_STORE_COMMIT
    # alongside CONFIG_RVV under CONFIG_SHARE=n. CAE tier-1
    # benchmarks don't use RVV, so disable it in the staged
    # defconfig — unblocks the store-commit path without patching
    # the upstream stale extern decl.
    if grep -q '^CONFIG_RVV=y' "$defconfig"; then
        sed -i 's/^CONFIG_RVV=y$/# CONFIG_RVV is not set/' "$defconfig"
    fi
    make riscv64-gem5-ref_defconfig >/dev/null
    if ! grep -q '^CONFIG_ITRACE_BINARY=y' .config; then
        echo "build-nemu-ref: .config did not pick up " \
             "CONFIG_ITRACE_BINARY=y after defconfig; check " \
             "$defconfig in the staged tree" >&2
        exit 1
    fi
    if grep -q '^CONFIG_SHARE=y' .config; then
        echo "build-nemu-ref: .config still has CONFIG_SHARE=y; " \
             "CAE needs the standalone interpreter" >&2
        exit 1
    fi
    if ! grep -q '^CONFIG_MMIO_RESET_VECTOR=0x80000000' .config; then
        echo "build-nemu-ref: .config did not pick up " \
             "CONFIG_MMIO_RESET_VECTOR=0x80000000; the XS raw binary " \
             "won't boot because cpu.pc lands outside MBASE" >&2
        exit 1
    fi

    # Build. Single-threaded fallback when nproc isn't available.
    jobs="$(nproc 2>/dev/null || echo 4)"
    make -j"$jobs"
)

out_bin="$CAE_NEMU_BUILD_DIR/build/riscv64-nemu-interpreter"
if [[ ! -x "$out_bin" ]]; then
    echo "build-nemu-ref: expected binary missing: $out_bin" >&2
    exit 1
fi
popd >/dev/null

if [[ "$prior_sha" != "$patch_sha" ]]; then
    echo "build-nemu-ref: patch sha recorded: $patch_sha"
fi
echo "build-nemu-ref: OK"
echo "  interpreter: $out_bin"
echo "  patch id   : $patch_id_file ($patch_sha)"
echo "  defconfig  : riscv64-gem5-ref_defconfig + CONFIG_ITRACE_BINARY=y"
