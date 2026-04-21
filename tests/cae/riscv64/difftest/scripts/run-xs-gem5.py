#!/usr/bin/env python3
"""Run a difftest benchmark under XiangShan GEM5 (KMH-V3) and emit
normalized stats for the KMH-V3 calibration track.

Flow:

  1. Load + validate the paired YAML via _pairedyaml.
  2. Enforce the XS-GEM5 commit pin (tools/cae/check-xiangshan-gem5-version.sh).
  3. Verify the benchmark's xs raw-binary sha256 against MANIFEST.json;
     the `xs` variant (<bench>.xs.bin) is what gets handed to
     kmhv3.py via --raw-cpt --generic-rv-cpt=<path>.
  4. Launch gem5.opt with `--disable-difftest` pre-prepended to the
     options (AC-K-9). This prevents xiangshan_system_init()'s
     default-on difftest path from pulling in NEMU .so dependencies
     that this track intentionally does NOT need.
  5. Parse m5out/stats.txt for simInsts / numCycles / simTicks etc.
  6. Emit a JSON report that matches the run-cae.py / run-gem5.py
     schema so diff.py can consume both N-side.

Assumptions:

  - The XS-GEM5 `build/RISCV/gem5.opt` already exists in the pinned
    checkout at ~/xiangshan-gem5 (or $XIANGSHAN_GEM5_ROOT).
  - The operator leaves GCBV_REF_SO / GCBV_MULTI_CORE_REF_SO /
    GCBH_REF_SO / NEMU_HOME unset. We do not clear them here
    because the user's shell may have other legitimate uses; we
    only assert the --disable-difftest path is taken.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _pairedyaml as p  # noqa: E402


REPO_ROOT = p.REPO_ROOT
DEFAULT_XS_GEM5_ROOT = Path(os.environ.get(
    "XIANGSHAN_GEM5_ROOT", str(Path.home() / "xiangshan-gem5")))
CHECK_XS_GEM5_VERSION = REPO_ROOT / "tools" / "cae" / "check-xiangshan-gem5-version.sh"

# Round 52 AC-K-5: plan.md:217 forbids writing into the external
# `~/xiangshan-gem5` checkout. The staged repo-local build area
# lives here; runners prefer this path for `gem5.opt`, falling
# back to the legacy `<xs-root>/build/RISCV/gem5.opt` only when
# the staged binary is absent. Operators build / symlink the
# binary into this directory per `docs/cae/xs-stack-known-
# good.md`.
STAGED_XS_GEM5_DIR = REPO_ROOT / "build" / "xs-gem5-build"
STAGED_XS_GEM5_BIN = STAGED_XS_GEM5_DIR / "gem5.opt"

# Round 54 AC-K-5: gem5.opt is built inside the Ubuntu 24.04
# based `cae/xs-gem5-build:latest` image and therefore dynamic-
# links against that image's libpython3.12 / libhdf5_serial.103
# / libprotobuf.32 / ... . Those exact sonames don't exist on
# most host distros (Arch, Fedora, RHEL), so we run gem5.opt
# inside the same image the binary was built against. The
# container mounts the repo and the XS-GEM5 source tree; all
# paths in the CLI remain valid because we mount them under
# their absolute host paths. `~/xiangshan-gem5` is still
# mounted READ-ONLY so the plan.md:217 contract holds inside
# the container too.
#
# Set `CAE_XS_GEM5_NO_DOCKER=1` to skip the wrap (only useful
# when gem5.opt's ABI happens to match the host — e.g. a dev
# host that IS Ubuntu 24.04).
XS_GEM5_BUILD_IMAGE = os.environ.get(
    "XS_GEM5_BUILD_IMAGE", "cae/xs-gem5-build:latest")

# Round 55 Q1: `tools/cae/build-xs-gem5-staged.sh` writes this
# file next to the staged gem5.opt with the exact image
# tag+digest used for the build. The runner prefers the
# recorded image over ambient `$XS_GEM5_BUILD_IMAGE` so a
# binary built under one image is executed under that SAME
# image — closes the round-54 "same image as build"
# provenance gap.
STAGED_XS_GEM5_IMAGE_META = STAGED_XS_GEM5_DIR / "image.txt"


def _assert_microtage_blocksize_32(outdir: Path) -> None:
    """Parse m5out/config.ini and require
    `[system.cpu.branchPred.microtage] blockSize = 32`.
    Fail-close on any other value (including the previously-
    observed 64 from a Parent.predictWidth proxy leak, or 0 from
    the pre-patch shadowing bug). Missing file is also a fail-
    close so a partial run cannot silently pass the gate.
    """
    cfg = outdir / "config.ini"
    if not cfg.is_file():
        raise SystemExit(
            "run-xs-gem5: config.ini missing from "
            f"{outdir}; microtage blockSize cannot be validated"
        )

    in_section = False
    observed: str | None = None
    for raw_line in cfg.read_text().splitlines():
        line = raw_line.strip()
        if line.startswith("[") and line.endswith("]"):
            in_section = (
                line == "[system.cpu.branchPred.microtage]"
            )
            continue
        if in_section and line.startswith("blockSize"):
            if "=" in line:
                observed = line.split("=", 1)[1].strip()
            break
    if observed is None:
        raise SystemExit(
            "run-xs-gem5: config.ini has no blockSize entry "
            "under [system.cpu.branchPred.microtage]; expected 32 "
            "(KMH-V3 tage index uses 32-byte aligned blocks)"
        )
    if observed != "32":
        raise SystemExit(
            "run-xs-gem5: config-mismatch — "
            "[system.cpu.branchPred.microtage] blockSize="
            f"{observed!r}, expected 32. The kmhv3.py override did "
            "not land (or the MicroTAGE patch regressed); refusing "
            "to treat this run as a valid KMH-V3 attempt."
        )


def _recorded_build_image() -> str | None:
    """Return the build image recorded by the staged helper,
    preferring the image@digest form when it is usable on this
    host. Returns None if the metadata file is absent or
    malformed.

    Digest-pinning caveat: `docker run image@sha256:...` forces
    docker to verify the digest against a registry, even when a
    local image with that exact digest is already present. For
    a local-only build image (e.g. `gem5-build:local` produced
    by the staged helper and never pushed anywhere), the
    registry check fails with `pull access denied`. We therefore
    only return the `@digest` form when the local image's
    current digest matches the recorded one — if it matches,
    the plain tag is already a safe reference to the pinned
    content. If it does NOT match (the operator rebuilt or
    retagged), we still return `@digest` so docker explicitly
    refuses rather than silently running against a different
    image.
    """
    if not STAGED_XS_GEM5_IMAGE_META.is_file():
        return None
    image = None
    digest = None
    for line in STAGED_XS_GEM5_IMAGE_META.read_text().splitlines():
        line = line.strip()
        if line.startswith("image:"):
            image = line.split(":", 1)[1].strip()
        elif line.startswith("digest:"):
            digest = line.split(":", 1)[1].strip()
    if image is None:
        return None
    if digest and digest.startswith("sha256:"):
        # Cross-check the recorded digest against the image's
        # current local digest. If they match, the plain tag is
        # already a content-addressable reference to the pinned
        # layer stack (and avoids the registry round-trip that
        # trips on local-only images). Mismatch falls through
        # to the digest-pinned form so docker refuses to run
        # against an inadvertently-replaced image.
        try:
            local_digest = subprocess.run(
                ["docker", "image", "inspect", "-f", "{{.Id}}", image],
                check=False, capture_output=True, text=True,
            ).stdout.strip()
        except (FileNotFoundError, subprocess.SubprocessError):
            local_digest = ""
        if local_digest == digest:
            return image
        return f"{image}@{digest}"
    return image


def _host_can_run_gem5(gem5: Path) -> bool:
    """Return True if every shared-lib dep of gem5.opt resolves
    on the host. Used to skip the docker wrap on hosts whose
    ABI matches the build image.
    """
    try:
        out = subprocess.run(
            ["ldd", str(gem5)],
            check=True, capture_output=True, text=True
        ).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        return False
    return "not found" not in out


def _docker_wrap_cli(cli: list[str], xs_root: Path,
                     outdir: Path, cpt_path: Path) -> list[str]:
    """Wrap a host-side gem5 CLI in `docker run ...` against
    the canonical build image. See module-level comment for
    rationale.

    Round 55 Q1: prefer the image recorded in
    `build/xs-gem5-build/image.txt` (the exact image+digest
    the staged binary was built with) over the ambient env
    fallback. This closes the round-54 "same image as
    build" provenance gap — a binary built under
    `gem5-build:local` can no longer silently be run under
    `cae/xs-gem5-build:latest`.
    """
    host_uid = str(os.geteuid())
    host_gid = str(os.getegid())
    mounts = [
        # Repo tree (RW for outdir + RO for the staged gem5.opt).
        ("-v", f"{REPO_ROOT}:{REPO_ROOT}:rw"),
        # XS-GEM5 pinned tree is READ-ONLY per plan.md:217.
        ("-v", f"{xs_root}:{xs_root}:ro"),
    ]
    # Outdir and cpt may live outside the repo for ad-hoc
    # invocations; mount their parents so gem5.opt sees them.
    extra_dirs = {outdir.resolve()}
    cpt_parent = cpt_path.resolve().parent
    extra_dirs.add(cpt_parent)
    for d in extra_dirs:
        if REPO_ROOT in d.parents or d == REPO_ROOT:
            continue
        if xs_root in d.parents or d == xs_root:
            continue
        mode = "rw" if d == outdir.resolve() else "ro"
        mounts.append(("-v", f"{d}:{d}:{mode}"))
    flat_mounts = [v for pair in mounts for v in pair]
    image = _recorded_build_image() or XS_GEM5_BUILD_IMAGE
    return [
        "docker", "run", "--rm",
        "--user", f"{host_uid}:{host_gid}",
        *flat_mounts,
        "-w", str(xs_root),
        image,
        *cli,
    ]


def _resolve_gem5_opt(xs_root: Path) -> Path:
    """Resolve the gem5.opt binary path for this invocation.

    Prefers `<repo>/build/xs-gem5-build/gem5.opt` (plan.md:217
    compliant staged area); falls back to
    `<xs-root>/build/RISCV/gem5.opt` only if the staged binary
    is absent. Raises FileNotFoundError if neither exists.
    """
    if STAGED_XS_GEM5_BIN.is_file():
        return STAGED_XS_GEM5_BIN
    legacy = xs_root / "build" / "RISCV" / "gem5.opt"
    if legacy.is_file():
        return legacy
    raise FileNotFoundError(
        f"run-xs-gem5: gem5.opt missing from both staged path "
        f"{STAGED_XS_GEM5_BIN} and legacy path {legacy}; build "
        f"xs-gem5 into the staged area or symlink the external "
        f"build (see docs/cae/xs-stack-known-good.md)"
    )

# Stats names differ by CPU-type + cache-protocol family. The
# candidate list below covers the XiangShan GEM5 build's customized
# DerivO3CPU + Ruby protocols plus the vanilla gem5 fallbacks we
# already use in run-gem5.py.
_STATS_CANDIDATES = {
    "total_insns":     ["sim_insts", "simInsts"],
    "total_cycles":    ["system.cpu.numCycles",
                        "system.cpu.cycles",
                        "system.cpu0.numCycles"],
    "sim_ticks":       ["sim_ticks", "simTicks"],
    "host_seconds":    ["host_seconds", "hostSeconds"],
    "mispredict_rate": ["system.cpu.branchPred.condIncorrect",
                        "system.cpu.branchPred.incorrect",
                        "system.cpu0.branchPred.condIncorrect"],
    "branch_count":    ["system.cpu.branchPred.condPredicted",
                        "system.cpu.branchPred.predictedTaken",
                        "system.cpu0.branchPred.condPredicted"],
    "l1d_misses":      ["system.cpu.dcache.overall_misses::total",
                        "system.cpu.dcache.demand_misses::total",
                        "system.l1d.misses",
                        "system.cpu0.dcache.overall_misses::total"],
    "l1d_accesses":    ["system.cpu.dcache.overall_accesses::total",
                        "system.cpu.dcache.demand_accesses::total",
                        "system.l1d.accesses",
                        "system.cpu0.dcache.overall_accesses::total"],
    "load_latency":    ["system.cpu.dcache.overall_avg_miss_latency::total",
                        "system.cpu.dcache.demand_avg_miss_latency::total",
                        "system.cpu0.dcache.overall_avg_miss_latency::total"],
}

_STATS_RE = re.compile(r"^(?P<key>\S+)\s+(?P<val>\S+)")


def _stats_any(stats: dict[str, float], keys: list[str],
               as_float: bool = False):
    for k in keys:
        if k in stats:
            v = stats[k]
            return float(v) if as_float else int(v)
    return 0.0 if as_float else 0


def _parse_stats(path: Path) -> dict[str, float]:
    if not path.is_file():
        raise FileNotFoundError(f"xs-gem5 stats file missing: {path}")
    out: dict[str, float] = {}
    for line in path.read_text().splitlines():
        line = line.rstrip()
        if not line or line.startswith("-----"):
            continue
        m = _STATS_RE.match(line)
        if not m:
            continue
        key = m.group("key")
        val = m.group("val")
        try:
            out[key] = float(val)
        except ValueError:
            continue
    return out


def _resolve_xs_binary(cfg: p.PairedConfig) -> Path:
    bench_name = cfg.benchmark["name"]
    entry = p.manifest_entry(bench_name)
    variants = entry.get("variants", {})
    if "xs" not in variants:
        raise ValueError(
            f"run-xs-gem5: benchmark {bench_name!r} has no 'xs' variant "
            f"in MANIFEST.json — build it via `make -C "
            f"tests/cae/riscv64/difftest/benchmarks/src xs-bins` first."
        )
    xs_info = variants["xs"]
    elf = (p.BENCH_DIR / xs_info["binary"]).resolve()
    if not elf.is_file():
        raise FileNotFoundError(
            f"run-xs-gem5: xs raw binary missing: {elf}; run "
            f"`make -C tests/cae/riscv64/difftest/benchmarks/src xs-bins`"
        )
    actual = p.sha256_of(elf)
    if actual != xs_info["sha256"]:
        raise ValueError(
            f"run-xs-gem5: xs binary sha256 drift for {bench_name}:\n"
            f"  file     : {elf}\n"
            f"  expected : {xs_info['sha256']}\n"
            f"  actual   : {actual}\n"
            f"Rebuild the binary or update MANIFEST.json via RFC."
        )
    return elf


def _enforce_pin(xs_root: Path) -> None:
    if not CHECK_XS_GEM5_VERSION.is_file():
        raise FileNotFoundError(
            f"run-xs-gem5: check script missing: {CHECK_XS_GEM5_VERSION}"
        )
    rc = subprocess.run(
        ["bash", str(CHECK_XS_GEM5_VERSION), str(xs_root)],
    ).returncode
    if rc != 0:
        raise SystemExit(rc)


def _enforce_launch_contract(cfg: p.PairedConfig) -> None:
    """AC-K-9 positive test: run-xs-gem5.py always passes
    --disable-difftest. A YAML that disables this toggle fails
    upfront before any subprocess spawns."""
    xs_runtime = (cfg.xs_gem5_only.get("runtime") or {})
    disable = xs_runtime.get("disable_difftest", True)
    if not disable:
        raise SystemExit(
            "run-xs-gem5: xs_gem5_only.runtime.disable_difftest must be "
            "true in this track (AC-K-9); refusing to launch because "
            "the timing-only path is the contract."
        )


def _build_cli(xs_root: Path, xs_binary: Path,
               cfg: p.PairedConfig) -> list[str]:
    # Round 52 AC-K-5: use the staged repo-local gem5.opt if
    # present; fall back to the legacy external path only if
    # the staged binary is absent. kmhv3.py stays sourced from
    # the read-only xs_root (it is source, not a build artefact).
    gem5 = _resolve_gem5_opt(xs_root)
    # CAE_XS_GEM5_CONFIG_SCRIPT: override the gem5 Python config
    # script used for the timing run. Default is `kmhv3.py` (the
    # plan-mandated KMH-V3 calibration target). Valid override
    # values: `idealkmhv3.py` (the upstream ideal-core variant
    # used as a Milestone-E artifact-production escape hatch
    # when the pinned KMH-V3 O3 pipeline livelocks upstream;
    # NOT a valid calibration source — see
    # docs/cae/m5-calibration-log.md for the documented
    # trade-off). Files are resolved relative to `xs_root /
    # configs / example /`.
    script_name = os.environ.get(
        "CAE_XS_GEM5_CONFIG_SCRIPT", "kmhv3.py"
    )
    kmhv3 = xs_root / "configs" / "example" / script_name
    if not kmhv3.is_file():
        raise FileNotFoundError(
            f"run-xs-gem5: config script missing at {kmhv3}"
        )
    cli = [str(gem5),
           # gem5 prepends its --outdir / --stats-file defaults; let
           # the caller override via --outdir.
           str(kmhv3),
           "--disable-difftest",
           "--raw-cpt",
           f"--generic-rv-cpt={xs_binary}",
           # Round 54 AC-K-5: kmhv3.py defaults `--mem-type=DRAMsim3`
           # which is an external submodule not built into the
           # staged `gem5.opt` (scons built without the
           # `ext/dramsim3/DRAMsim3/` extra). DDR3_1600_8x8 is
           # gem5's stock DDR3 model, always compiled in, and
           # produces a representative memory-latency profile for
           # the KMH-V3 IPC calibration gate. If the operator ever
           # rebuilds gem5 with DRAMsim3 support (clone the
           # submodule + scons EXTRAS=ext/dramsim3/DRAMsim3), drop
           # this override or expose it as a YAML knob.
           "--mem-type=DDR3_1600_8x8"]
    return cli


def _override_benchmark(cfg: p.PairedConfig, benchmark: str) -> None:
    """Mutate cfg.benchmark['name'] for the rest of this invocation.

    Used by run-xs-suite.sh when iterating over the config's suite
    benchmark list: the XS side must resolve the binary from the
    overridden name, not the config's default (`alu`). Preserves
    the original name under `_original_name` so
    `_pairedyaml.verify_benchmark`'s cross-check semantics remain
    consistent with the in-order track's existing override path.
    """
    if "name" not in cfg.benchmark:
        raise SystemExit("run-xs-gem5: common.benchmark.name is required")
    cfg.benchmark["_original_name"] = cfg.benchmark["name"]
    cfg.benchmark["name"] = benchmark


def run(cfg_path: str, report_path: Path | None,
        outdir: Path, xs_root: Path,
        benchmark_override: str | None = None,
        first_pc_debug: bool = False) -> dict:
    cfg = p.load_config(cfg_path)
    if benchmark_override is not None:
        _override_benchmark(cfg, benchmark_override)
    _enforce_launch_contract(cfg)
    _enforce_pin(xs_root)
    xs_binary = _resolve_xs_binary(cfg)

    outdir.mkdir(parents=True, exist_ok=True)
    stats_txt = outdir / "stats.txt"
    # Clean previous stats.txt to guarantee the one we parse is
    # from this run (kmhv3.py appends by default).
    if stats_txt.is_file():
        stats_txt.unlink()

    cli = _build_cli(xs_root, xs_binary, cfg)
    # Prepend gem5's own outdir flag so stats.txt lands where we
    # expect it; see xs-gem5 main.py defaults
    # (DEFAULT_OUTDIR="m5out", DEFAULT_STATS_FILE="stats.txt").
    cli = [cli[0], "--outdir", str(outdir)] + cli[1:]

    # BS-23 AC-K-10: --first-pc-debug enables gem5's Exec debug
    # flag and redirects it to a small log we can tail for the
    # first committed PC. gem5's `--debug-flags=Exec` prints one
    # line per retired instruction; at tier-1 benchmark sizes
    # (~1M insns) this is too large, so we redirect through
    # `--debug-start=0 --debug-end=10` to restrict to the first
    # 10 ticks. The parsed first line's pc is the observed
    # runtime boot PC.
    #
    # Default off because a real calibration run doesn't need it
    # — it's only needed for the AC-K-10 preflight. `null`
    # first_pc is still the default (honest SKIP) when this flag
    # isn't passed.
    #
    # Round 54 AC-K-5: `--debug-flags` / `--debug-file` /
    # `--debug-start` / `--debug-end` are gem5-generic options,
    # not kmhv3.py script options — they MUST be inserted
    # BEFORE the config-script path or kmhv3's argparse rejects
    # them. cli[0] is gem5.opt and cli[1..3] are
    # `--outdir <dir>`; the script path is at cli[3], so insert
    # at index 3.
    exec_log: Path | None = None
    if first_pc_debug:
        exec_log = outdir / "exec-head.log"
        # Round 10: widen the debug-end window to capture the
        # FIRST architectural retire instead of only the first
        # 10 ticks of simulated time. Pre-R10 value of 10 ticks
        # was far too small — at the kmhv3 2 GHz clock that's
        # 0.005 cycles, before any instruction has even reached
        # fetch. Observed first-commit cycle on `mem-stream` is
        # ~100-500 (tens to hundreds of thousands of ticks), so
        # 2 000 000 ticks = ~2 000 cycles at 2 GHz leaves a
        # comfortable margin. The parser only reads the first
        # `system.cpu ... 0x<pc>` line, so a wider window has
        # no correctness cost — only a marginal log-size
        # increase bounded by the time until first commit.
        debug_args = [
            "--debug-flags=Exec",
            f"--debug-file={exec_log.name}",
            "--debug-start=0",
            "--debug-end=2000000",
        ]
        cli = cli[:3] + debug_args + cli[3:]

    # Round 54 AC-K-5: wrap gem5.opt in `docker run` against the
    # same image it was built in, unless the host ABI happens to
    # match or the operator explicitly opts out via
    # CAE_XS_GEM5_NO_DOCKER=1. See _docker_wrap_cli() docstring.
    gem5 = Path(cli[0])
    opt_out = os.environ.get("CAE_XS_GEM5_NO_DOCKER", "") == "1"
    if not opt_out and not _host_can_run_gem5(gem5):
        cli = _docker_wrap_cli(cli, xs_root, outdir, xs_binary)

    t0 = time.time()
    proc = subprocess.run(cli, cwd=str(xs_root))
    wallclock = time.time() - t0

    # Annotate the stats file with the chosen difftest mode (AC-K-9):
    # the first line is a comment so stats.txt remains greppable.
    if stats_txt.is_file():
        original = stats_txt.read_text()
        stats_txt.write_text(
            f"# xs-gem5 difftest_mode=disabled\n"
            f"# cae-qemu xs-gem5 driver: kmhv3.py invoked with "
            f"--disable-difftest, no NEMU .so required\n"
            + original)

    if proc.returncode != 0:
        # Even on a panic, config.ini has already been emitted by
        # the Python-side SimObject resolve pass. Enforce the
        # config-landing invariant for the KMH-V3 branch
        # predictor before reporting the generic gem5 failure, so
        # a silent MicroTAGE blockSize regression surfaces as a
        # specific config-mismatch error rather than hiding
        # behind a downstream commit-stuck panic.
        _assert_microtage_blocksize_32(outdir)
        raise SystemExit(
            f"run-xs-gem5: gem5.opt exited {proc.returncode}; see {outdir}"
        )

    # Happy-path callers go through the same config-landing gate
    # so the guard is never skipped when stats collection
    # succeeds.
    _assert_microtage_blocksize_32(outdir)

    stats = _parse_stats(stats_txt)
    total_insns = _stats_any(stats, _STATS_CANDIDATES["total_insns"])
    total_cycles = _stats_any(stats, _STATS_CANDIDATES["total_cycles"])
    mispreds = _stats_any(stats, _STATS_CANDIDATES["mispredict_rate"],
                          as_float=True)
    branches = _stats_any(stats, _STATS_CANDIDATES["branch_count"],
                          as_float=True)
    l1d_misses = _stats_any(stats, _STATS_CANDIDATES["l1d_misses"])
    l1d_accesses = _stats_any(stats, _STATS_CANDIDATES["l1d_accesses"])
    load_lat = _stats_any(stats, _STATS_CANDIDATES["load_latency"],
                          as_float=True)

    mispredict_rate = (mispreds / branches) if branches > 0 else 0.0
    l1d_mpki = ((l1d_misses / total_insns) * 1000.0
                if total_insns > 0 else 0.0)
    ipc = (total_insns / total_cycles) if total_cycles > 0 else 0.0

    # AC-K-10: first architectural PC.
    #
    # Round 6 set first_pc=null unconditionally (Codex rejected
    # round-5's manifest fallback). Round 9 (BS-23) adds a real
    # runtime source behind --first-pc-debug: when enabled, we
    # tail the gem5 Exec-debug log written to outdir/<file> and
    # parse the first `system.cpu ... 0x<hex>` line. gem5's
    # Exec record format is:
    #   <tick>: system.cpu: @<symbol>  <pc> @<frame>  ...
    # or
    #   <tick>: system.cpu: T0 : @<symbol> : <pc>      <asm>
    # depending on the protocol family. The generic regex
    # `\s0x([0-9a-f]+)\s` catches the first hex word after
    # "system.cpu" which is the committed pc.
    #
    # BS-26 (round 11) AC-K-10 fail-closed: when first_pc_debug
    # was requested, any parse failure must be visible to the
    # downstream 4-way preflight. We record `first_pc_error` as a
    # concrete diagnostic string. Readers that only consumed
    # `first_pc` are still correct (null = unknown); readers
    # gating on "XS runtime first-PC promised but missing" can
    # now distinguish "not requested" (error=None, pc=None) from
    # "requested and failed" (error=set, pc=None).
    first_pc: int | None = None
    first_pc_error: str | None = None
    if first_pc_debug:
        if exec_log is None:
            first_pc_error = ("--first-pc-debug was requested but no "
                              "Exec log path was prepared")
        elif not exec_log.is_file():
            first_pc_error = (f"--first-pc-debug requested but Exec log "
                              f"{exec_log.name} was not written by gem5")
        else:
            import re
            exec_re = re.compile(r"system\.cpu[^\n]*?\s0x([0-9a-fA-F]+)\s")
            for line in exec_log.read_text().splitlines():
                m = exec_re.search(line)
                if m:
                    first_pc = int(m.group(1), 16)
                    break
            if first_pc is None:
                first_pc_error = (f"--first-pc-debug requested but no "
                                  f"'system.cpu ... 0x<pc>' line parsed "
                                  f"from {exec_log.name}")

    # AC-K-3.1: expose the retired-insn count as a top-level field
    # so run-xs-suite.sh's count-alignment gate can compare CAE vs
    # XS-GEM5 vs NEMU within a ±1 retired-insn tolerance. Mirrors
    # aggregate.total_insns (same underlying stat); keeping a
    # dedicated field isolates the preflight contract from any
    # future aggregate-shape change.
    retired_insn_count = total_insns

    report = {
        "backend": "xs-gem5",
        "backend_variant": cfg.xs_gem5_cpu_type,
        "config_name": cfg.common["contract"]["config_name"],
        "benchmark": cfg.benchmark["name"],
        "clock_freq_hz": int(cfg.cpu.get("clock_freq_hz",
                                          1_000_000_000)),
        "num_cpus": int(cfg.cpu.get("core_count", 1)),
        "wallclock_seconds": wallclock,
        "difftest_mode": "disabled",
        "first_pc": first_pc,
        "first_pc_error": first_pc_error,
        "retired_insn_count": retired_insn_count,
        "aggregate": {
            "total_insns": total_insns,
            "total_cycles": total_cycles,
            "ipc": ipc,
            "mispredict_rate": mispreds / branches if branches > 0 else 0.0,
            "l1d_mpki": l1d_mpki,
            "avg_load_latency": load_lat,
        },
        "raw_stats_path": str(stats_txt),
    }
    if report_path is not None:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2,
                                          sort_keys=False) + "\n")
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", required=True,
                    help="Paired YAML name or path (e.g. xs-1c-functional)")
    ap.add_argument("--output", type=Path,
                    help="Write the normalized JSON report here; "
                         "default: stdout")
    ap.add_argument("--outdir", type=Path,
                    default=REPO_ROOT / "tests" / "cae" / "riscv64" / "difftest" /
                            "reports" / "xs-gem5-m5out",
                    help="gem5 outdir; stats.txt is written here")
    ap.add_argument("--xs-root", type=Path,
                    default=DEFAULT_XS_GEM5_ROOT,
                    help="Path to ~/xiangshan-gem5")
    ap.add_argument("--benchmark",
                    help="Override the config's common.benchmark.name. "
                         "Used by run-xs-suite.sh to iterate over the "
                         "config's suite benchmark list — the CAE side "
                         "already has a --benchmark override, and the "
                         "XS side must match it so diff.py does not "
                         "trip its benchmark-mismatch guard.")
    ap.add_argument("--first-pc-debug", action="store_true",
                    help="BS-23 AC-K-10: enable gem5's Exec debug flag "
                         "restricted to the first 10 ticks and parse "
                         "the first committed PC out of the resulting "
                         "log. When the flag is absent, `first_pc` in "
                         "the JSON report stays null and the suite "
                         "preflight treats the XS column as SKIP.")
    args = ap.parse_args()

    report = run(args.config, args.output, args.outdir, args.xs_root,
                 args.benchmark, first_pc_debug=args.first_pc_debug)
    if args.output is None:
        print(json.dumps(report, indent=2, sort_keys=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
