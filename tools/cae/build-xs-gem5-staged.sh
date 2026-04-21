#!/usr/bin/env bash
#
# Plan-compliant staged XS-GEM5 build helper.
#
# Plan reference: docs/superpowers/plans/2026-04-18-cae-phase-2-
# revision-kmhv3-plan.md:217 states that `~/xiangshan-gem5` is
# READ-ONLY and all build artefacts belong under
# `<repo>/build/xs-gem5-build`.
#
# Round 53 AC-K-5: this script enforces that contract. It copies
# the pinned source tree into the repo-local staged area via
# `rsync -a --exclude='build/' --exclude='.git/'`, runs
# `scons build/RISCV/gem5.opt` INSIDE the staged copy, and
# symlinks the resulting binary at
# `<repo>/build/xs-gem5-build/gem5.opt` so `run-xs-gem5.py`'s
# `_resolve_gem5_opt()` finds it. `~/xiangshan-gem5/` is not
# mutated at any step; a post-build verification prints its
# mtime + sha256 summary so the operator can confirm.
#
# Usage:
#   tools/build-xs-gem5-staged.sh [XIANGSHAN_GEM5_ROOT]
#
# XIANGSHAN_GEM5_ROOT defaults to ~/xiangshan-gem5.
# If already staged, rsync is incremental; a re-run after the
# pin changes refreshes the staged source copy.
#
# Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms and conditions of the GNU General
# Public License, version 2 or later, as published by the Free
# Software Foundation.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$here/.." && pwd)"

xs_root="${1:-${XIANGSHAN_GEM5_ROOT:-$HOME/xiangshan-gem5}}"
xs_root="$(realpath "$xs_root")"

staged_dir="$repo_root/build/xs-gem5-build"
staged_src="$staged_dir/src"
staged_bin="$staged_dir/gem5.opt"

if [[ ! -d "$xs_root" ]]; then
    echo "build-xs-gem5-staged: source tree missing: $xs_root" >&2
    echo "  set XIANGSHAN_GEM5_ROOT or pass the path as an" \
         "argument" >&2
    exit 1
fi

# Verify the pin before spending a 20-40 min build on the wrong
# commit. tools/check-xiangshan-gem5-version.sh is the canonical
# pin checker; it exits 0 when the checkout's HEAD matches.
check_pin="$repo_root/tools/check-xiangshan-gem5-version.sh"
if [[ -x "$check_pin" ]]; then
    echo "build-xs-gem5-staged: verifying XS-GEM5 pin..."
    "$check_pin" "$xs_root"
fi

# Snapshot the external tree's state for the
# read-only-invariant check at the end. sha256sum over a sorted
# file list captures both file-content changes and any new /
# removed files. Round 54 (Codex round-53 queued directive):
# the scope covers the WHOLE external checkout minus `build/`
# and `.git/`, matching both the rsync exclude list below and
# the "byte-identical before/after" wording in the doc. Earlier
# round-53 scope was narrower (only configs/src/SConstruct/
# site_scons/build_opts/build_tools/ext/resource), which left
# `SConsopts`, `include`, `system`, `tests`, `util`, and root-
# level dotfiles outside the invariant check.
snapshot_external() {
    # NUL-separated pipeline so filenames with spaces (e.g. the
    # XS-GEM5 "VirtIORng 2.py" duplicates) don't split into
    # bogus path fragments. `xargs -0 sha256sum` reads one file
    # per NUL; the outer `sha256sum | awk` reduces to a single
    # hex digest.
    (
        cd "$xs_root" && \
        find . -type f \
             ! -path "./build/*" \
             ! -path "./.git/*" \
             -print0 \
             2>/dev/null \
             | LC_ALL=C sort -z \
             | xargs -0 -r sha256sum \
             | sha256sum \
             | awk '{print $1}'
    )
}
echo "build-xs-gem5-staged: snapshotting external tree pre-build"
external_snapshot="$(snapshot_external)"
echo "build-xs-gem5-staged: external sha256 (pre) = $external_snapshot"

