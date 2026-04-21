/*
 * CAE (Cycle Approximate Engine) — NEMU functional difftest trace format
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

/*
 * Binary layout of the retired-instruction trace and periodic
 * architectural-state checkpoint files produced on both the CAE side
 * (at the per-target cae-cpu retire boundary) and the NEMU side (via
 * tools/nemu-itrace.patch applied into <repo>/build/nemu-ref/).
 * scripts/nemu-difftest.py byte-compares two files that both start
 * with CaeTraceHeader.
 *
 * The layout is arch-neutral. Guest-ISA specifics live in the
 * per-target CAE emitter; this header only declares the on-disk
 * shape and the small set of self-describing mode / version /
 * record_size constants the consumer uses to reject an incompatible
 * producer before any byte-diff begins (AC-K-2.1).
 *
 * All multi-byte fields are little-endian. Padding inside records is
 * explicit via the reserved fields to avoid depending on struct
 * layout of the host compiler when the emitter and consumer live in
 * different processes (CAE / NEMU / nemu-difftest.py).
 */

#ifndef CAE_TRACE_H
#define CAE_TRACE_H

#include <stdint.h>

#define CAE_TRACE_MAGIC            0x49454143u   /* 'CAEI' little-endian */
#define CAE_TRACE_VERSION          1u
#define CAE_TRACE_ENDIAN_LITTLE    1u

/* File-wide operation mode. Producer and consumer must agree. */
#define CAE_TRACE_MODE_TRACE       0u   /* tier-1: one record per retire */
#define CAE_TRACE_MODE_CHECKPOINT  1u   /* tier-2: one record per 1M retires */

/*
 * ISA subset bitmask. A benchmark is only eligible for byte-diff when
 * producer's isa_subset == consumer's isa_subset. RVV workloads are
 * explicitly out-of-scope for v1 (AC-K-1.1); trace v2 will extend.
 */
#define CAE_TRACE_ISA_RV64GC       (1u << 0)
#define CAE_TRACE_ISA_B            (1u << 1)
#define CAE_TRACE_ISA_ZIFENCEI     (1u << 2)
#define CAE_TRACE_ISA_SCALAR_FP    (1u << 3)
#define CAE_TRACE_ISA_AMO          (1u << 4)
/* Reserved for future: CAE_TRACE_ISA_RVV = (1u << 5) when trace v2 lands. */

/*
 * Per-record flag bits.
 *
 *   split   1 = the architectural access crosses a natural boundary
 *               (page / cache line) but the retire boundary still
 *               collapses both halves into one record. Both sides
 *               must report 1 or both must report 0.
 *   excn    1 = an architectural exception was taken on this insn
 *               (illegal opcode, page fault, ecall); rd_value /
 *               mem_value fields are ignored by the consumer.
 *   spec    1 = this record belongs to a speculatively retired
 *               instruction that was later squashed. CAE emits these
 *               only in --full-trace debug mode; NEMU never emits
 *               them. Consumers reject mismatched spec bits.
 *   scfail  1 = store-conditional failure; rd_value carries 1 and
 *               mem_value is ignored.
 */
#define CAE_TRACE_FLAG_MEM_WRITE   (1u << 0)   /* has architectural store */
#define CAE_TRACE_FLAG_EXCEPTION   (1u << 1)   /* trap taken on this insn */
#define CAE_TRACE_FLAG_SPLIT       (1u << 2)
#define CAE_TRACE_FLAG_SPEC_REPLAY (1u << 3)
#define CAE_TRACE_FLAG_SCFAIL      (1u << 4)

/*
 * File header. 16 bytes, little-endian. Every trace or checkpoint
 * file starts with one instance; the consumer rejects unsupported
 * magic / version / endianness / mode / record_size / isa_subset
 * before inspecting any record.
 *
 * reserved is explicit padding so the struct layout is well-defined
 * across compilers without relying on attribute(packed); the emitter
 * and consumer write zero bytes here.
 */
typedef struct CaeTraceHeader {
    uint32_t magic;         /* CAE_TRACE_MAGIC */
    uint16_t version;       /* CAE_TRACE_VERSION */
    uint8_t  endianness;    /* CAE_TRACE_ENDIAN_LITTLE for v1 */
    uint8_t  mode;          /* CAE_TRACE_MODE_{TRACE,CHECKPOINT} */
    uint16_t record_size;   /* 40 for trace mode; sizeof checkpoint record */
    uint16_t reserved;      /* padding, must be 0 */
    uint32_t isa_subset;    /* CAE_TRACE_ISA_* bitmask */
} CaeTraceHeader;

