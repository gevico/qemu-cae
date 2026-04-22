# CAE Difftest Missing Benchmarks Plan

## Goal Description

Add 10 new bare-metal RISC-V benchmarks to the CAE difftest suite, filling coverage gaps in store forwarding, memory-level parallelism, strided access, address aliasing, irregular memory patterns, sorting, recursive calls, and streaming bandwidth. New benchmarks land as `gated: false` initially (not in the AC-4 six-benchmark gate), promoted after characterization. The existing AC-4 suite remains frozen.

## Acceptance Criteria

- AC-1: Each new benchmark compiles to all three tier-1 variants (`.qemu.riscv`, `.se.riscv`, `.xs.bin`)
  - Positive Tests: `make build-all` produces all 30 new artifacts (10 benchmarks x 3 variants)
  - Negative Tests: Missing source file causes build failure, not silent skip

- AC-2: Each benchmark exports a `workload` symbol compatible with `bench.S` harness
  - Positive Tests: `objdump -t <elf> | grep workload` shows a global function symbol
  - Negative Tests: A benchmark with missing `workload` symbol fails at link time

- AC-3: Each benchmark runs to completion on CAE (sentinel detected) within 900s
  - Positive Tests: `run-cae.py inorder-1c --benchmark <name>` produces a valid JSON report with `insns > 0`
  - Negative Tests: A benchmark that never writes sentinel times out with clear error

- AC-4: Each benchmark runs to completion on gem5 SE MinorCPU
  - Positive Tests: `run-gem5.py inorder-1c --benchmark <name>` produces a valid JSON report
  - Negative Tests: A benchmark using M-mode CSR in SE variant fails (must use `#ifdef GEM5_SE` guard)

- AC-5: Each benchmark passes determinism check (two consecutive runs produce identical results)
  - Positive Tests: `determinism-check.sh --mode serial <config> <name>` exits 0
  - Negative Tests: A benchmark with random seed or timer-dependent behavior fails determinism

- AC-6: MANIFEST.json entry for each benchmark includes all required metadata
  - Positive Tests: `ci-gate.py --stage benchmark_manifest` passes with new entries
  - Negative Tests: Entry missing `sentinel_addr` or `sha256` causes manifest check failure

- AC-7: Dynamic instruction count per benchmark is within 50K-5M range
  - Positive Tests: gem5 `simInsts` stat falls within range for each benchmark
  - Negative Tests: A benchmark exceeding 5M insns is flagged for reduction

- AC-8: New benchmarks do NOT modify the AC-4 gated suite
  - Positive Tests: `run-suite.sh inorder-1c` still runs exactly 6 original benchmarks
  - Negative Tests: Adding a benchmark to the AC-4 list without explicit plan approval fails review

- AC-9: Makefile supports generic C benchmark compilation (not per-benchmark bespoke rules)
  - Positive Tests: Adding a new C benchmark requires only a source file and a one-line Makefile entry
  - Negative Tests: A C benchmark that needs custom CFLAGS still compiles via the generic path with overrides

## Path Boundaries

### Upper Bound (Maximum Acceptable Scope)

All 10 benchmarks implemented with full MANIFEST metadata, gem5 SE validation, determinism-checked, and diff reports generated. Generic C benchmark build framework in Makefile. New `inorder-extended` config that includes all 16 benchmarks for optional comprehensive sweeps.

### Lower Bound (Minimum Acceptable Scope)

5 LSU microbenchmarks (store_forward, mlp, stride_walk, overlap_mix, pair_alias) implemented as C + inline asm, all compiling and running on CAE and gem5 SE, with MANIFEST entries (`gated: false`). Makefile extended with generic C pattern. Remaining 5 benchmarks deferred.

### Allowed Choices

- Can use: C with inline asm for LSU micros (exact memory access control); pure C for algorithm benchmarks; `volatile` pointers and memory barriers
- Can use: Custom iteration counts to hit the 50K-5M instruction target
- Cannot use: libc functions (malloc, printf, memcpy from libc); external submodules; floating-point for LSU micros
- Cannot use: M-mode CSR in `.se.riscv` variant (must guard with `#ifdef GEM5_SE`)

## Feasibility Hints and Suggestions

### Conceptual Approach

Each benchmark follows the same pattern:
```c
// lsu_store_forward.c
#include <stdint.h>

#define ITERS 100000

void workload(void)
{
    volatile uint64_t buf[4];
    for (int i = 0; i < ITERS; i++) {
        buf[0] = (uint64_t)i;
        uint64_t v = buf[0];  // store-to-load forward
        buf[1] = v + 1;
        (void)buf[1];
    }
}
```

