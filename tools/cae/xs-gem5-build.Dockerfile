# syntax=docker/dockerfile:1
#
# XS-GEM5 build image for the CAE difftest calibration lane.
#
# Round 54 AC-K-5: canonical Dockerfile maintained by this
# repo so future dep additions (sqlite3, hdf5, etc.) land
# via a documented image bump rather than ad-hoc in-container
# apt-installs. `tools/build-xs-gem5-staged.sh` runs scons
# inside a container built from this file; the staged build
# area under `<repo>/build/xs-gem5-build/src/` is the only
# writable mount, so `~/xiangshan-gem5` stays read-only per
# plan.md:217.
#
# Build command:
#   docker build -t cae/xs-gem5-build:latest \
#                -f tools/xs-gem5-build.Dockerfile \
#                tools/
#
# Override the image via the env var the helper reads:
#   XS_GEM5_BUILD_IMAGE=cae/xs-gem5-build:latest \
#       bash tools/build-xs-gem5-staged.sh
#
# Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later

FROM ubuntu:24.04

# Noninteractive apt so the image-build doesn't hang on
# tzdata / debconf prompts. Pinned DEBIAN_FRONTEND is
# strictly better than `yes | apt install` because it also
# silences timezone / locale wizards.
ENV DEBIAN_FRONTEND=noninteractive

# --- Base build toolchain --------------------------------------------------
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        build-essential \
        g++ \
        gcc \
        make \
        ca-certificates \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

# --- Python / scons --------------------------------------------------------
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        scons \
        python3 \
        python3-dev \
        python3-venv \
        python3-pip \
        python-is-python3 \
    && rm -rf /var/lib/apt/lists/*

# --- C/C++ library deps pulled by XS-GEM5's scons ------------------------
# - libboost-all-dev: headers + random + system + thread
#   (arch_db / dev/virtio / cpu/simple need uuid+random).
# - libsqlite3-dev: arch_db.hh includes sqlite3.h (required
#   by XS-GEM5 but NOT upstream vanilla gem5, which is why
#   the stock gem5-build image omits it).
# - libhdf5-dev + libpng-dev: optional features; scons warns
#   when absent. Including them avoids a future silent
#   regression.
# - libgoogle-perftools-dev: tcmalloc for release perf.
# - libprotobuf-dev + protobuf-compiler: gem5 protocol-
#   buffer-based messaging layer (occasional enable-by-
#   default).
# - zlib1g-dev, libzstd-dev: legacy / statstream compressors.
# - libelf-dev: gem5's elf loader.
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        libboost-all-dev \
        libsqlite3-dev \
        libhdf5-dev \
        libpng-dev \
        libgoogle-perftools-dev \
        libprotobuf-dev \
        protobuf-compiler \
        zlib1g-dev \
        libzstd-dev \
        libelf-dev \
    && rm -rf /var/lib/apt/lists/*

# --- Build-friendly defaults ----------------------------------------------
# Matches the behavior of the round-53 helper: scons runs in
# /workspace and writes into build/RISCV/ there. `--user` at
# `docker run` time is the caller's responsibility (the
# helper handles uid/gid pass-through and final chown).
WORKDIR /workspace

# Reasonable default; helper overrides via `docker run ... cmd`.
CMD ["scons", "--help"]
