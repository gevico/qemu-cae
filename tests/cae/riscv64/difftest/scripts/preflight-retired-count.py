#!/usr/bin/env python3
"""
AC-K-3.1 retired-insn-count alignment gate.

Compares the retired-insn counts reported by CAE, NEMU, and
XS-GEM5 for one benchmark. Extracted from run-xs-suite.sh in
round 12 (parallel to the round-11 preflight-first-pc.py
extraction) so tests/cae/run-cae-test.sh can exercise the same
authoritative code path with synthetic fixtures, and so the
suite driver can invoke the script under the round-12 set-e-safe
idiom.

Arguments:
    cae_json_path nemu_stats_path nemu_itrace_path xs_json_path mode

Exit codes:
    0 -> VERDICT: MATCH    (all known counts agree within 1)
    1 -> VERDICT: DIVERGE  (spread > 1; AC-K-3.1 fails)
    2 -> VERDICT: SKIP     (fewer than 2 counts available)

BS-20 AC-K-3.1: NEMU retired count comes from the exact
`retired_insn_count` field that run-nemu-ref.sh writes into
`nemu-itrace-stats.json` (parsed from the patch's
`NEMU-ITRACE-FINAL-COUNT:` stderr line). In trace mode the
value equals `(file_size - 16) / 40`, but we prefer the stats
file so trace and checkpoint paths use the same contract. When
the stats file is missing (very-old run-nemu-ref.sh or the
interpreter crashed before the flush line), fall back to
file-size inference so partial runs still report a best-effort
number.

Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
"""

import json
import os
import sys


def load_agg_insns(path):
    try:
        with open(path) as f:
            return int(json.load(f)["aggregate"]["total_insns"])
    except (OSError, KeyError, TypeError, ValueError):
        return None


def load_cae_trace_count(cae_json_path, mode):
    """Count CAE trace records from `cae-itrace.bin` if present.

    Round 10: `aggregate.total_insns` in `cae.json` counts every
    retired architectural instruction across the whole QEMU run,
    including post-sentinel halt-loop retires that the trace
    emitter stops emitting at the freeze boundary. NEMU and
    XS-GEM5 naturally stop counting at the same freeze point, so
    comparing the CAE aggregate against either yields a 5-10 insn
    spread on every benchmark (the exact size of the halt-loop
    tail QEMU retires between the sentinel store and the QMP
    driver's `quit`). Returning the trace-record count here lets
    the AC-K-3.1 spread gate apply the same definition of
    "retired" on all three lanes.

    Falls back to None on read errors or when the trace file is
    absent; callers continue to load_agg_insns for the cae_cnt
    field in that case.
    """
    try:
        trace_path = os.path.join(
            os.path.dirname(cae_json_path), "cae-itrace.bin"
        )
        size = os.path.getsize(trace_path)
    except OSError:
        return None
    if mode == "checkpoint":
        record_size = 608
        interval = 1_000_000
        if size < 16 + record_size:
            return None
        return ((size - 16) // record_size) * interval
    if size < 16 + 40:
        return None
    return (size - 16) // 40


def load_xs_count(path):
    try:
        with open(path) as f:
            doc = json.load(f)
        v = doc.get("retired_insn_count")
        if v is None:
            v = doc.get("aggregate", {}).get("total_insns")
        return int(v) if v is not None else None
    except (OSError, KeyError, TypeError, ValueError):
        return None


def nemu_count(stats_path, functional_path, mode):
    try:
        with open(stats_path) as f:
            doc = json.load(f)
        v = doc.get("retired_insn_count")
        if v is not None:
            return int(v)
    except (OSError, ValueError, TypeError):
        pass
    try:
        size = os.path.getsize(functional_path)
    except OSError:
        return None
    if mode == "checkpoint":
        record_size = 608
        interval = 1_000_000
        if size < 16 + record_size:
            return None
        return ((size - 16) // record_size) * interval
    if size < 16 + 40:
        return None
    return (size - 16) // 40


def fmt(v):
    return "None" if v is None else str(v)


def main(argv):
    if len(argv) != 6:
        print("usage: preflight-retired-count.py cae_json "
              "nemu_stats nemu_itrace xs_json mode",
              file=sys.stderr)
        return 2

    (cae_json, nemu_stats_json, nemu_functional, xs_json,
     run_mode) = argv[1:6]

    # Round 10: prefer the CAE trace-record count over
    # `aggregate.total_insns` so CAE's stop-point semantics match
    # NEMU / XS-GEM5 (the sentinel store + ebreak boundary).
    # Fall back to the aggregate count if the trace file is
    # missing (older runs, non-trace-mode invocations).
    cae_cnt = load_cae_trace_count(cae_json, run_mode)
    if cae_cnt is None:
        cae_cnt = load_agg_insns(cae_json)
    xs_cnt = load_xs_count(xs_json)
    nemu_cnt = nemu_count(nemu_stats_json, nemu_functional, run_mode)

    print(f"cae={fmt(cae_cnt)} nemu={fmt(nemu_cnt)} xs={fmt(xs_cnt)}")

    known = [v for v in (cae_cnt, nemu_cnt, xs_cnt) if v is not None]
    if len(known) < 2:
        print("VERDICT: SKIP (fewer than 2 counts available)")
        return 2
    spread = max(known) - min(known)
    if spread > 1:
        print(f"VERDICT: DIVERGE (spread={spread})")
        return 1
    print("VERDICT: MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