Generic C build rule in Makefile:
```makefile
C_BENCHMARKS := lsu_store_forward lsu_mlp lsu_stride_walk ...
C_BENCH_SRCS = $(addprefix cbench/,$(addsuffix .c,$(C_BENCHMARKS)))

$(BENCH_OUT)/%.qemu.riscv: cbench/%.c bench.S $(LINK_QEMU) | $(BENCH_OUT)
    $(CC) $(C_BENCH_CFLAGS) -T $(LINK_QEMU) -o $@ bench.S $<
```

### Relevant References

| Path | Description |
|------|-------------|
| `tests/cae/riscv64/difftest/benchmarks/src/bench.S` | Harness: calls `workload`, writes sentinel, exits |
| `tests/cae/riscv64/difftest/benchmarks/src/Makefile` | Build rules, CoreMark C pattern to extend |
| `tests/cae/riscv64/difftest/benchmarks/MANIFEST.json` | Benchmark metadata schema |
| `tests/cae/riscv64/difftest/scripts/run-suite.sh` | Hardcoded AC-4 benchmark list (line 35) |
| `tests/cae/riscv64/difftest/scripts/ci-gate.py` | `_AC4_BENCHMARKS` tuple (line 295) |
| `tests/cae/riscv64/difftest/configs/inorder-1c.yaml` | In-order difftest config |

## Dependencies and Sequence

### Milestones

1. **Build framework**: Generic C benchmark compilation in Makefile
   - Phase A: Add `C_BENCHMARKS` list and generic `%.qemu.riscv` / `%.se.riscv` pattern rules
   - Phase B: Verify with one placeholder benchmark

2. **LSU microbenchmarks** (highest priority, self-contained):
   - `lsu_store_forward` → `lsu_stride_walk` → `lsu_mlp` → `lsu_overlap_mix` → `lsu_pair_alias`
   - Each: write C source → compile → verify on CAE → verify on gem5 SE → add MANIFEST entry

3. **Algorithm benchmarks** (depend on generic C framework):
   - `qsort` → `spmv` → `rsort`

4. **Supplementary benchmarks**:
   - `towers` → `stream_triad`

5. **Validation sweep**: Run all new benchmarks through full difftest pipeline, generate IPC comparison reports

Dependencies: Milestone 1 → Milestones 2/3/4 (parallel within each) → Milestone 5

## Task Breakdown

| Task ID | Description | Target AC | Tag | Depends On |
|---------|-------------|-----------|-----|------------|
| task-T1 | Add generic C benchmark build rules to Makefile | AC-9 | coding | - |
| task-T2 | Implement `lsu_store_forward.c` | AC-1, AC-2, AC-3 | coding | task-T1 |
| task-T3 | Implement `lsu_stride_walk.c` | AC-1, AC-2, AC-3 | coding | task-T1 |
| task-T4 | Implement `lsu_mlp.c` | AC-1, AC-2, AC-3 | coding | task-T1 |
| task-T5 | Implement `lsu_overlap_mix.c` | AC-1, AC-2, AC-3 | coding | task-T1 |
| task-T6 | Implement `lsu_pair_alias.c` | AC-1, AC-2, AC-3 | coding | task-T1 |
| task-T7 | Implement `qsort.c` (clean-room quicksort) | AC-1, AC-2, AC-3 | coding | task-T1 |
| task-T8 | Implement `spmv.c` (sparse matrix-vector multiply) | AC-1, AC-2, AC-3 | coding | task-T1 |
| task-T9 | Implement `rsort.c` (radix sort) | AC-1, AC-2, AC-3 | coding | task-T1 |
| task-T10 | Implement `towers.c` (Tower of Hanoi) | AC-1, AC-2, AC-3 | coding | task-T1 |
| task-T11 | Implement `stream_triad.c` (a[i]=b[i]+s*c[i]) | AC-1, AC-2, AC-3 | coding | task-T1 |
| task-T12 | Add MANIFEST.json entries for all 10 benchmarks | AC-6 | coding | task-T2..T11 |
| task-T13 | Verify all benchmarks on gem5 SE MinorCPU | AC-4 | coding | task-T12 |
| task-T14 | Run determinism check on all benchmarks | AC-5 | coding | task-T13 |
| task-T15 | Verify AC-4 suite unchanged (6 benchmarks frozen) | AC-8 | coding | task-T12 |
| task-T16 | Validate instruction counts within 50K-5M range | AC-7 | analyze | task-T13 |
| task-T17 | Generate IPC comparison reports for all new benchmarks | AC-3, AC-4 | coding | task-T14 |

## Claude-Codex Deliberation

### Agreements

- New benchmarks should land as `gated: false` initially, not destabilizing AC-4
- LSU micros need inline asm or volatile for exact memory access control
- All three variants (qemu, se, xs) required per tier-1 contract
- Generic C build framework needed (not per-benchmark bespoke rules)
- Instruction count 50K-5M is reasonable for one-insn-per-tb mode

