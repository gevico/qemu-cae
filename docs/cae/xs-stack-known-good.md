# XS-GEM5 Known-Good Boot CLI

This document records the verified XS-GEM5 (pinned
`~/xiangshan-gem5` at `e584f268a6`) invocation that boots a
CAE-produced `*.xs.bin` under `--disable-difftest` without
requiring a NEMU reference `.so`. It is the operator-side
precondition for AC-K-5 live calibration (`t-spike-xsgem5`).

## Pin

```
tools/xiangshan-gem5-version.txt  (SHA e584f268a6, matches
                                   the checkout verified here)
```

## Plan-compliant staged build area

Per `docs/superpowers/plans/2026-04-18-cae-phase-2-revision-
kmhv3-plan.md:217`, **`~/xiangshan-gem5` is read-only**;
build artefacts belong under `<repo>/build/xs-gem5-build`.
`tests/cae-difftest/scripts/run-xs-gem5.py` prefers the
staged path `<repo>/build/xs-gem5-build/gem5.opt` and falls
back to `<xs-root>/build/RISCV/gem5.opt` only when the
staged binary is absent.

### Recommended flow: `tools/build-xs-gem5-staged.sh`
(Docker-backed, round-54)

```sh
# From <repo>:
bash tools/build-xs-gem5-staged.sh
```

The helper runs scons INSIDE the `cae/xs-gem5-build:latest`
Docker image, built from the in-repo Dockerfile at
`tools/xs-gem5-build.Dockerfile`. The image ships with
scons + boost + g++ 13 + libsqlite3-dev + libhdf5-dev +
libpng-dev + libgoogle-perftools-dev + libprotobuf-dev +
zlib1g-dev + libzstd-dev + libelf-dev — every dep XS-GEM5's
scons pulls in. Host-side `sudo apt install libboost-all-dev`
is not required; nothing except docker itself is installed
on the host. The container mounts ONLY the staged repo-local
source copy; `~/xiangshan-gem5` is never mounted and the
read-only invariant is preserved by the pre/post sha256 gate.

If the default image is absent the helper builds it
automatically from the in-repo Dockerfile before running
scons; manual one-time setup is therefore optional:

```sh
docker build -t cae/xs-gem5-build:latest \
             -f tools/xs-gem5-build.Dockerfile \
             tools/
```

Set `XS_GEM5_BUILD_IMAGE=<image>` to override the image
name (e.g. to reuse an existing `gem5-build:local` from a
legacy setup, or to pin `ghcr.io/gem5/ubuntu-24.04_all-
dependencies:v24-0`). Overrides disable the auto-build
fallback, so the chosen image must exist on the host.

The helper:

1. Verifies the external checkout's SHA matches the pinned
   one via `tools/check-xiangshan-gem5-version.sh`.
2. Snapshots the external tree's content hash pre-build.
   Round-54 scope (matching the rsync exclude list): every
   tracked file under `~/xiangshan-gem5/`, excluding
   `build/` and `.git/`. So `configs`, `src`, `SConstruct`,
   `site_scons`, `build_opts`, `build_tools`, `ext`,
   `resource`, `include`, `system`, `tests`, `util`,
   `SConsopts`, and every root-level dotfile are all
   included in the hash.
3. Rsyncs the source (excluding `build/` and `.git/`) into
   `<repo>/build/xs-gem5-build/src/` so the build runs on an
   in-repo copy.
4. Runs `scons build/RISCV/gem5.opt -j$(nproc)` inside the
   staged source copy. Output lands at
   `<repo>/build/xs-gem5-build/src/build/RISCV/gem5.opt`.
5. Symlinks `<repo>/build/xs-gem5-build/gem5.opt` → that
   path so `run-xs-gem5.py` picks it up.
6. Re-checks the external tree's hash; if it changed, the
   helper exits non-zero — a plan violation. The pre/post
   scope is identical, so a clean run is byte-identical
   across the full non-build/non-git surface.

`~/xiangshan-gem5/` is not mutated at any step. The helper
is idempotent: a re-run rsyncs incrementally, so only
source-hash-changed files are recompiled. Wall-clock on
cold runs: ~20-40 minutes on modern machines.

### Fallback: symlink an existing external build

If `gem5.opt` has already been built inside the external
checkout from an earlier (pre-round-53) workflow:

```sh
ln -s ~/xiangshan-gem5/build/RISCV/gem5.opt \
      <repo>/build/xs-gem5-build/gem5.opt
```

This satisfies the staged-path contract without a rebuild.
The external tree stays read-only (the symlink itself lives
inside the repo). Preferred only as a transition aid;
round-53+ workflows should use the helper above.

## Canonical CLI

```sh
cd <repo>
./build/xs-gem5-build/gem5.opt \
    ~/xiangshan-gem5/configs/example/kmhv3.py \
    --raw-cpt \
    --generic-rv-cpt=<path>/<bench>.xs.bin \
    --disable-difftest
```

`run-xs-gem5.py` constructs this CLI automatically —
operators normally don't invoke gem5 directly; the runner
reads the paired YAML and plumbs the paths.

### Notes on each flag

- `kmhv3.py` — recommended XS-GEM5 entrypoint per
  `~/xiangshan-gem5/README.md`; encapsulates the KMH-V3
  parameter set (fetchW=32 / decodeW=8 / renameW=8 /
  commitW=8 / ROB=352 / LQ=72 / SQ=56 / numPhysIntRegs=224
  / numPhysFloatRegs=256 / SbufferEntries=16) that AC-K-5
  calibrates against. Sourced from the read-only external
  checkout (configs are source, not build artefacts).
