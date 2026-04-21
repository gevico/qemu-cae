# CAE - Cycle Approximate Engine for QEMU

CAE is a timing-approximate accelerator for QEMU that adds per-instruction cycle charging on top of TCG's functional execution. It enables fast, approximate CPU performance modeling without requiring a full cycle-accurate simulator like gem5.

Based on **QEMU v11.0.0**.

## Key Features

- **1.3-3.5x faster than gem5** on equivalent workloads
- **In-order and Out-of-Order CPU models** (5-stage pipeline, KMH-V3 OoO)
- **Table-driven RISC-V instruction classifier** with 120+ opcodes, 21 codec formats
- **Branch predictors**: 2-bit local, tournament, TAGE-SC-L, decoupled BPU
- **Memory hierarchy**: L1 cache, DRAM, MSHR with bank-conflict model, TLB miss hook
- **Built-in difftest infrastructure** for IPC comparison against gem5 (SE + FS modes)
- **Full EEMBC CoreMark** bare-metal port (Apache-2.0 vendor sources)
- **138 unit tests** covering all CAE subsystems

## Quick Start

### Build

```bash
mkdir build && cd build
../configure --target-list=riscv64-softmmu --enable-cae
make -j$(nproc)
```

### Run a Benchmark

```bash
# CPI=1 mode (no timing model, fastest)
./build/qemu-system-riscv64 -accel cae \
    -machine virt -cpu rv64 -m 256M -nographic \
    -bios <your-binary.riscv>

# In-order 5-stage pipeline with branch prediction + L1 cache
./build/qemu-system-riscv64 \
    -accel cae,cpu-model=inorder-5stage,\
bpred-model=2bit-local,memory-model=l1-dram \
    -machine virt -cpu rv64 -m 256M -nographic \
    -bios <your-binary.riscv>
```

### Run Difftest Against gem5

```bash
# Build benchmarks via the Makefile (uses configure-generated config)
make -C build/tests/cae/riscv64-softmmu build-difftest

# Run single benchmark comparison
CAE_BUILD_DIR=$(pwd)/build bash tests/cae/riscv64/difftest/scripts/run-all.sh inorder-1c alu

# Run full 6-benchmark suite
CAE_BUILD_DIR=$(pwd)/build bash tests/cae/riscv64/difftest/scripts/run-suite.sh inorder-1c
```

### Run Unit Tests

```bash
meson test -C build qemu:test-cae --print-errorlogs
```

## IPC Accuracy (CAE vs gem5 MinorCPU SE mode)

Measured on QEMU v11.0.0 with `inorder-1c` configuration:

| Benchmark | CAE IPC | gem5 IPC | Error | Mispred Match | CAE Speedup |
|-----------|---------|----------|-------|---------------|-------------|
| alu | 1.2495 | 1.2495 | **0.001%** | 0.005% vs 0.015% | 2.7x |
| coremark-1c | 0.7302 | 0.7470 | **2.3%** | 25.2% vs 6.3% | 1.5x |
| pointer-chase | 0.6343 | 0.7361 | **13.8%** | 0.39% vs 0.42% | 2.8x |
| matmul-small | 0.8027 | 1.0175 | **21.1%** | 12.5% vs 0.006% | 1.3x |
| mem-stream | 0.6885 | 0.9018 | **23.7%** | 3.05% vs 3.06% | 3.5x |
| branchy | 0.9046 | 0.6969 | **29.8%** | 20.1% vs 20.0% | 2.5x |

