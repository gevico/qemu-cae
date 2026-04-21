#!/usr/bin/env python3
"""Byte-compare two CAE / NEMU retired-instruction trace files.

Per AC-K-2 the NEMU functional-difftest is binding before any
timing-gate evaluation. This script owns ALL functional comparison
for the KMH-V3 track; diff.py handles timing-only N-side
comparison separately.

Protocol (AC-K-2.1 / AC-K-2.4):

    Every file begins with a CaeTraceHeader matching
    include/cae/trace.h (16 bytes, little-endian):

        magic        uint32  'CAEI' (0x49454143) LE
        version      uint16  1
        endianness   uint8   1 (little-endian)
        mode         uint8   0=trace, 1=checkpoint
        record_size  uint16  40 for trace, 608 for checkpoint
        reserved     uint16  0
        isa_subset   uint32  CAE_TRACE_ISA_* bitmask

    The header is compared first. magic / version / endianness /
    mode / record_size / isa_subset MUST match byte-for-byte
    between the two files. A mismatch is reported as a header
    error and exits non-zero before any record is inspected.

    After the header, records are parsed per the declared
    record_size. Mode 0 (trace) record layout:

        pc           uint64
        opcode       uint32
        rd_idx       uint8   (0xFF = no GPR write)
        flags        uint8   (CAE_TRACE_FLAG_* bits)
        mem_size     uint16  (0 | 1 | 2 | 4 | 8)
        rd_value     uint64
        mem_addr     uint64
        mem_value    uint64

    Each field is compared individually; the first divergence is
    reported with (insn_index, field, expected, actual) and the
    script exits non-zero (AC-K-2 negative test).

Usage:

    nemu-difftest.py --cae <cae.bin> --nemu <nemu.bin> [--mode trace|checkpoint]

The --mode flag is a sanity override for scripted callers; the
header already carries the authoritative mode, and the script
fails closed if --mode disagrees with the header.

run-xs-suite.sh invokes this script once per benchmark and refuses
to invoke diff.py (timing gate) when this exits non-zero.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from typing import NamedTuple


# On-disk magic / sizes. Must match include/cae/trace.h.
CAE_TRACE_MAGIC = 0x49454143  # 'CAEI' little-endian
CAE_TRACE_VERSION = 1
CAE_TRACE_ENDIAN_LITTLE = 1
CAE_TRACE_HEADER_SIZE = 16
CAE_TRACE_RECORD_SIZE_TRACE = 40
CAE_TRACE_RECORD_SIZE_CHECKPOINT = (
    8 +                 # retire_index
    8 +                 # pc
    8 * 32 +            # gpr
    8 * 32 +            # fpr
    8 * 8 +             # csrs
    8 +                 # memory_hash
    4 + 4               # flags + reserved
)

CAE_TRACE_MODE_TRACE = 0
CAE_TRACE_MODE_CHECKPOINT = 1

CAE_TRACE_FLAG_MEM_WRITE = 1 << 0
CAE_TRACE_FLAG_EXCEPTION = 1 << 1
CAE_TRACE_FLAG_SPLIT = 1 << 2
CAE_TRACE_FLAG_SPEC_REPLAY = 1 << 3
CAE_TRACE_FLAG_SCFAIL = 1 << 4


class Header(NamedTuple):
    magic: int
    version: int
    endianness: int
    mode: int
    record_size: int
    reserved: int
    isa_subset: int


_HEADER_STRUCT = struct.Struct("<I H B B H H I")
assert _HEADER_STRUCT.size == CAE_TRACE_HEADER_SIZE

_TRACE_STRUCT = struct.Struct("<Q I B B H Q Q Q")
assert _TRACE_STRUCT.size == CAE_TRACE_RECORD_SIZE_TRACE


class TraceRecord(NamedTuple):
    pc: int
    opcode: int
    rd_idx: int
    flags: int
    mem_size: int
    rd_value: int
    mem_addr: int
    mem_value: int


def _read_header(path: Path) -> Header:
    with path.open("rb") as f:
        buf = f.read(CAE_TRACE_HEADER_SIZE)
    if len(buf) != CAE_TRACE_HEADER_SIZE:
        raise ValueError(
            f"{path}: header truncated "
            f"(got {len(buf)} bytes, expected {CAE_TRACE_HEADER_SIZE})"
        )
    return Header._make(_HEADER_STRUCT.unpack(buf))


def _header_reject(tag: str, hdr: Header, expect: str,
                   actual: str) -> None:
    raise ValueError(
        f"nemu-difftest: header mismatch ({tag}): {expect}, got {actual} "
        f"(magic=0x{hdr.magic:08x} version={hdr.version} "
        f"endianness={hdr.endianness} mode={hdr.mode} "
        f"record_size={hdr.record_size} isa_subset=0x{hdr.isa_subset:x})"
    )


def _validate_header(path: Path, hdr: Header,
                     expected_mode: int | None) -> None:
    """Raise ValueError if the header is not a v1 CAEI trace this script
    knows how to consume (AC-K-2.1 fail-closed before any byte-diff)."""
    if hdr.magic != CAE_TRACE_MAGIC:
        raise ValueError(
            f"{path}: wrong magic 0x{hdr.magic:08x} "
            f"(expected 0x{CAE_TRACE_MAGIC:08x} 'CAEI')"
        )
    if hdr.version != CAE_TRACE_VERSION:
        raise ValueError(
            f"{path}: unsupported version {hdr.version} "
            f"(this script consumes v{CAE_TRACE_VERSION})"
        )
    if hdr.endianness != CAE_TRACE_ENDIAN_LITTLE:
        raise ValueError(
            f"{path}: unsupported endianness {hdr.endianness} "
            f"(only little-endian=1 is implemented)"
        )
    if hdr.mode not in (CAE_TRACE_MODE_TRACE, CAE_TRACE_MODE_CHECKPOINT):
        raise ValueError(f"{path}: unknown mode {hdr.mode}")
    if hdr.mode == CAE_TRACE_MODE_TRACE:
        if hdr.record_size != CAE_TRACE_RECORD_SIZE_TRACE:
            raise ValueError(
                f"{path}: mode=trace but record_size={hdr.record_size} "
                f"(expected {CAE_TRACE_RECORD_SIZE_TRACE})"
            )
    if hdr.mode == CAE_TRACE_MODE_CHECKPOINT:
        if hdr.record_size != CAE_TRACE_RECORD_SIZE_CHECKPOINT:
            raise ValueError(
                f"{path}: mode=checkpoint but record_size={hdr.record_size} "
                f"(expected {CAE_TRACE_RECORD_SIZE_CHECKPOINT})"
            )
    if hdr.reserved != 0:
        raise ValueError(
            f"{path}: reserved field is {hdr.reserved} "
            f"(must be 0 for v1)"
        )
    if expected_mode is not None and hdr.mode != expected_mode:
        raise ValueError(
            f"{path}: mode={hdr.mode} disagrees with --mode "
            f"override {expected_mode}"
        )


def _compare_headers(left: Header, right: Header) -> None:
    """Cross-compare the two producer headers. Any mismatch here is
    fatal before any record is inspected — the files would not be
    comparable otherwise."""
    if left.magic != right.magic:
        _header_reject("magic", left,
                       f"left=0x{left.magic:08x} right=0x{right.magic:08x}",
                       "byte-diff refused")
    if left.version != right.version:
        raise ValueError(
            f"nemu-difftest: header mismatch (version): producer="
            f"{left.version}, consumer={right.version}"
        )
    if left.endianness != right.endianness:
        raise ValueError(
            f"nemu-difftest: header mismatch (endianness): producer="
            f"{left.endianness}, consumer={right.endianness}"
        )
    if left.mode != right.mode:
        raise ValueError(
            f"nemu-difftest: header mismatch (mode): producer="
            f"{left.mode}, consumer={right.mode}"
        )
    if left.record_size != right.record_size:
        raise ValueError(
            f"nemu-difftest: header mismatch (record_size): producer="
            f"{left.record_size}, consumer={right.record_size}"
        )
    if left.isa_subset != right.isa_subset:
        raise ValueError(
            f"nemu-difftest: header mismatch (isa_subset): producer="
            f"0x{left.isa_subset:x}, consumer=0x{right.isa_subset:x}"
        )


def _open_records(path: Path, record_size: int):
    """Yield (index, raw_bytes) pairs for each record after the header."""
    with path.open("rb") as f:
        f.seek(CAE_TRACE_HEADER_SIZE)
        idx = 0
        while True:
            chunk = f.read(record_size)
            if not chunk:
                return
            if len(chunk) != record_size:
                raise ValueError(
                    f"{path}: trailing partial record at index {idx} "
                    f"({len(chunk)} of {record_size} bytes)"
                )
            yield idx, chunk
            idx += 1


def _decode_trace(raw: bytes) -> TraceRecord:
    return TraceRecord._make(_TRACE_STRUCT.unpack(raw))


def _first_trace_divergence(cae: Path, nemu: Path) -> str | None:
    """Return None on equal, else a human-readable first-divergence str."""
    left_iter = _open_records(cae, CAE_TRACE_RECORD_SIZE_TRACE)
    right_iter = _open_records(nemu, CAE_TRACE_RECORD_SIZE_TRACE)
    while True:
        left = next(left_iter, None)
        right = next(right_iter, None)
        if left is None and right is None:
            return None
        if left is None:
            assert right is not None
            return (f"(insn_index={right[0]}, field=length, "
                    f"expected=<more>, actual=<eof>): "
                    f"cae ended before nemu")
        if right is None:
            return (f"(insn_index={left[0]}, field=length, "
                    f"expected=<eof>, actual=<more>): "
                    f"nemu ended before cae")
        lidx, lbuf = left
        _, rbuf = right
        if lbuf == rbuf:
            continue
        lrec = _decode_trace(lbuf)
        rrec = _decode_trace(rbuf)
        # Field-at-a-time comparison so the error message names the
        # first offending column, per AC-K-2 negative test.
        for name in ("pc", "opcode", "rd_idx", "flags", "mem_size",
                     "rd_value", "mem_addr", "mem_value"):
            lv = getattr(lrec, name)
            rv = getattr(rrec, name)
            if lv != rv:
                return (f"(insn_index={lidx}, field={name}, "
                        f"expected={lv:#x}, actual={rv:#x})")
        # Should not reach here if raw bytes differed.
        return (f"(insn_index={lidx}, field=raw, "
                f"expected={lbuf.hex()}, actual={rbuf.hex()})")


def _first_byte_divergence_checkpoint(cae: Path, nemu: Path,
                                      record_size: int) -> str | None:
    """Generic byte-diff for checkpoint mode. We do not decode
    individual fields (the layout is 608 B with many repeated uint64
    arrays); instead the first record whose raw bytes differ is
    reported with a byte offset and its retire_index prefix."""
    left_iter = _open_records(cae, record_size)
    right_iter = _open_records(nemu, record_size)
    while True:
        left = next(left_iter, None)
        right = next(right_iter, None)
        if left is None and right is None:
            return None
        if left is None:
            assert right is not None
            return (f"(checkpoint_index={right[0]}, field=length, "
                    f"expected=<more>, actual=<eof>): cae ended first")
        if right is None:
            return (f"(checkpoint_index={left[0]}, field=length, "
                    f"expected=<eof>, actual=<more>): nemu ended first")
        lidx, lbuf = left
        _, rbuf = right
        if lbuf == rbuf:
            continue
        # retire_index is the first uint64 in the record.
        l_ret = struct.unpack("<Q", lbuf[:8])[0]
        r_ret = struct.unpack("<Q", rbuf[:8])[0]
        byte_off = next(i for i in range(len(lbuf)) if lbuf[i] != rbuf[i])
        return (f"(checkpoint_index={lidx}, retire_index cae={l_ret} "
                f"nemu={r_ret}, byte_offset={byte_off}, "
                f"expected=0x{lbuf[byte_off]:02x}, "
                f"actual=0x{rbuf[byte_off]:02x})")


def compare(cae: Path, nemu: Path,
            expected_mode: int | None = None) -> int:
    for p in (cae, nemu):
        if not p.is_file():
            print(f"nemu-difftest: missing producer: {p}",
                  file=sys.stderr)
            return 2

    try:
        cae_h = _read_header(cae)
        _validate_header(cae, cae_h, expected_mode)
        nemu_h = _read_header(nemu)
        _validate_header(nemu, nemu_h, expected_mode)
        _compare_headers(cae_h, nemu_h)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    mode_name = ("trace" if cae_h.mode == CAE_TRACE_MODE_TRACE
                 else "checkpoint")
    try:
        if cae_h.mode == CAE_TRACE_MODE_TRACE:
            first = _first_trace_divergence(cae, nemu)
        else:
            first = _first_byte_divergence_checkpoint(
                cae, nemu, cae_h.record_size)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    if first is None:
        print(f"nemu-difftest: {mode_name}-mode byte-diff PASS "
              f"(cae={cae} nemu={nemu})")
        return 0

    print(f"nemu-difftest: {mode_name}-mode byte-diff FAIL {first}",
          file=sys.stderr)
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cae", type=Path, required=True,
                    help="Path to cae-itrace.bin / cae-checkpoints.bin")
    ap.add_argument("--nemu", type=Path, required=True,
                    help="Path to nemu-itrace.bin / nemu-checkpoints.bin")
    ap.add_argument("--mode", choices=("trace", "checkpoint"),
                    help="Override the expected trace mode; must match "
                         "the producer headers")
    args = ap.parse_args()

    expected_mode = None
    if args.mode == "trace":
        expected_mode = CAE_TRACE_MODE_TRACE
    elif args.mode == "checkpoint":
        expected_mode = CAE_TRACE_MODE_CHECKPOINT

    return compare(args.cae, args.nemu, expected_mode)


if __name__ == "__main__":
    sys.exit(main())