- `--raw-cpt` — accepts a raw binary in place of a gem5
  checkpoint; no GCPT restorer needed.
- `--generic-rv-cpt=<path>` — the binary to boot.
  CAE's ELF→raw pipeline produces
  `tests/cae-difftest/benchmarks/build/<name>.xs.bin` in
  the expected format.
- `--disable-difftest` — registered in
  `configs/common/Options.py:647` as
  `action="store_false", dest="enable_difftest"`.
  `cliff.py` additionally forces `args.enable_difftest =
  False` after `parse_args()`, and `auto_xiangshan.py`
  forces the same inside `xiangshan_system_init()`. The
  runtime still prints `warn("Difftest is disabled\n")`
  from `src/cpu/base.cc:242`; this is informational, not
  a correctness gate.

## Stats output path

`m5out/stats.txt` by default. `gem5.opt` defaults
`--outdir` to `m5out` (`src/python/m5/main.py:81`) and
`--stats-file` to `stats.txt` (`src/python/m5/main.py:113`).
`run-xs-suite.sh` passes per-benchmark `--outdir` to keep
reports segregated.

## Environment variables

For a clean no-difftest run, the following MUST remain
UNSET (and no `--difftest-ref-so` flag must be passed):

- `GCBV_REF_SO`
- `GCBV_MULTI_CORE_REF_SO`
- `GCBH_REF_SO`
- `NEMU_HOME`

For the `--raw-cpt` boot path specifically, the
`GCB_RESTORER` / `GCBV_RESTORER` / `GCBH_RESTORER`
variables are also unnecessary.

## External build dependencies

**Docker-backed build (round 54+)**: the only host-side
dependency is docker itself plus the
`cae/xs-gem5-build:latest` image (or a compatible
override). All C++ build deps (`libboost-all-dev`,
`libsqlite3-dev`, `libhdf5-dev`, `libpng-dev`,
`libpython3-dev`, `libgoogle-perftools-dev`,
`libprotobuf-dev`, ...) live inside the container. The
host's package manager is not touched.

The canonical image is defined by
`tools/xs-gem5-build.Dockerfile` in this repo. Adding a
future XS-GEM5 dep is a one-line edit to that file plus an
image rebuild — no ad-hoc in-container apt-install paths.

Build the image (`tools/build-xs-gem5-staged.sh` also does
this automatically on first run):

```sh
docker build -t cae/xs-gem5-build:latest \
             -f tools/xs-gem5-build.Dockerfile \
             tools/
```

Legacy `gem5-build:local` (built from the XS-GEM5 upstream
`util/dockerfiles/build-env/` Dockerfile) also works but
lacks `libsqlite3-dev`; prefer the in-repo Dockerfile.

**Legacy host-side build** (fallback; only needed if
docker is unavailable). Install on Debian / Ubuntu:

```sh
sudo apt install -y libboost-all-dev libhdf5-dev libpng-dev \
                    libpython3-dev libgoogle-perftools-dev
```

Then skip the docker layer and invoke scons directly
inside `<repo>/build/xs-gem5-build/src/`. Not the
recommended flow — the docker image gives reproducible
dependencies across dev hosts.

## Status on this host (round 55)

- `~/xiangshan-gem5` checkout: present, pin `e584f268a6`,
  read-only (plan.md:217); pre/post sha256 verified
  identical across the round-54 build.
- `<repo>/build/xs-gem5-build/gem5.opt`: **BUILT** (910 MB,
  built via `tools/build-xs-gem5-staged.sh` against
  `gem5-build:local`). Round-55 recorded the exact
  image+digest in `<repo>/build/xs-gem5-build/image.txt` so
  `run-xs-gem5.py` pins the same image for run-time docker
  wrap.
- `<repo>/build/xs-gem5-build/image.txt`: new round-55 file
  with `image: <tag>` + `digest: sha256:<id>` — the runner
  reads this and prefers the recorded image over ambient
  `$XS_GEM5_BUILD_IMAGE`.
- `run-xs-gem5.py`: docker-wraps the gem5.opt invocation
  inside the build image (arch host vs Ubuntu container ABI
  mismatch; `CAE_XS_GEM5_NO_DOCKER=1` opts out for matching
  dev hosts).
- `run-xs-suite.sh xs-1c-kmhv3 --benchmark alu`: advances
  into `**** REAL SIMULATION ****` on this host as of
  round 54. Round-55 diagnostic confirmed the remaining
  blocker: `MicroTAGE: blockSize=0` pre-stats panic
  (`Commit stage is stucked for more than 40,000 cycles!`)
  reproduces on both `alu.xs.bin` and `coremark-1c.xs.bin`,
  ruling out CAE-side binary-layout concerns and narrowing
  the failure to a predictor-param-proxy bug in the pinned
  XS-GEM5 tree. Fix lane scheduled for round 56:
  `tools/xs-gem5-microtage-blocksize.patch` applied to the
  staged source after rsync.
- `cae/xs-gem5-build:latest`: in-repo Dockerfile
  (`tools/xs-gem5-build.Dockerfile`) available and wired
  into `build-xs-gem5-staged.sh`'s auto-build-on-first-run
  path; has NOT yet been exercised on this host (the
  round-54 build used the pre-existing `gem5-build:local`
  image plus the now-removed in-container apt-install
  workaround).

Round-55 advances: Q1 image-provenance closed; Q2 (this
section) refreshed. Remaining AC-K-5 blocker: round-56
microtage blockSize patch + rebuild + sweep.