/*
 * Trace mode record. 40 bytes, little-endian. Emitted once per
 * retired architectural instruction when mode == CAE_TRACE_MODE_TRACE.
 *
 * One record per architectural insn means:
 *   - misaligned single-store crossing a page boundary: 1 record with
 *     flags.split=1 and mem_value covering the architectural width
 *   - LR/SC pair: 2 records (one per architectural instruction)
 *   - failed SC: 1 record with flags.scfail=1, rd_value=1
 *   - AMO: 1 record; mem_value is the post-AMO memory value
 *
 * AC-K-2.4 covers the semantics above; the emitter and difftest tool
 * both enforce them.
 */
typedef struct CaeTraceRecord {
    uint64_t pc;          /* architectural PC of the retired insn */
    uint32_t opcode;      /* raw 32-bit opcode (RVC is zero-extended) */
    uint8_t  rd_idx;      /* 0..31, 0xFF = no GPR write-back */
    uint8_t  flags;       /* CAE_TRACE_FLAG_* bitmask */
    uint16_t mem_size;    /* 0 | 1 | 2 | 4 | 8 — 0 when no mem write */
    uint64_t rd_value;    /* post-write GPR value (0 when rd_idx=0xFF) */
    uint64_t mem_addr;    /* 0 when no mem write */
    uint64_t mem_value;   /* store data, AMO result; up to 8 B */
} CaeTraceRecord;

/*
 * Checkpoint mode record. 608 bytes, little-endian. Emitted every
 * CAE_TRACE_CHECKPOINT_INTERVAL_DEFAULT retires when mode ==
 * CAE_TRACE_MODE_CHECKPOINT, bounding tier-2 trace volume per AC-K-2.
 *
 * Field order mirrors NEMU's runtime architectural state so the
 * emitter can memcpy-style fill the record on both sides. The memory
 * hash is a 64-bit FNV-1a over the 1 MiB window starting at
 * MANIFEST.reset_pc (the per-target checkpoint emitter and the NEMU
 * itrace patch compute the same hash byte-for-byte so the field
 * compares equal on a deterministic workload). Consumers may skip
 * the hash via --skip-mem-hash on nemu-difftest.py (still
 * exit-code-0 when every other field matches).
 */
#define CAE_TRACE_CHECKPOINT_INTERVAL_DEFAULT  1000000u  /* AC-K-2 tier-2 */
#define CAE_TRACE_CHECKPOINT_GPR_COUNT         32u
#define CAE_TRACE_CHECKPOINT_FPR_COUNT         32u
#define CAE_TRACE_CHECKPOINT_CSR_COUNT         8u        /* frm, fflags,
                                                          * fcsr, mstatus,
                                                          * mepc, mcause,
                                                          * mie, priv */

typedef struct CaeTraceCheckpointRecord {
    uint64_t retire_index;                              /* 1M-aligned */
    uint64_t pc;
    uint64_t gpr[CAE_TRACE_CHECKPOINT_GPR_COUNT];       /* architectural int regs */
    uint64_t fpr[CAE_TRACE_CHECKPOINT_FPR_COUNT];       /* arch FP regs (64-bit wide) */
    uint64_t csrs[CAE_TRACE_CHECKPOINT_CSR_COUNT];      /* selected CSRs */
    uint64_t memory_hash;                               /* FNV-1a 64-bit over MBASE+1MiB */
    uint32_t flags;                                     /* 0 for v1 */
    uint32_t reserved;                                  /* padding to multiple of 8 */
} CaeTraceCheckpointRecord;

/*
 * Compile-time layout guards. If any platform miscompiles these, the
 * producer and consumer would silently disagree on byte-diff bounds —
 * better to fail the build now.
 */
_Static_assert(sizeof(CaeTraceHeader) == 16,
               "CaeTraceHeader must be exactly 16 bytes");
_Static_assert(sizeof(CaeTraceRecord) == 40,
               "CaeTraceRecord must be exactly 40 bytes");
_Static_assert(sizeof(CaeTraceCheckpointRecord) ==
               8 +                                              /* retire_index */
               8 +                                              /* pc */
               8 * CAE_TRACE_CHECKPOINT_GPR_COUNT +
               8 * CAE_TRACE_CHECKPOINT_FPR_COUNT +
               8 * CAE_TRACE_CHECKPOINT_CSR_COUNT +
               8 +                                              /* memory_hash */
               4 + 4,                                           /* flags + reserved */
               "CaeTraceCheckpointRecord size drifted; update header + emitter");

#endif /* CAE_TRACE_H */