# Rsync source to the staged area. --exclude='build/' keeps
# the build output out of the source copy; we write output into
# `$staged_src/build/RISCV/` below. --exclude='.git/' drops VCS
# metadata (scons doesn't need it).
mkdir -p "$staged_src"
echo "build-xs-gem5-staged: rsyncing source -> $staged_src"
rsync -a --delete \
      --exclude='build/' \
      --exclude='.git/' \
      "$xs_root"/ "$staged_src"/

# Apply repo-local patches AFTER rsync so the external checkout at
# $xs_root stays byte-identical per plan.md:217. Each patch is a
# plain unified diff against paths rooted at `$staged_src`, so
# `patch -p1 -d $staged_src` applies it in the standard scons
# source layout. The patches directory is `tools/`; any file
# matching `tools/xs-gem5-*.patch` is applied in sort order.
# Failure aborts the build so a partially-patched tree never
# produces a misleading binary.
shopt -s nullglob
patches=("$repo_root/tools/"xs-gem5-*.patch)
shopt -u nullglob
if (( ${#patches[@]} > 0 )); then
    echo "build-xs-gem5-staged: applying ${#patches[@]} repo-local" \
         "patch(es) to $staged_src"
    for p in "${patches[@]}"; do
        echo "  - applying $(basename "$p")"
        if ! patch -p1 -d "$staged_src" --forward < "$p"; then
            echo "build-xs-gem5-staged: patch apply failed:" \
                 "$p" >&2
            exit 1
        fi
    done
fi

# Build. scons writes into `$staged_src/build/RISCV/`. Parallelism
# defaults to nproc; the caller can override via $MAKE_JOBS.
jobs="${MAKE_JOBS:-$(nproc)}"

# Round 54 AC-K-5: the XS-GEM5 build runs inside a Docker
# container rather than on the bare host. Rationale:
#   (1) avoids needing host-side `sudo apt install
#       libboost-all-dev` / pacman equivalents — the image
#       carries all scons + boost + toolchain prerequisites;
#   (2) keeps the build environment reproducible across dev
#       hosts (WSL, native Linux, containers on CI);
#   (3) plan.md:217 read-only contract on `~/xiangshan-gem5`
#       is preserved either way — the container mounts ONLY
#       the staged repo-local source copy, never the external
#       checkout.
#
# Canonical image: `cae/xs-gem5-build:latest`, built from the
# in-repo Dockerfile `tools/xs-gem5-build.Dockerfile`. The
# in-repo Dockerfile ships with every XS-GEM5 scons
# prerequisite (libboost-all-dev, libsqlite3-dev, libhdf5-dev,
# libprotobuf-dev, ...), so no in-container apt-install is
# required. Set `XS_GEM5_BUILD_IMAGE=<image>` to override,
# e.g. to a legacy `gem5-build:local` or
# `ghcr.io/gem5/ubuntu-24.04_all-dependencies:v24-0`.
build_image="${XS_GEM5_BUILD_IMAGE:-cae/xs-gem5-build:latest}"
dockerfile_path="$repo_root/tools/xs-gem5-build.Dockerfile"
if ! docker image inspect "$build_image" >/dev/null 2>&1; then
    # Auto-build the canonical image from the in-repo
    # Dockerfile. Only kicks in when the default image is in
    # effect and the Dockerfile is actually present — an
    # explicit `XS_GEM5_BUILD_IMAGE=<other>` override with a
    # missing image is still treated as a hard error so the
    # operator doesn't silently get a different image than
    # requested.
    if [[ "$build_image" == "cae/xs-gem5-build:latest" \
          && -f "$dockerfile_path" ]]; then
        echo "build-xs-gem5-staged: image '$build_image' not" \
             "present. Building from $dockerfile_path..."
        docker build -t "$build_image" \
                     -f "$dockerfile_path" \
                     "$repo_root/tools/"
    else
        echo "build-xs-gem5-staged: docker image" \
             "'$build_image' not present." >&2
        echo "  Build the canonical image:" >&2
        echo "    docker build -t cae/xs-gem5-build:latest \\" >&2
        echo "        -f tools/xs-gem5-build.Dockerfile" \
             "tools/" >&2
        echo "  or override via" \
             "XS_GEM5_BUILD_IMAGE=<image>." >&2
        exit 1
    fi
fi

echo "build-xs-gem5-staged: running scons build/RISCV/gem5.opt" \
     "-j$jobs inside $build_image (expect 20-40 min)"

# Run as root inside the container so scons can write into
# the mounted `/workspace` regardless of the host UID/GID
# layout. After scons finishes, the container chowns the
# build outputs back to the calling user so the host can
# delete / re-enter `<repo>/build/xs-gem5-build/src/` without
# sudo. The XS-GEM5 source copy is the only path mounted, so
# the host's external checkout remains read-only.
#
# The canonical image ships with every scons dep baked in
# (libboost-all-dev, libsqlite3-dev, ...), so no
# in-container apt-install is required. If an override image
# is used that lacks some dep, the build will fail fast with
# the missing-header error — fix by updating
# `tools/xs-gem5-build.Dockerfile` and rebuilding the image,
# NOT by ad-hoc apt-install inside this script (that path is
# intentionally removed because it drifts the build
# environment away from the recorded Dockerfile).
host_uid="$(id -u)"
host_gid="$(id -g)"
docker run --rm \
    -v "$staged_src":/workspace:rw \
    -w /workspace \
    "$build_image" \
    bash -c "
set -euo pipefail
scons build/RISCV/gem5.opt -j${jobs}
chown -R ${host_uid}:${host_gid} build
"

built_bin="$staged_src/build/RISCV/gem5.opt"
if [[ ! -x "$built_bin" ]]; then
    echo "build-xs-gem5-staged: scons reported success but" \
         "$built_bin is missing" >&2
    exit 1
fi

# Relink the staged-area pointer so run-xs-gem5.py's
# _resolve_gem5_opt() picks it up. The target path stays at
# <repo>/build/xs-gem5-build/gem5.opt per the runner's
# expectation.
rm -f "$staged_bin"
ln -s "$built_bin" "$staged_bin"

# Round 55 Q1 (Codex round-54 queued): persist the exact
# image tag+digest used for this build next to the staged
# binary so the runner can pin the SAME image for `docker
# run`. Without this file `run-xs-gem5.py` falls back to
# ambient `$XS_GEM5_BUILD_IMAGE` / its compile-time default
# and can silently drift — e.g. a binary built under
# `gem5-build:local` getting executed under
# `cae/xs-gem5-build:latest`. The file is two lines:
#   image: <tag>
#   digest: sha256:<...>
# Written as plain text so ops can `cat` it and any tool
# (bash, python, jq-less grep) can parse it trivially.
image_digest="$(docker image inspect -f '{{.Id}}' \
                      "$build_image" 2>/dev/null || true)"
image_meta="$staged_dir/image.txt"
{
    echo "image: $build_image"
    echo "digest: ${image_digest:-unknown}"
    echo "# Recorded by tools/build-xs-gem5-staged.sh at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$image_meta"
echo "build-xs-gem5-staged: recorded build image in $image_meta"

# Re-snapshot the external tree; the hash MUST match the
# pre-build value. Any divergence means the staged build
# accidentally wrote back into the read-only checkout and is a
# plan violation. Scope matches the pre-snapshot + the rsync
# exclude list (whole tree minus `build/` and `.git/`).
external_snapshot_post="$(snapshot_external)"

echo "build-xs-gem5-staged: external sha256 (post) =" \
     "$external_snapshot_post"

if [[ "$external_snapshot" != "$external_snapshot_post" ]]; then
    echo "build-xs-gem5-staged: plan-violation detected —" \
         "$xs_root mutated during build (sha256 changed)" >&2
    echo "  pre:  $external_snapshot" >&2
    echo "  post: $external_snapshot_post" >&2
    exit 1
fi

echo "build-xs-gem5-staged: done. Staged binary:"
echo "  $staged_bin -> $built_bin"
echo "  external tree bit-identical before / after (ok)"
echo ""
echo "Next step: bash tests/cae-difftest/scripts/run-xs-suite.sh" \
     "xs-1c-kmhv3 --benchmark alu"
