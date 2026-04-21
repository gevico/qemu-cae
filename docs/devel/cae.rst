.. _cae:

==============================
CAE - Cycle Approximate Engine
==============================

CAE is a timing-approximate accelerator for QEMU that adds per-instruction
cycle charging on top of TCG's functional execution. It reuses TCG for
instruction decoding and execution while maintaining a parallel timing model
that tracks cycles, branch predictions, cache behavior, and pipeline state.

.. contents:: Table of Contents
   :local:
   :depth: 2

Overview
========

CAE inherits ``TYPE_TCG_ACCEL`` and forces ``one-insn-per-tb`` mode so that
every guest instruction triggers a TB exit, allowing the timing model to
charge cycles and update microarchitectural state per instruction.

The execution flow for each guest instruction is:

1. **Pre-classify**: Fetch instruction bytes at the current PC and run the
   architecture-specific classifier (``CaeUopClassifier.classify``) to
   populate a ``CaeUop`` with type, functional unit, codec, operand
   registers, and semantic flags.

2. **Execute**: TCG executes the single-instruction TB functionally. Memory
   access hooks (``cae_mem_access_notify``) capture load/store addresses.

3. **Charge**: ``cae_charge_executed_tb()`` invokes the CPU timing model
   to compute cycle cost, updates the engine's global clock, resolves
   branch predictions, and emits retire-side trace records.

This loop repeats for every retired instruction, producing a deterministic
cycle count that can be compared against reference simulators.

Architecture
============

Engine (``CaeEngine``)
----------------------

The engine is the central timing coordinator. It maintains:

- **current_cycle**: Global monotonic clock, incremented by each CPU's
  charge path.
- **event_queue**: A GTree-based priority queue for scheduled events
  (cache completions, timer ticks), ordered by (cycle, sequence) for
  deterministic tie-breaking.
- **mem_backend**: Pluggable memory timing model implementing
  ``CaeMemClass`` (stub, L1+DRAM, or MSHR).
- **cpu_model**: Pluggable CPU timing model implementing
  ``CaeCpuModelClass`` (CPI=1, inorder-5stage, or ooo-kmhv3).
- **bpred**: Pluggable branch predictor implementing ``CaeBPredClass``.
- **Sentinel mechanism**: A memory-address-based freeze gate for
  deterministic benchmark measurement windows.

CPU Pipeline (``CaeCpu``)
-------------------------

Each virtual CPU has a ``CaeCpu`` instance containing:

- **active_uop_pool[64]**: Persistent instruction classification pool.
  The ``active_uop`` pointer tracks the currently-executing instruction.
  Persistence across ``cpu_exec`` slices is required for deterministic
  branch prediction accounting.
- **Statistics**: ``cycle_count``, ``insn_count``, ``stall_cycles``,
  branch prediction counters, L1D hit/miss counters.
- **Speculative state**: Checkpoint save/restore for branch misprediction
  recovery (``spec_snap``, ``spec_squash_sqn``).

Instruction Classification (``CaeUop``)
----------------------------------------

The classifier maps raw instruction bytes to a ``CaeUop`` containing:

- **type**: ``CAE_UOP_ALU``, ``BRANCH``, ``LOAD``, ``STORE``, ``MUL``,
  ``DIV``, ``FPU``, ``SYSTEM``, ``FENCE``, ``ATOMIC``, ``UNKNOWN``.
- **fu_type**: Functional unit assignment for scheduling.
- **codec**: Operand format (R/I/S/B/U/J for base ISA, plus 14 compressed
  formats for RVC).
- **Semantic flags**: ``is_load``, ``is_store``, ``is_branch``,
  ``is_call``, ``is_return``, ``is_indirect``, ``is_conditional``.
- **Operand registers**: ``src_regs[]``, ``dst_regs[]`` with ``x0``
  destination suppression (``num_dst=0`` when ``rd=x0``).

The RISC-V classifier (``target/riscv/cae/cae-riscv-uop.c``) uses a
table-driven design:

- ``CaeRvOp`` enum: Dense enumeration of 120+ opcodes (base I/M/A/F/D +
  RVC).
- ``op_table[CAE_RV_OP_COUNT]``: Static lookup table mapping each opcode
  to ``{uop_type, fu_type, codec, flags}``.
- ``decode_base()`` / ``decode_compressed()``: Bit-level decoders that
  map instruction encoding to ``CaeRvOp``, with illegal encoding
  rejection (reserved branch funct3, invalid FP funct3, reserved
  compressed forms).
- ``extract_operands()``: Single function dispatching by codec to fill
  ``src_regs``/``dst_regs``.

CPU Timing Models
=================

CPI=1 (``cpi1``)
-----------------

Default model. Every instruction costs exactly 1 cycle. No pipeline
simulation. Useful as a functional-only baseline.

In-Order 5-Stage (``inorder-5stage``)
--------------------------------------

Models a classic 5-stage RISC pipeline with configurable latencies:

- **Base latency**: 1 cycle for ALU/branch/load/store.
- **Multi-cycle**: ``latency-mul`` (default 3), ``latency-div`` (20),
  ``latency-fpu`` (4).
- **Overlap**: ``overlap-permille`` controls CPI reduction for
  instruction-level parallelism (0-1000, where 1000 = full overlap).
- **Load-use stall**: ``load-use-stall-cycles`` adds extra cycles when
  a load result is consumed by the next instruction.
- **Branch misprediction penalty**: ``mispredict-penalty-cycles``.

Out-of-Order KMH-V3 (``ooo-kmhv3``)
-------------------------------------

