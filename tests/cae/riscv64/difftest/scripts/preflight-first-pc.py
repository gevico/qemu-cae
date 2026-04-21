#!/usr/bin/env python3
"""
AC-K-10 4-way first-PC preflight.

Consumes the manifest reset PC plus the per-benchmark report dir
artifacts (CAE QMP first-PC JSON, CAE trace, NEMU trace,
XS-GEM5 JSON) and decides whether all producers agree on the
boot PC. Extracted from run-xs-suite.sh in round 11 so the same
authoritative Python path can be exercised by a harness
regression (tests/cae/run-cae-test.sh) with synthetic fixtures.

Exit codes:
  0 -> VERDICT: MATCH        (all non-skip columns agree)
  1 -> VERDICT: DIVERGE      (concrete disagreement; fail the benchmark)
  2 -> VERDICT: SKIP         (no non-skip column produced a PC)

BS-26 (round 11) AC-K-10 fail-closed contract: whenever
xs-gem5.json carries a non-None `first_pc_error`, or xs_pc is
null (meaning run-xs-gem5.py was invoked with
`--first-pc-debug` but could not record a runtime first-PC),
return DIVERGE with a concrete reason. This replaces the
previous round-10 behaviour that filtered null xs_pc out of the
`vals` compare set and could still return MATCH on the three
remaining columns.

Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
"""

import json
import struct
import sys


def as_u64(x):
    if x is None:
        return None
    if isinstance(x, int):
        return x & 0xffffffffffffffff
    s = str(x).strip()
    if not s:
        return None
    base = 16 if s.lower().startswith("0x") else 0
    try:
        return int(s, base) & 0xffffffffffffffff
    except ValueError:
        return None


def first_record_pc(path, mode):
    """
    BS-19: first-record pc reader for TRACE mode only. In
    checkpoint mode record 0 is emitted at retire 1M (not boot),
    so comparing it as a boot-PC source is structurally wrong.
    Returns None in checkpoint mode so the preflight treats the
    column as SKIP.
    """
    if mode != "trace":
        return None
    try:
        with open(path, "rb") as f:
            header = f.read(16)
            if len(header) < 16:
                return None
            record = f.read(40)
            if len(record) < 40:
                return None
    except OSError:
        return None
    return struct.unpack_from("<Q", record, 0)[0]


def load_first_pc(path):
    try:
        with open(path) as f:
            return as_u64(json.load(f).get("first_pc"))
    except (OSError, ValueError):
        return None


def load_first_pc_error(path):
    """
    BS-26 (round 11) AC-K-10 fail-closed. run-xs-gem5.py records
    first_pc_error when --first-pc-debug was requested but the
    Exec log was missing or unparsable. Older JSONs predating
    this field resolve to None (absent = not requested = ok).
    """
    try:
        with open(path) as f:
            return json.load(f).get("first_pc_error")
    except (OSError, ValueError):
        return None


def fmt(v):
    return "None" if v is None else f"0x{v:x}"


def tag(v, mode_is_cp):
    if mode_is_cp and v is None:
        return "SKIP (checkpoint mode)"
    return fmt(v)


def main(argv):
    if len(argv) != 7:
        print("usage: preflight-first-pc.py manifest_pc cae_json "
              "cae_itrace nemu_itrace xs_json mode",
              file=sys.stderr)
        return 2

    (manifest_pc_raw, cae_json, cae_itrace,
     nemu_itrace, xs_json, run_mode) = argv[1:7]

    manifest_pc = as_u64(manifest_pc_raw)
    cae_pc = load_first_pc(cae_json)
    cae_trace_pc = first_record_pc(cae_itrace, run_mode)
    nemu_trace_pc = first_record_pc(nemu_itrace, run_mode)
    xs_pc = load_first_pc(xs_json)
    xs_err = load_first_pc_error(xs_json)

    mode_is_cp = (run_mode == "checkpoint")
    print(f"manifest={fmt(manifest_pc)} cae_qmp={fmt(cae_pc)} "
          f"cae_trace={tag(cae_trace_pc, mode_is_cp)} "
          f"nemu_trace={tag(nemu_trace_pc, mode_is_cp)} "
          f"xs_gem5={fmt(xs_pc)}")

    #
    # BS-26 (round 11) AC-K-10 fail-closed. The suite
    # unconditionally passes --first-pc-debug to run-xs-gem5.py,
    # so xs-gem5.json carries first_pc_error whenever the XS-side
    # Exec-log parse failed. A set first_pc_error OR a still-null
    # xs_pc after the flag was requested MUST fail the benchmark
    # — otherwise a silent XS parse failure degrades the 4-way
    # preflight to a 3-way match against the remaining columns,
    # which violates AC-K-10.
    #
    if xs_err is not None:
        print(f"VERDICT: DIVERGE (xs-gem5 first_pc_error: {xs_err})")
        return 1
    if xs_pc is None:
        print("VERDICT: DIVERGE (xs-gem5 first_pc=null; "
              "--first-pc-debug requested but XS did not record a "
              "runtime first-PC)")
        return 1

    vals = [v for v in (manifest_pc, cae_pc, cae_trace_pc,
                        nemu_trace_pc, xs_pc)
            if v is not None and v != 0]
    if not vals:
        print("VERDICT: SKIP")
        return 2
    if any(v != vals[0] for v in vals):
        print(f"VERDICT: DIVERGE (reference={fmt(vals[0])})")
        return 1
    print("VERDICT: MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
