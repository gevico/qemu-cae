#!/usr/bin/env python3
"""Run a difftest benchmark under QEMU + CAE and emit normalized stats.

Reads a paired YAML (see _pairedyaml.py for the schema), verifies the
benchmark binary against MANIFEST.json, launches the CAE-enabled
qemu-system-riscv64 with the benchmark as -bios, polls the sentinel
over QMP until the workload terminates, then reads per-CPU counters
via qom-get and writes a JSON report to stdout or --output.

Output shape (per the v1 schema contract):

    {
      "schema_version": 1,
      "config_name": "atomic-1c",
      "benchmark": "alu",
      "backend": "cae",
      "backend_variant": "cae-phase1-cpi1",
      "num_cpus": 1,
      "clock_freq_hz": 1000000000,
      "sample_stats_at": "ebreak",
      "cpus": [{"cpu_index": 0, "cycles": ..., "insns": ..., ...}],
      "aggregate": {"total_cycles": ..., "total_insns": ..., "ipc": ...},
      "wallclock_seconds": ...
    }

The caller (diff.py / run-all.sh) is responsible for pairing CAE
output with a gem5 run. This script never invokes gem5.
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
DEFAULT_BINARY = Path(os.environ.get("CAE_BUILD_DIR", str(REPO_ROOT / "build"))) / "qemu-system-riscv64"
SENTINEL_VALUE = 0xCAE0CAFE

# Round 7: directories whose modification should invalidate a
# previously-built `qemu-system-riscv64`. Round 6 discovered that
# `build-cae/qemu-system-riscv64` had been stale since Round 0 —
# every "live" CAE run from Rounds 0-5 had been executing a pre-
# substrate binary, and the suite runner never caught it. This
# guard compares the binary's mtime against the max mtime across
# the CAE source lanes and auto-rebuilds (or fails closed if
# rebuild is unavailable) before proceeding.
CAE_SOURCE_DIRS_FOR_FRESHNESS = (
    REPO_ROOT / "hw" / "cae",
    REPO_ROOT / "cae",
    REPO_ROOT / "include" / "cae",
    REPO_ROOT / "include" / "hw" / "cae",
    REPO_ROOT / "target" / "riscv" / "cae",
    REPO_ROOT / "accel" / "cae",
)


def _max_source_mtime(dirs):
    """Return the max mtime across all regular files under `dirs`.

    Missing directories are skipped silently; a missing tree simply
    drops out of the max calculation. Returns 0.0 if every tree is
    missing (unit-test-friendly).
    """
    latest = 0.0
    for d in dirs:
        if not d.is_dir():
            continue
        for entry in d.rglob("*"):
            try:
                if entry.is_file():
                    mtime = entry.stat().st_mtime
                    if mtime > latest:
                        latest = mtime
            except OSError:
                continue
    return latest


def _ensure_qemu_binary_fresh(qemu_bin: Path) -> None:
    """Fail-closed freshness guard for `qemu-system-riscv64`.

    If the binary does not exist or is older than any source file
    under `CAE_SOURCE_DIRS_FOR_FRESHNESS`, auto-run ninja to
    rebuild it. If ninja is unavailable or the rebuild fails, raise
    `SystemExit(2)` with a diagnostic matching the pre-existing
    "build build-cae first" message.

    Opt-out: set `CAE_SKIP_FRESHNESS_CHECK=1` in the environment.
    Intended only for situations where the caller guarantees a
    fresh binary by other means (e.g. a CI pipeline that just ran
    ninja).
    """
    if os.environ.get("CAE_SKIP_FRESHNESS_CHECK") == "1":
        return

    build_dir = qemu_bin.parent
    src_mtime = _max_source_mtime(CAE_SOURCE_DIRS_FOR_FRESHNESS)
    bin_mtime = qemu_bin.stat().st_mtime if qemu_bin.is_file() else 0.0

    if bin_mtime >= src_mtime and qemu_bin.is_file():
        return

    if not (build_dir / "build.ninja").is_file():
        print(
            f"run-cae.py: {qemu_bin} is stale (or missing) and "
            f"{build_dir / 'build.ninja'} is absent; cannot auto-"
            f"rebuild — configure build-cae first or set "
            f"CAE_SKIP_FRESHNESS_CHECK=1 if you are sure the binary "
            f"is fresh.",
            file=sys.stderr,
        )
        raise SystemExit(2)

    print(
        f"run-cae.py: {qemu_bin} stale (bin mtime={bin_mtime:.0f} < "
        f"src mtime={src_mtime:.0f}); running ninja -C {build_dir} "
        f"qemu-system-riscv64",
        file=sys.stderr,
    )
    try:
        subprocess.run(
            ["ninja", "-C", str(build_dir), "qemu-system-riscv64"],
            check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print(
            f"run-cae.py: ninja rebuild of {qemu_bin} failed: {exc}",
            file=sys.stderr,
        )
        raise SystemExit(2)

    if not qemu_bin.is_file():
        print(
            f"run-cae.py: ninja reported success but {qemu_bin} "
            f"still missing",
            file=sys.stderr,
        )
        raise SystemExit(2)


def _resolve_sentinel_addr(cfg: p.PairedConfig) -> int:
    vinfo = p.variant_entry(cfg.benchmark["name"], "qemu")
    return int(vinfo["sentinel_addr"], 0)


# Round 44 (directive step 3, BitLesson Corollary L): benchmark-
# local stimulus targets (e.g. `spec_marker`, `spec_result` in
# spec-mem-leak-check.S) cannot be hard-coded in harness YAMLs
# because the linker layout rolls whenever the benchmark
# rebuilds. Resolve `${sym:<name>}` tokens in
# `cae_only.spec_stimulus_program` against the benchmark's QEMU
# ELF symbol table before forwarding via qom-set. Unknown
# symbols fail the run loudly.
_SYM_TOKEN_RE = re.compile(r"\$\{sym:([A-Za-z_][A-Za-z0-9_]*)\}")


def _resolve_symbol_addr(elf: Path, name: str) -> int:
    objdump = "riscv64-linux-gnu-objdump"
    try:
        result = subprocess.run(
            [objdump, "-t", str(elf)],
            capture_output=True, text=True, check=True,
        )
    except FileNotFoundError as exc:
        raise RuntimeError(
            f"${{sym:{name}}} resolution needs '{objdump}' on PATH; "
            "install the riscv64-linux-gnu toolchain or pre-"
            "expand addresses in the YAML"
        ) from exc
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"{objdump} -t {elf} failed: {exc.stderr.strip()}"
        ) from exc
    suffix = f" {name}"
    for line in result.stdout.splitlines():
        if line.endswith(suffix):
            addr_str = line.split(None, 1)[0]
            try:
                return int(addr_str, 16)
            except ValueError:
                continue
    raise RuntimeError(
        f"symbol '{name}' not found in {elf} — referenced via "
        f"${{sym:{name}}} in spec_stimulus_program"
    )


def _expand_sym_tokens(program: str, elf: Path) -> str:
    def _sub(match: "re.Match[str]") -> str:
        addr = _resolve_symbol_addr(elf, match.group(1))
        return f"0x{addr:016x}"
    return _SYM_TOKEN_RE.sub(_sub, program)


# Map YAML cae_only fields -> accel property names + "value is mandatory?".
# Any leaf under cae_only that doesn't appear here is rejected to avoid
# silently launching a run with an ignored knob. Keep this list in sync
# with accel/cae/cae-all.c::cae_accel_instance_init.
_CAE_ACCEL_FIELD_MAP = {
    "cpu.cpu_model":                       "cpu-model",
    "cpu.latency_mul":                     "latency-mul",
    "cpu.latency_div":                     "latency-div",
    "cpu.latency_fpu":                     "latency-fpu",
    "cpu.overlap_permille":                "overlap-permille",
    "cpu.load_use_stall_cycles":           "load-use-stall-cycles",
    # Round 49 AC-K-5: OoO-kmhv3 width + regfile forwarders.
    # Applied to the cae-cpu-ooo instance in init_machine when
    # cpu-model=ooo-kmhv3 is selected; non-OoO cpu-models ignore.
    "cpu.rob_size":                        "ooo-rob-size",
    "cpu.lq_size":                         "ooo-lq-size",
    "cpu.sq_size":                         "ooo-sq-size",
    "cpu.issue_width":                     "ooo-issue-width",
    "cpu.commit_width":                    "ooo-commit-width",
    "cpu.rename_width":                    "ooo-rename-width",
    "cpu.num_phys_int_regs":               "ooo-num-phys-int-regs",
    "cpu.num_phys_float_regs":             "ooo-num-phys-float-regs",
    "cpu.issue_ports":                     "ooo-issue-ports",
    "cpu.virtual_issue_window":            "ooo-virtual-issue-window",
    "cpu.dependent_load_stall_cycles":     "ooo-dependent-load-stall-cycles",
    "bpred.bpred_model":                   "bpred-model",
    "bpred.local_history_bits":            "local-history-bits",
    "bpred.btb_entries":                   "btb-entries",
    "bpred.btb_assoc":                     "btb-assoc",
    "bpred.ras_depth":                     "ras-depth",
    "bpred.mispredict_penalty_cycles":     "mispredict-penalty-cycles",
    "memory.memory_model":                 "memory-model",
    "memory.l1.size_bytes":                "l1-size",
    "memory.l1.assoc":                     "l1-assoc",
    "memory.l1.line_size":                 "l1-line-size",
    "memory.l1.latency_hit_cycles":        "l1-hit-cycles",
    "memory.l1.latency_miss_cycles":       "l1-miss-cycles",
    "memory.dram.read_cycles":             "dram-read-cycles",
    "memory.dram.write_cycles":            "dram-write-cycles",
    # AC-K-3.2 MSHR knobs. Propagated to cae-cache-mshr when
    # memory-model=mshr. Setting mshr.mshr_size=1 collapses the
    # wrapper to a sync passthrough (no overlap credit); values >= 2
    # make the parallel-outstanding accounting kick in.
    "memory.mshr.mshr_size":               "mshr-size",
    "memory.mshr.fill_queue_size":         "fill-queue-size",
    "memory.mshr.writeback_queue_size":    "writeback-queue-size",
    # Round 50 AC-K-5: L1D bank-conflict + sbuffer evict-threshold.
    # bank_count / bank_conflict_stall_cycles only take effect under
    # memory-model=mshr (cae-cache-mshr applies them to its
    # per-bank-last-cycle tracker). sbuffer_evict_threshold is
    # forwarded to cae-cpu-ooo's "sbuffer-evict-threshold" which
    # then propagates into the embedded sbuffer child at
    # complete() time; it takes effect only under
    # cpu-model=ooo-kmhv3.
    "memory.mshr.bank_count":              "bank-count",
    "memory.mshr.bank_conflict_stall_cycles": "bank-conflict-stall-cycles",
    "memory.sbuffer_evict_threshold":      "sbuffer-evict-threshold",
    "memory.tlb_miss_cycles":              "tlb-miss-cycles",
    # Round 18 t-icache: separate-instruction-cache knobs
    # consumed by accel/cae/cae-all.c when memory-model=mshr.
    # Maps the paired-YAML cae_only.memory.l1i.* block onto
    # the accel-class `icache-*` properties.
    "memory.l1i.size_bytes":               "icache-size",
    "memory.l1i.assoc":                    "icache-assoc",
    "memory.l1i.line_size":                "icache-line-size",
    "memory.l1i.latency_hit_cycles":       "icache-hit-cycles",
    "memory.l1i.latency_miss_cycles":      "icache-miss-cycles",
}


def _cae_backend_variant(cfg: p.PairedConfig) -> str:
    cpu = cfg.cae_only.get("cpu", {}) if cfg.cae_only else {}
    cpu_model = cpu.get("cpu_model", "cpi1") if isinstance(cpu, dict) else "cpi1"
    bpred = cfg.cae_only.get("bpred", {}) if cfg.cae_only else {}
    bpred_model = (bpred.get("bpred_model", "none")
                   if isinstance(bpred, dict) else "none")
    return f"cae-{cpu_model}-bpred-{bpred_model}"


def _cae_accel_knobs(cae_only: dict[str, object]) -> list[tuple[str, str]]:
    """Flatten cae_only into ordered (accel_prop, value_str) pairs.

    Rejects unknown nested keys so config-equivalence.py's unsupported
    flag has a matching run-time enforcement path. Order preserved for
    deterministic command lines.
    """
    if not cae_only:
        return []
    pairs = list(p.flatten("", cae_only))
    out: list[tuple[str, str]] = []
    for key, value in pairs:
        if not key or isinstance(value, (dict, list)):
            continue
        # Round 41: `spec_stimulus_program` is a CaeEngine QOM
        # property, not an accel knob — forwarded later via
        # qom-set on /objects/cae-engine, not via -accel cae,...
        # Accept it here so the whitelist doesn't reject it.
        if key == "spec_stimulus_program":
            continue
        mapped = _CAE_ACCEL_FIELD_MAP.get(key)
        if mapped is None:
            raise RuntimeError(
                f"run-cae.py: unknown cae_only field '{key}' "
                f"(accepted keys: {sorted(_CAE_ACCEL_FIELD_MAP)})"
            )
        out.append((mapped, str(value)))
    return out


def _qmp_send(proc: subprocess.Popen[str], cmd: str, **args: object) -> None:
    payload: dict[str, object] = {"execute": cmd}
    if args:
        payload["arguments"] = args
    assert proc.stdin is not None
    proc.stdin.write(json.dumps(payload) + "\n")
    proc.stdin.flush()


def _qmp_recv(proc: subprocess.Popen[str]) -> dict[str, object]:
    assert proc.stdout is not None
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("QMP channel closed unexpectedly")
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "return" in obj or "error" in obj:
            return obj


def _read_sentinel_u64(
    proc: subprocess.Popen[str], sentinel_addr: int
) -> int | None:
    """Single-shot read of sentinel; returns None if unparseable."""
    _qmp_send(proc, "human-monitor-command",
              **{"command-line": f"xp /1gx 0x{sentinel_addr:x}"})
    reply = _qmp_recv(proc)
    if "error" in reply:
        raise RuntimeError(f"QMP xp error: {reply['error']}")
    out = str(reply.get("return", "")).strip()
    if ":" not in out:
        return None
    _, _, val_str = out.partition(":")
    val_str = val_str.strip().split()[0] if val_str else ""
    if not val_str:
        return None
    try:
        return int(val_str, 0)
    except ValueError:
        return None


def _run_to_stopped_sentinel(
    proc: subprocess.Popen[str], sentinel_addr: int, timeout_s: float
) -> None:
    """Drive the VM through cont/stop slices until we observe the
    sentinel on a paused vCPU.

    AC-11 requires serial determinism, which means the moment we
    sample CAE counters has to be at a deterministic CPU boundary.
    Polling the sentinel while the vCPU is still running gives the
    halt-loop (ebreak -> wfi -> j halt) a variable number of extra
    retirements between the detection point and the final
    QMP stop + qom-get. Instead, alternate cont/stop with a small
    sleep; once a check-while-stopped sees the sentinel, the CPU
    is frozen and the counters are guaranteed stable.

    The VM starts paused (-S was on the QEMU command line) so the
    first iteration issues `cont` to get execution going.
    """
    deadline = time.monotonic() + timeout_s
    slice_s = 0.1
    running = False
    while time.monotonic() < deadline:
        if not running:
            _qmp_send(proc, "cont")
            _qmp_recv(proc)
            running = True

        time.sleep(slice_s)

        _qmp_send(proc, "stop")
        _qmp_recv(proc)
        running = False

        val = _read_sentinel_u64(proc, sentinel_addr)
        if val == SENTINEL_VALUE:
            return

        # Grow the slice modestly so long benchmarks finish within a
        # few seconds of wallclock without drowning us in stop/cont
        # round trips.
        if slice_s < 1.0:
            slice_s = min(1.0, slice_s * 1.5)

    raise TimeoutError(
        f"Sentinel 0x{SENTINEL_VALUE:08x} not observed at 0x{sentinel_addr:x} "
        f"within {timeout_s}s (guest may be stuck or the sentinel window "
        "is beyond the configured timeout)"
    )


def _qom_get_uint(proc: subprocess.Popen[str], path: str, prop: str) -> int:
    _qmp_send(proc, "qom-get", path=path, property=prop)
    reply = _qmp_recv(proc)
    if "error" in reply:
        raise RuntimeError(f"qom-get {path}.{prop} failed: {reply['error']}")
    value = reply.get("return")
    if not isinstance(value, int):
        raise RuntimeError(
            f"qom-get {path}.{prop} returned non-integer: {value!r}"
        )
    return value


def _qom_get_uint_opt(proc: subprocess.Popen[str], path: str,
                      prop: str) -> int:
    """Read a uint property, returning 0 when the prop doesn't exist.

    Used for round-2 secondary-metric stats whose presence depends on
    which cpu-model / bpred-model is active. A cpi1 + bpred-none run
    legitimately has no bpred stats; treating that as "0 mispredicts"
    is the right behaviour for suite-wide aggregation.
    """
    _qmp_send(proc, "qom-get", path=path, property=prop)
    reply = _qmp_recv(proc)
    if "error" in reply:
        return 0
    value = reply.get("return")
    if not isinstance(value, int):
        return 0
    return value


def run(cfg: p.PairedConfig, qemu_bin: Path, output: Path | None,
        timeout_s: float, trace_out: Path | None = None,
        checkpoint_out: Path | None = None) -> dict[str, object]:
    elf = p.verify_benchmark(cfg, variant="qemu")
    sentinel_addr = _resolve_sentinel_addr(cfg)
    clock = int(cfg.cpu["clock_freq_hz"])
    num_cpus = int(cfg.cpu["core_count"])
    mem_bytes = int(cfg.memory["dram_size"])
    mem_mib = max(64, mem_bytes // (1 << 20))

    # Translate the YAML's cae_only section into comma-separated
    # -accel cae,key=value pairs. The accel property names must match
    # those exposed by accel/cae/cae-all.c::cae_accel_instance_init;
    # we error out on unknown knobs so a stale YAML doesn't silently
    # launch with the wrong model.
    cae_knobs = _cae_accel_knobs(cfg.cae_only)
    # AC-11 determinism: plumb the benchmark's sentinel address into
    # the accel so the engine freezes counters the moment the guest
    # writes the sentinel. This removes halt-loop sampling jitter
    # from all downstream reads.
    cae_knobs.append(("sentinel-addr", str(sentinel_addr)))
    # AC-K-2 trace-out plumbing. When the caller passes --trace-out
    # (run-xs-suite.sh does at every tier-1 benchmark), propagate it
    # to the CAE accelerator so the retire-boundary emitter writes
    # into the requested path. Parent directory is created so a fresh
    # report dir can be driven without racy mkdir-in-suite.
    #
    # AC-K-10 alignment: also pass `trace-start-pc=<manifest.reset_pc>`
    # so the CAE trace skips the QEMU virt machine's reset-vector
    # bootrom at 0x1000 and starts emitting from the benchmark's
    # entry point — matching NEMU's direct raw-binary boot at the
    # same address. Without this, CAE's first retired PC is 0x1000
    # (bootrom jmp) while NEMU's is MANIFEST.reset_pc (0x80000000)
    # and the preflight diverges on every tier-1 run.
    # Resolve MANIFEST.reset_pc once so trace-out and checkpoint-out
    # both feed it into trace-start-pc.
    reset_pc_value: int | None = None
    try:
        bench_entry = p.load_manifest()["benchmarks"].get(
            cfg.benchmark["name"])
        reset_pc_str = (bench_entry or {}).get("reset_pc")
        if reset_pc_str is not None:
            parsed = int(str(reset_pc_str), 0)
            if parsed != 0:
                reset_pc_value = parsed
    except (KeyError, ValueError):
        reset_pc_value = None

    if trace_out is not None:
        trace_out.parent.mkdir(parents=True, exist_ok=True)
        cae_knobs.append(("trace-out", str(trace_out)))
    if checkpoint_out is not None:
        checkpoint_out.parent.mkdir(parents=True, exist_ok=True)
        cae_knobs.append(("checkpoint-out", str(checkpoint_out)))
    if (trace_out is not None or checkpoint_out is not None) \
            and reset_pc_value is not None:
        cae_knobs.append(("trace-start-pc", str(reset_pc_value)))
    accel_arg = "cae"
    if cae_knobs:
        accel_arg = "cae," + ",".join(f"{k}={v}" for k, v in cae_knobs)

    cmd = [
        str(qemu_bin),
        "-accel", accel_arg,
        "-machine", "virt",
        "-cpu", "rv64",
        "-m", f"{mem_mib}M",
        "-nographic", "-serial", "none", "-monitor", "none",
        "-bios", str(elf),
        "-S", "-qmp", "stdio",
    ]

    t0 = time.monotonic()
    proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True,
    )
    try:
        assert proc.stdout is not None
        proc.stdout.readline()  # QMP greeting
        _qmp_send(proc, "qmp_capabilities")
        _qmp_recv(proc)

        # Round 41 directive step 5: forward the
        # spec_stimulus_program YAML field to the engine via
        # qom-set on /objects/cae-engine. Setter pre-validates
        # the format (<op>:<addr>:<bytes>[:<value>]; ...); a
        # malformed program fails the run loudly. Empty/missing
        # field is a no-op.
        cae_cfg = cfg.cae_only if cfg.cae_only else {}
        spec_program = cae_cfg.get("spec_stimulus_program")
        if spec_program:
            # Round 44: resolve `${sym:<name>}` tokens against
            # the benchmark's QEMU ELF symbol table. See
            # _expand_sym_tokens docstring for the rationale.
            spec_program = _expand_sym_tokens(str(spec_program), elf)
            _qmp_send(proc, "qom-set",
                      path="/objects/cae-engine",
                      property="spec-stimulus-program",
                      value=spec_program)
            reply = _qmp_recv(proc)
            if "error" in reply:
                raise RuntimeError(
                    "qom-set /objects/cae-engine.spec-stimulus-program "
                    f"failed: {reply['error']}"
                )

        # Arm the sbuffer tick-driver evidence path ONLY on
        # the xs-1c-kmhv3 calibration lane. Other OoO configs
        # keep the zero-threshold default behaviour so their
        # reports are not perturbed by a value a different
        # lane chose. The sbuffer child's `inactive-threshold`
        # and `sqfull-commit-lag-threshold` are RW QOM
        # properties on `/objects/cae-cpu-model/sbuffer` and
        # take effect immediately on write — the tick pump
        # reads them every retire. We MUST write these
        # post-QMP-handshake (post-user_creatable_complete),
        # so we target the child object directly: the
        # cpu-model-side `sbuffer-inactive-threshold` /
        # `sbuffer-sqfull-commit-lag-threshold` properties
        # only forward during construction and would be a
        # no-op on a live write.
        #
        # Values: `inactive-threshold` = 2 matches the DEC-5
        # legal minimum (1 would starve Full / SQFull).
        #
        # `sqfull-commit-lag-threshold` = 2 is the Round-9
        # retune that makes the natural SQFull trigger
        # reachable on the representative store-dense
        # Lower-Bound benchmarks. Under the Round-8 plan-
        # compliant ordering, at the alloc-charge tick the
        # sbuffer holds exactly one just-dispatched store
        # whose sqn = store_sqn_next - 1, and
        # sbuffer_commit_sqn still reflects the previous
        # charge's committed store. The tick-entry lag is
        # therefore `store_sqn_next - sbuffer_commit_sqn = 2`
        # on every 1-cycle store charge. The pre-Round-9
        # value of 4 (chosen when the observable peak was
        # 1 under the inverted ordering, hence "25% of
        # sbuffer-entries=16") was above the measured peak
        # and never fired naturally. Value 2 fires SQFull
        # exactly on this one-entry non-drainable head
        # pattern, which is the only reachable natural
        # Lower-Bound eviction cause (Full needs
        # occupancy>=12, Timeout needs inactive>=3; both
        # unreachable at `occupancy_max=1` + `inactive_max=1`).
        # Soft-ignore errors so cpu-models without a sbuffer
        # child (cpi1 / inorder-5stage / older builds) under
        # the xs-1c-kmhv3 lane still run cleanly.
        lane_config_name = cfg.common["contract"]["config_name"]
        if lane_config_name == "xs-1c-kmhv3":
            sb_path = "/objects/cae-cpu-model/sbuffer"
            for prop, val in (
                ("inactive-threshold", 2),
                ("sqfull-commit-lag-threshold", 2),
            ):
                _qmp_send(proc, "qom-set",
                          path=sb_path,
                          property=prop, value=val)
                reply = _qmp_recv(proc)
                if "error" in reply:
                    # Non-fatal: cpu-models without a sbuffer
                    # child (cpi1 / inorder-5stage) skip this
                    # path so existing in-order regressions
                    # stay unchanged.
                    pass

        # Drive the VM through cont/stop slices until the sentinel is
        # observed on a paused vCPU. The helper's contract is "returns
        # with the VM stopped and the sentinel visible", which is the
        # stable boundary required to read CAE counters deterministically.
        _run_to_stopped_sentinel(proc, sentinel_addr, timeout_s)

        # Sanity-check the sentinel one more time on the stopped VM.
        # A ghost read from the cont/stop loop would show the pre-sentinel
        # zero here; real sentinel writes show 0xCAE0CAFE.
        _qmp_send(proc, "human-monitor-command",
                  **{"command-line": f"xp /1gx 0x{sentinel_addr:x}"})
        reply = _qmp_recv(proc)
        verify = str(reply.get("return", "")).strip().lower()
        if f"{SENTINEL_VALUE:x}" not in verify:
            raise RuntimeError(
                f"Sentinel verification failed after QMP stop: {verify!r}"
            )

        # Read aggregate + per-CPU counters.
        current_cycle = _qom_get_uint(proc,
                                      "/objects/cae-engine",
                                      "current-cycle")
        base_freq = _qom_get_uint(proc,
                                  "/objects/cae-engine",
                                  "base-freq-hz")
        engine_cpus = _qom_get_uint(proc,
                                    "/objects/cae-engine",
                                    "num-cpus")
        effective_cpus = max(num_cpus, engine_cpus)
        cpus: list[dict[str, object]] = []
        first_pc_observed: int | None = None
        for idx in range(effective_cpus):
            path = f"/objects/cae-engine/cpu{idx}"
            cycles = _qom_get_uint(proc, path, "cycle-count")
            insns = _qom_get_uint(proc, path, "insn-count")
            stall = _qom_get_uint(proc, path, "stall-cycles")
            bp_pred = _qom_get_uint_opt(proc, path, "bpred-predictions")
            bp_miss = _qom_get_uint_opt(proc, path, "bpred-mispredictions")
            l1d_hit = _qom_get_uint_opt(proc, path, "l1d-hits")
            l1d_miss = _qom_get_uint_opt(proc, path, "l1d-misses")
            mem_stall = _qom_get_uint_opt(proc, path,
                                          "memory-stall-cycles")
            load_hit = _qom_get_uint_opt(proc, path, "load-hits")
            load_miss = _qom_get_uint_opt(proc, path, "load-misses")
            load_stall = _qom_get_uint_opt(proc, path,
                                           "load-stall-cycles")
            # Round 41 directive step 5: speculative-memory
            # stimulus seam accept / reject counters. Both
            # default to 0 on builds before round 40 (the
            # -opt fetcher returns 0 on unknown props).
            spec_drained = _qom_get_uint_opt(proc, path,
                                             "spec-stimuli-drained")
            spec_rejected = _qom_get_uint_opt(proc, path,
                                              "spec-stimuli-rejected")
            # AC-K-10: CaeCpu latches the first retired PC as a read-
            # only QOM property so the suite can cross-check it
            # against NEMU's first record and the manifest's
            # reset_pc. The property is added in round 4; older
            # builds without it return 0, which harmlessly looks like
            # "never retired" and the cross-check treats it as a skip.
            first_pc = _qom_get_uint_opt(proc, path, "first-pc")
            if first_pc_observed is None and first_pc:
                first_pc_observed = first_pc
            ipc = float(insns) / cycles if cycles else 0.0
            mispredict_rate = (float(bp_miss) / bp_pred) if bp_pred else 0.0
            l1d_total = l1d_hit + l1d_miss
            # MPKI = L1D misses per kilo-instruction (all data accesses).
            l1d_mpki = (1000.0 * l1d_miss / insns) if insns else 0.0
            # Average *load* latency = load_stall_cycles / (load_hits +
            # load_misses). Plan's AC-4 metric is per-LOAD, not
            # per-data-access — stores and AMO writes must not inflate
            # this number. Round-3's version used the L1D aggregate and
            # produced 60.0 on alu because the sentinel store counted
            # as a "load"; the round-4 split counts store/AMO into
            # l1d_* but keeps load_* clean.
            load_total = load_hit + load_miss
            avg_load_latency = (float(load_stall) / load_total
                                if load_total else 0.0)
            cpus.append({
                "cpu_index": idx,
                "cycles": cycles,
                "insns": insns,
                "stall_cycles": stall,
                "first_pc": first_pc,
                "ipc": ipc,
                "bpred_predictions": bp_pred,
                "bpred_mispredictions": bp_miss,
                "mispredict_rate": mispredict_rate,
                "l1d_hits": l1d_hit,
                "l1d_misses": l1d_miss,
                "l1d_mpki": l1d_mpki,
                "memory_stall_cycles": mem_stall,
                "load_hits": load_hit,
                "load_misses": load_miss,
                "load_stall_cycles": load_stall,
                "avg_load_latency": avg_load_latency,
                "spec_stimuli_drained": spec_drained,
                "spec_stimuli_rejected": spec_rejected,
            })
        # Round 50 AC-K-5: capture cpu-model AC-K-5 counters if the
        # OoO cpu-model attached. These live on /objects/cae-cpu-model
        # (added in cae_init_machine when cpu-model != cpi1) and
        # surface the segmented scheduler, violation tracker,
        # bank-conflict, and sbuffer evict-threshold counters that
        # the kmhv3 YAML drives. Missing properties (under cpi1 /
        # inorder-5stage cpu-models, or older builds) resolve to
        # 0 via the -opt fetcher and are harmless.
        cpu_model_path = "/objects/cae-cpu-model"
        cpu_model_stats = {
            "scheduler_enqueued": _qom_get_uint_opt(
                proc, cpu_model_path, "scheduler-enqueued"),
            "scheduler_issued": _qom_get_uint_opt(
                proc, cpu_model_path, "scheduler-issued"),
            "scheduler_backpressure": _qom_get_uint_opt(
                proc, cpu_model_path, "scheduler-backpressure"),
            "violation_loads_observed": _qom_get_uint_opt(
                proc, cpu_model_path, "violation-loads-observed"),
            "violation_stores_observed": _qom_get_uint_opt(
                proc, cpu_model_path, "violation-stores-observed"),
            "violation_raw_violations": _qom_get_uint_opt(
                proc, cpu_model_path, "violation-raw-violations"),
            "violation_replay_consumed": _qom_get_uint_opt(
                proc, cpu_model_path, "violation-replay-consumed"),
            "rename_stalls": _qom_get_uint_opt(
                proc, cpu_model_path, "rename-stalls"),
            # Tick-driver per-retire counters live on the cpu-model
            # object itself. bank_conflict_cpu_events accumulates
            # every same-bank same-cycle retire collision; the
            # sbuffer_eviction_events counter accumulates real
            # Timeout / Full / SQFull evictions produced by the
            # sbuffer tick pump. These are the kmhv3-calibration
            # evidence signals: the current scope expects
            # sbuffer_eviction_events > 0 on store-dense workloads
            # and bank_conflict_cpu_events may stay 0 until the
            # virtual-issue batching lands. Missing on
            # inorder-5stage / cpi1 cpu-models -> 0 via the -opt
            # fetcher.
            "bank_conflict_cpu_events": _qom_get_uint_opt(
                proc, cpu_model_path, "bank-conflict-cpu-events"),
            "sbuffer_eviction_events": _qom_get_uint_opt(
                proc, cpu_model_path, "sbuffer-eviction-events"),
        }
        # Legacy telemetry kept as secondary fields so existing
        # AC-K-5 audit trails that consult the older per-module
        # counters keep reading them. bank_conflict_events lives
        # on /objects/cae-mem-root (mshr path).
        # sbuffer_evict_threshold_events is the pressure signal
        # on the sbuffer child: "retire with occupancy >=
        # watermark" — intentionally separate from the real
        # eviction total above.
        mshr_path = "/objects/cae-mem-root"
        cpu_model_stats["bank_conflict_events"] = _qom_get_uint_opt(
            proc, mshr_path, "bank-conflict-events")
        sb_path = f"{cpu_model_path}/sbuffer"
        cpu_model_stats["sbuffer_evict_threshold_events"] = (
            _qom_get_uint_opt(proc, sb_path, "evict-threshold-events"))
        # Per-cause eviction counters are live on the sbuffer
        # child too — capture all three so the calibration log
        # can show three-cause exclusivity on real workloads.
        cpu_model_stats["sbuffer_timeout_evicts"] = _qom_get_uint_opt(
            proc, sb_path, "timeout-evicts")
        cpu_model_stats["sbuffer_full_evicts"] = _qom_get_uint_opt(
            proc, sb_path, "full-evicts")
        cpu_model_stats["sbuffer_sqfull_evicts"] = _qom_get_uint_opt(
            proc, sb_path, "sqfull-evicts")
        # Tick-path diagnostic telemetry. Lifetime-only. Lets
        # the calibration log explain the eviction-signal shape
        # quantitatively: if tick-head-drainable-events greatly
        # exceeds tick-head-non-drainable-events the workload's
        # commit pace keeps the sbuffer head always committable
        # (Full / SQFull branches cannot fire by construction);
        # tick-occupancy-max bounds the natural residency peak
        # against evict_threshold; tick-inactive-max bounds how
        # close inactive_cycles approached the Timeout gate.
        cpu_model_stats["sbuffer_tick_calls"] = _qom_get_uint_opt(
            proc, sb_path, "tick-calls")
        cpu_model_stats["sbuffer_tick_head_drainable_events"] = (
            _qom_get_uint_opt(proc, sb_path,
                              "tick-head-drainable-events"))
        cpu_model_stats["sbuffer_tick_head_non_drainable_events"] = (
            _qom_get_uint_opt(proc, sb_path,
                              "tick-head-non-drainable-events"))
        cpu_model_stats["sbuffer_tick_inactive_max"] = (
            _qom_get_uint_opt(proc, sb_path, "tick-inactive-max"))
        cpu_model_stats["sbuffer_tick_occupancy_max"] = (
            _qom_get_uint_opt(proc, sb_path, "tick-occupancy-max"))

        _qmp_send(proc, "quit")
        try:
            _qmp_recv(proc)
        except Exception:
            pass
    finally:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    wallclock = time.monotonic() - t0

    total_cycles = current_cycle
    total_insns = sum(int(c["insns"]) for c in cpus)
    total_bp_pred = sum(int(c.get("bpred_predictions", 0)) for c in cpus)
    total_bp_miss = sum(int(c.get("bpred_mispredictions", 0)) for c in cpus)
    total_l1d_hit = sum(int(c.get("l1d_hits", 0)) for c in cpus)
    total_l1d_miss = sum(int(c.get("l1d_misses", 0)) for c in cpus)
    total_load_hit = sum(int(c.get("load_hits", 0)) for c in cpus)
    total_load_miss = sum(int(c.get("load_misses", 0)) for c in cpus)
    total_load_stall = sum(int(c.get("load_stall_cycles", 0)) for c in cpus)
    # Round 42 (Codex round-41 review): aggregate
    # stimulus-drain totals promised by the round-41 contract
    # but missed by the initial implementation. Additive to the
    # existing aggregate schema; empty-program runs emit 0.
    total_spec_drained = sum(
        int(c.get("spec_stimuli_drained", 0)) for c in cpus)
    total_spec_rejected = sum(
        int(c.get("spec_stimuli_rejected", 0)) for c in cpus)
    ipc = (total_insns / total_cycles) if total_cycles else 0.0
    mispredict_rate = (
        float(total_bp_miss) / total_bp_pred if total_bp_pred else 0.0
    )
    l1d_mpki = (1000.0 * total_l1d_miss / total_insns
                if total_insns else 0.0)
    # Load-only avg latency (plan's AC-4 metric) — see BL-20260418-
    # avg-load-latency-load-only. Aggregate counts: stores / AMO
    # contributions live in total_l1d_* but are excluded here.
    avg_load_latency = (
        float(total_load_stall) / (total_load_hit + total_load_miss)
        if (total_load_hit + total_load_miss) else 0.0
    )

    report: dict[str, object] = {
        "schema_version": 1,
        "config_name": cfg.common["contract"]["config_name"],
        "benchmark": cfg.benchmark["name"],
        "backend": "cae",
        "backend_variant": _cae_backend_variant(cfg),
        "num_cpus": len(cpus),
        "clock_freq_hz": base_freq if base_freq else clock,
        "sample_stats_at": cfg.measurement.get("sample_stats_at", "ebreak"),
        # AC-K-10: first architectural PC observed by the CAE retire
        # path, as reported by the CAE engine's per-CPU first-pc
        # latch. None when the QOM property doesn't exist (pre-round-4
        # binary) or the run never retired an insn. run-xs-suite.sh
        # cross-checks this against NEMU's first record and the
        # manifest's reset_pc.
        "first_pc": first_pc_observed,
        "cpus": cpus,
        "aggregate": {
            "total_cycles": total_cycles,
            "total_insns": total_insns,
            "ipc": ipc,
            "mispredict_rate": mispredict_rate,
            "l1d_mpki": l1d_mpki,
            "avg_load_latency": avg_load_latency,
            "total_spec_stimuli_drained": total_spec_drained,
            "total_spec_stimuli_rejected": total_spec_rejected,
        },
        # Round 50 AC-K-5: cpu-model-level counters (scheduler,
        # violation, rename_stalls) + memory-model counters
        # (bank_conflict_events, sbuffer evict-threshold). Populated
        # only when the corresponding modules attached; the -opt
        # QMP fetcher returns 0 for absent properties so cpi1 /
        # non-mshr configs emit an all-zeros block harmlessly.
        "cpu_model": cpu_model_stats,
        "wallclock_seconds": wallclock,
    }

    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("w") as f:
            json.dump(report, f, indent=2, sort_keys=False)
            f.write("\n")
    return report


def _selftest_forwarder(qemu_bin: Path) -> int:
    """Verify accel→cpu-model property forwarding via QMP."""
    _ensure_qemu_binary_fresh(qemu_bin)
    if not qemu_bin.is_file():
        print(f"run-cae.py: missing {qemu_bin}", file=sys.stderr)
        return 2

    checks: list[tuple[str, str, str, object]] = [
        ("cpu-model=ooo-kmhv3,ooo-issue-ports=4",
         "/objects/cae-cpu-model", "sched-issue-ports", 4),
        ("cpu-model=ooo-kmhv3",
         "/objects/cae-cpu-model", "sched-issue-ports", 2),
        ("cpu-model=ooo-kmhv3,ooo-issue-ports=4,ooo-issue-width=6",
         "/objects/cae-cpu-model", "issue-width", 6),
        ("cpu-model=ooo-kmhv3,ooo-virtual-issue-window=4",
         "/objects/cae-cpu-model", "virtual-issue-window", 4),
        ("cpu-model=ooo-kmhv3,ooo-dependent-load-stall-cycles=3",
         "/objects/cae-cpu-model", "dependent-load-stall-cycles", 3),
    ]
    fail = 0
    for accel_opts, path, prop, expected in checks:
        proc = subprocess.Popen(
            [str(qemu_bin), "-accel", f"cae,{accel_opts}",
             "-M", "virt", "-nographic", "-monitor", "none",
             "-serial", "none", "-S", "-qmp", "stdio"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True,
        )
        try:
            _qmp_send(proc, "qmp_capabilities")
            _qmp_recv(proc)
            _qmp_send(proc, "qom-get", path=path, property=prop)
            reply = _qmp_recv(proc)
            _qmp_send(proc, "quit")
            val = reply.get("return")
            if val == expected:
                print(f"PASS: {accel_opts} → {prop}={val}")
            else:
                print(f"FAIL: {accel_opts} → {prop}={val} "
                      f"(expected {expected})", file=sys.stderr)
                fail = 1
        finally:
            proc.terminate()
            proc.wait(timeout=5)
    return fail


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("config", nargs="?", help="Paired YAML name or path")
    ap.add_argument("--benchmark",
                    help="Override common.benchmark.name from the YAML")
    ap.add_argument("--qemu-bin", type=Path, default=DEFAULT_BINARY)
    ap.add_argument("--output", type=Path,
                    help="Write JSON report here (default stdout)")
    ap.add_argument("--trace-out", type=Path, default=None,
                    help="Write the CAE retire-boundary binary trace "
                         "here (AC-K-2 tier-1 functional difftest). "
                         "Propagated into `-accel cae,trace-out=<path>`; "
                         "the RISC-V emitter opens the file on first "
                         "retired instruction and writes a 16-byte "
                         "CaeTraceHeader followed by 40-byte "
                         "CaeTraceRecord entries per include/cae/trace.h.")
    ap.add_argument("--checkpoint-out", type=Path, default=None,
                    help="Write periodic architectural-state "
                         "checkpoints here (AC-K-2 tier-2 functional "
                         "difftest). Propagated into `-accel cae,"
                         "checkpoint-out=<path>`; the RISC-V emitter "
                         "opens the file on first interval trigger "
                         "(default every 1 000 000 retired "
                         "instructions) and writes a 16-byte "
                         "CaeTraceHeader with mode=CHECKPOINT "
                         "followed by CaeTraceCheckpointRecord "
                         "entries per include/cae/trace.h.")
    ap.add_argument("--timeout", type=float, default=900.0,
                    help="QMP sentinel poll timeout, seconds (large by "
                         "default because one-insn-per-tb + softmmu "
                         "hooks are slow on memory-heavy benchmarks)")
    ap.add_argument("--self-test", action="store_true",
                    help="Run accel-forwarder self-test instead of a "
                         "benchmark. Verifies ooo-issue-ports and "
                         "ooo-issue-width forwarding via QMP.")
    args = ap.parse_args()

    if args.self_test:
        return _selftest_forwarder(args.qemu_bin)

    if args.config is None:
        ap.error("config is required (or use --self-test)")

    cfg = p.load_config(args.config)
    if args.benchmark:
        cfg.benchmark.setdefault("_original_name", cfg.benchmark["name"])
        cfg.benchmark["name"] = args.benchmark

    # Round 7 freshness guard: rebuild (or fail closed) when the
    # CAE binary predates the CAE source lanes. Round 6 discovered
    # that `build-cae/qemu-system-riscv64` had been stale since
    # Round 0 — every prior "live" run had been executing a pre-
    # substrate binary. _ensure_qemu_binary_fresh raises
    # SystemExit(2) if rebuild is unavailable or fails.
    _ensure_qemu_binary_fresh(args.qemu_bin)

    if not args.qemu_bin.is_file():
        print(f"run-cae.py: missing {args.qemu_bin}; build build-cae first",
              file=sys.stderr)
        return 2

    report = run(cfg, args.qemu_bin, args.output, args.timeout,
                 trace_out=args.trace_out,
                 checkpoint_out=args.checkpoint_out)
    if args.output is None:
        json.dump(report, sys.stdout, indent=2, sort_keys=False)
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