**Key observations:**
- **alu**: Near-perfect IPC match (pure ALU, no memory or branch pressure)
- **coremark-1c**: Good accuracy; misprediction rate difference is the main gap
- **branchy**: Misprediction rate matches perfectly (20.1% vs 20.0%); IPC gap is from misprediction penalty modeling (static penalty vs gem5's pipeline-depth-aware flush)
- **mem-stream/pointer-chase**: Gap from memory latency modeling differences
- All benchmarks: **CAE is 1.3-3.5x faster** than gem5 in wall-clock time

## CAE Accel Properties

Pass properties via `-accel cae,key=value,...`:

| Property | Default | Description |
|----------|---------|-------------|
| `cpu-model` | `cpi1` | `cpi1`, `inorder-5stage`, or `ooo-kmhv3` |
| `bpred-model` | `none` | `none`, `2bit-local`, `tournament`, `tage-sc-l`, `decoupled` |
| `memory-model` | `stub` | `stub`, `l1-dram`, or `mshr` |
| `sentinel-addr` | 0 | Memory address for benchmark completion detection |
| `latency-mul` | 3 | Multiply instruction latency (cycles) |
| `latency-div` | 20 | Divide instruction latency (cycles) |
| `latency-fpu` | 4 | FP instruction latency (cycles) |
| `mispredict-penalty-cycles` | 3 | Branch misprediction penalty (cycles) |
| `l1-size` | 32768 | L1 cache size in bytes |
| `l1-assoc` | 2 | L1 cache associativity |
| `tlb-miss-cycles` | 0 | TLB miss penalty (cycles, 0=disabled) |
| `sched-issue-ports` | 2 | Scheduler issue ports (OoO model) |
| `virtual-issue-window` | 0 | Virtual-issue batching window (0=disabled) |
| `dependent-load-stall-cycles` | 0 | Dependent-load serialization penalty |

## Architecture

```
  QEMU TCG (functional execution)
       |
       v
  [one-insn-per-tb mode]
       |
       +---> cae_mem_access_notify (load/store/fetch hook from softmmu)
       +---> cae_tlb_miss_hook (TLB refill hook from cputlb.c)
       |
       v
  CaeUopClassifier -----> op_table[CaeRvOp] (table-driven, 120+ opcodes)
       |
       v
  CaeUop {type, fu_type, codec, flags, src/dst_regs}
       |
       v
  CaeCpuModel.charge()
       |
       +-----> CaeCpuInorder (5-stage pipeline, configurable latencies)
       +-----> CaeCpuOoo (ROB/IQ/LSQ/RAT, virtual-issue batching)
       |
       v
  CaeEngine (cycle counter + event queue)
       |
       +-----> L1 Cache / DRAM / MSHR (memory timing)
       +-----> Branch Predictor (2-bit / tournament / TAGE-SC-L / decoupled BPU)
       +-----> TLB miss penalty (from softmmu hook)
```

## Difftest Infrastructure

The difftest framework compares CAE against gem5 using paired YAML configurations:

```
tests/cae/
├── Makefile.target              # Orchestrator (copied to build dir by configure)
├── riscv64/
│   ├── Makefile.target          # User-mode tests
│   ├── Makefile.softmmu-target  # System-mode tests
│   ├── Makefile.difftest-target # Difftest targets
│   └── difftest/
│       ├── benchmarks/          # 7 tier-1 benchmarks + EEMBC CoreMark
│       │   └── src/             # Source (assembly + C)
│       ├── configs/             # Paired YAML (inorder-1c, xs-1c-kmhv3, ...)
│       └── scripts/             # Python/Bash drivers
```

**Build artifacts** go to `$CAE_BUILD_DIR/tests/cae/difftest/` (mirrors source tree).

**Supported gem5 modes:**
- `se` (default): SE mode with `.se.riscv` binaries — accurate IPC comparison
- `fs`: FS bare-metal mode with `.qemu.riscv` binaries — functional verification

**Benchmarks:**

| Benchmark | Instructions | Description | Gated |
|-----------|-------------|-------------|-------|
| alu | ~1M | Pure ALU tight loop, no memory | Yes |
| mem-stream | ~230K | Sequential load/store stream | Yes |
| pointer-chase | ~388K | Dependent load chain (linked list) | Yes |
| branchy | ~1.15M | Unpredictable xorshift branches (~50% taken) | Yes |
| matmul-small | ~8.6M | 8x8 integer matrix multiply | Yes |
| coremark-1c | ~5.1M | Reduced CoreMark-style core loop | Yes |
| coremark-full | — | Full EEMBC CoreMark (bare-metal, Apache-2.0) | No* |

*coremark-full requires multi-insn-per-tb or sampling for automated difftest.

## Project Structure

| Directory | Description |
|-----------|-------------|
| `accel/cae/` | QEMU accelerator registration and ops |
| `cae/` | Engine core: engine, events, pipeline, uop, checkpoint, trace |
| `include/cae/` | Public headers for all CAE interfaces |
| `hw/cae/` | Timing models: CPU (inorder, OoO), cache, DRAM |
| `hw/cae/ooo/` | OoO internals: ROB, LSQ, RAT, IQ, scheduler, store buffer |
| `hw/cae/bpred/` | Branch predictors: BTB, RAS, 2-bit local, TAGE-SC-L, decoupled BPU |
| `target/riscv/cae/` | RISC-V instruction classifier, trace, checkpoint |
| `tools/cae/` | gem5/NEMU version pins, XS-GEM5 build scripts and patches |
| `tests/unit/test-cae.c` | 138 unit tests |
| `tests/cae/` | Difftest framework (per-arch, following tests/tcg/ pattern) |

## License

CAE is part of QEMU and licensed under GPL-2.0-or-later.
CoreMark vendor sources under `tests/cae/riscv64/difftest/benchmarks/src/coremark/vendor/` are licensed under Apache-2.0 (EEMBC).