Models an aggressive out-of-order core matching the XiangShan KunMingHu V3
microarchitecture:

- **Rename width**: 8-wide rename, 8-wide commit.
- **ROB**: 352 entries.
- **Load/Store queues**: 72/56 entries.
- **Physical registers**: 224 integer, 256 floating-point.
- **Scheduler**: 3-segment, 64-entry per segment, configurable
  ``sched-issue-ports`` (default 2, max 8).
- **Virtual-issue batching**: Independent consecutive retires get
  ``cycles=0`` (co-issue) within the ``virtual-issue-window``.
- **Dependent-load stall**: ``dependent-load-stall-cycles`` penalty
  when a load's source depends on a prior load's destination.
- **Speculative checkpoint**: Full pipeline state save/restore on
  branch misprediction (ROB, IQ, LSQ, RAT, store buffer, MSHR).
- **Store buffer**: 16-entry with eviction threshold tracking.

Branch Prediction
=================

Four predictor families are available:

- **2-bit local**: Per-PC saturating counter with configurable
  ``local-history-bits``, ``btb-entries``, ``btb-assoc``, ``ras-depth``.
- **Tournament**: Gshare + local + meta chooser.
- **TAGE-SC-L**: Tagged geometric-length predictor with statistical
  corrector and loop predictor.
- **Decoupled BPU**: Separate direction and target predictors, matching
  the XiangShan DecoupledBPU design.

Prediction happens at TB entry (frontend); update happens at TB retire
(backend). This decoupled timing matches real hardware where prediction
runs ahead of execution.

Memory Hierarchy
================

Three memory model tiers:

- **stub**: Zero-latency passthrough (default).
- **l1-dram**: Simple L1 data cache + flat DRAM model. Configurable
  ``l1-size``, ``l1-assoc``, ``l1-line-size``, ``l1-hit-cycles``,
  ``l1-miss-cycles``, ``dram-read-cycles``, ``dram-write-cycles``.
- **mshr**: Miss Status Holding Register model with bank-conflict
  tracking. Configurable ``mshr-size``, ``bank-count``,
  ``bank-conflict-stall-cycles``, ``sbuffer-evict-threshold``.

A separate I-cache model is available for the ``mshr`` tier with
independent size/associativity/latency knobs.

Difftest Infrastructure
=======================

The ``tests/cae-difftest/`` directory contains a production-grade
comparison harness:

**Runners:**

- ``run-cae.py``: Launches QEMU with CAE accel, polls for sentinel,
  reads QMP counters, produces JSON report.
- ``run-gem5.py``: Launches gem5 MinorCPU in SE mode, parses stats.txt.
- ``run-xs-gem5.py``: Launches XiangShan GEM5 with KMH-V3 config.

**Comparison:**

- ``diff.py``: N-side metric comparison (IPC, cycles, misprediction
  rate, L1D MPKI, average load latency) with noise-floor filtering.
- ``ci-gate.py``: Multi-stage CI gate with suite-level max/geomean
  thresholds.

**Configuration:**

Paired YAML files (``configs/*.yaml``) serve as single source of truth
for both CAE and reference simulator parameters. The
``config-equivalence.py`` validator ensures both sides consume the
correct fields.

**Benchmarks:**

Six tier-1 microbenchmarks: ``alu``, ``mem-stream``, ``pointer-chase``,
``branchy``, ``matmul-small``, ``coremark-1c``. Each produces QEMU
(``.qemu.riscv``) and gem5 SE (``.se.riscv``) variants from the same
source, pinned by SHA-256 in ``MANIFEST.json``.

Building and Running
====================

**Configure:**

.. code-block:: bash

   mkdir build-cae && cd build-cae
   ../configure --target-list=riscv64-softmmu --enable-cae

**Build:**

.. code-block:: bash

   make -j$(nproc)

**Run with in-order model:**

.. code-block:: bash

   ./qemu-system-riscv64 \
       -accel cae,cpu-model=inorder-5stage,bpred-model=2bit-local,\
   memory-model=l1-dram,l1-size=32768,l1-assoc=2,\
   mispredict-penalty-cycles=7 \
       -machine virt -cpu rv64 -m 256M -nographic \
       -bios firmware.bin

**Unit tests:**

.. code-block:: bash

   meson test -C build-cae qemu:test-cae --print-errorlogs

**Difftest suite:**

.. code-block:: bash

   make -C tests/cae-difftest/benchmarks/src/
   bash tests/cae-difftest/scripts/run-suite.sh inorder-1c

Adding a New Instruction
========================

With the table-driven classifier, adding a new RISC-V instruction
requires three steps:

1. Add a ``CaeRvOp`` enum value (e.g., ``CAE_RV_OP_SH1ADD``).
2. Add a table entry: ``[CAE_RV_OP_SH1ADD] = ALU_R``.
3. Add a decode case in ``decode_base()`` for the new opcode/funct3/funct7
   combination.

No changes to ``extract_operands()`` are needed if the instruction uses
an existing codec (R/I/S/B/U/J or compressed variants).

Adding a New Target Architecture
=================================

CAE's architecture-neutral design supports multiple ISAs. To add a new
target:

1. Create ``target/<arch>/cae/`` with a ``CaeUopClassifier`` that maps
   the ISA's instruction encoding to ``CaeUop`` fields.
2. Register the classifier via ``cae_uop_register_classifier()`` in a
   ``type_init()`` function.
3. Add the target to ``accelerator_targets`` in ``meson.build``.
4. Optionally implement target-specific trace and checkpoint emitters.