### Resolved Disagreements

- **Suite scope**: Codex suggested `inorder-extended` config; Claude agrees — new benchmarks use a separate optional config, AC-4 stays frozen
- **C vs asm for LSU**: Codex flagged compiler reordering risk; resolved by allowing C + inline asm + volatile for LSU micros
- **stream_triad FP scope**: Codex flagged FP widens model mismatch; resolved by marking `uses: ["scalar", "fp"]` and `frontend_sensitive: false`, users can exclude from gates
- **towers frontend_sensitive**: Codex correctly identified RAS dependency; resolved by setting `frontend_sensitive: true` in MANIFEST

### Convergence Status

- Final Status: `partially_converged` (direct mode, no second Codex pass)

## Pending User Decisions

- DEC-1: Primary target gate
  - Claude Position: Land as `gated: false`, create optional `inorder-extended` config for sweeps
  - Codex Position: N/A - open question
  - Tradeoff Summary: Gating immediately risks destabilizing AC-4 baseline; staged promotion is safer
  - Decision Status: PENDING

- DEC-2: spmv/qsort provenance
  - Claude Position: Clean-room reimplementation (no riscv-tests dependency per draft constraint)
  - Codex Position: Need precise provenance rule for adapted code
  - Tradeoff Summary: Clean-room avoids license issues but may diverge from canonical behavior
  - Decision Status: PENDING

## Implementation Notes

### Code Style Requirements

- Implementation code and comments must NOT contain plan-specific terminology such as "AC-", "Milestone", "Step", "Phase", or similar workflow markers
- These terms are for plan documentation only, not for the resulting codebase
- Use descriptive, domain-appropriate naming in code instead
- Each benchmark source file should have a brief header comment describing what microarchitectural behavior it exercises

--- Original Design Draft Start ---

# 补充 CAE Difftest 缺失的 Benchmark

## 背景

通过分析 risc-v-simulator 项目支持的 30 个 benchmark，发现 CAE 当前的 6 个 gated benchmark 存在多个微架构测试盲区。

## 需要补充的 Case（按优先级排序）

### 最高优先级：5 个 LSU 微基准

这些直接测试 CAE 存储层次建模的核心功能：

1. **lsu_store_forward** — Store-to-load forwarding 测试
   - 连续 store + load 同一地址，验证 store buffer forwarding 行为
   - CAE 完全缺失此类测试

2. **lsu_mlp** — Memory-Level Parallelism 测试
   - 8 路独立 load stream 并发，压测 MSHR 并发处理能力
   - CAE 完全缺失此类测试

3. **lsu_stride_walk** — 跨步访存测试
   - stride-16 跨步访问 32K 数组，压测 prefetcher 和 cache line 利用率
   - 补充 mem-stream（unit stride）的覆盖

4. **lsu_overlap_mix** — 部分重叠 store→load 转发测试
   - store narrow + load wide 重叠，测试 store buffer merging 和 forwarding corner case
   - CAE 完全缺失

5. **lsu_pair_alias** — 地址别名测试
   - 交替 aliased/non-aliased store+load 对，测试 store 消歧
   - CAE 完全缺失

### 高优先级：2 个 riscv-tests benchmark

6. **spmv** — 稀疏矩阵向量乘
   - 不规则间接访存模式，pointer-chase 和 mem-stream 都无法覆盖

7. **qsort** — 快速排序
   - 不可预测分支 + 指针密集访存，桥接 branchy 和 pointer-chase

### 中等优先级：3 个补充测试

8. **stream_triad** — STREAM Triad (a[i]=b[i]+s*c[i])
   - 3 路带宽 + FMA，补充 STREAM 系列

9. **towers** — 汉诺塔递归
   - 递归调用密集，压测 RAS (Return Address Stack)

10. **rsort** — 基数排序
    - 流式内存模式，最小分支，补充 mem-stream 的变体

## 实现方式

所有 benchmark 都是裸机 RISC-V C 程序：
- 编译为 `.qemu.riscv`（bare-metal）和 `.se.riscv`（gem5 SE）两个变体
- 使用现有 `bench.S` harness（ebreak sentinel 终止）
- 添加到 `MANIFEST.json` 作为 tier-1 gated benchmark
- 在 `inorder-1c.yaml` 配置下与 gem5 MinorCPU 对比

## 约束

- 每个 benchmark 的动态指令数控制在 100K-5M（one-insn-per-tb 模式下可接受）
- 不修改现有 6 个 benchmark 的行为
- 复用现有 Makefile 编译框架，扩展 C 编译支持
- 不引入外部依赖（riscv-tests、Embench 等外部子模块暂不考虑）

--- Original Design Draft End ---
