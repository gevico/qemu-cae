# CAE - QEMU 周期近似引擎

CAE (Cycle Approximate Engine) 是基于 QEMU 的时序近似加速器，在 TCG 功能仿真基础上增加逐指令的 cycle charging，实现快速、近似的 CPU 性能建模，无需 gem5 等全周期精确模拟器。

基于 **QEMU v11.0.0**。

## 核心特性

- **比 gem5 快 1.3-3.5 倍**
- **支持顺序和乱序 CPU 模型**（5 级流水线、KMH-V3 乱序）
- **查表驱动 RISC-V 指令分类器**，覆盖 120+ 操作码、21 种编码格式
- **分支预测器**：2-bit local、tournament、TAGE-SC-L、decoupled BPU
- **存储层次**：L1 cache、DRAM、MSHR（含 bank-conflict 模型）、TLB miss hook
- **内置 difftest 框架**，支持与 gem5 的 IPC 精度对比（SE + FS 模式）
- **完整 EEMBC CoreMark** 裸机移植（Apache-2.0 许可）
- **138 个单元测试**，覆盖所有子系统

## 快速开始

### 编译

```bash
mkdir build && cd build
../configure --target-list=riscv64-softmmu --enable-cae
make -j$(nproc)
```

### 运行基准测试

```bash
# CPI=1 模式（无时序模型，最快）
./build/qemu-system-riscv64 -accel cae \
    -machine virt -cpu rv64 -m 256M -nographic \
    -bios <你的二进制文件.riscv>

# 顺序 5 级流水线 + 分支预测 + L1 cache
./build/qemu-system-riscv64 \
    -accel cae,cpu-model=inorder-5stage,\
bpred-model=2bit-local,memory-model=l1-dram \
    -machine virt -cpu rv64 -m 256M -nographic \
    -bios <你的二进制文件.riscv>
```

### 与 gem5 对比精度

```bash
# 通过 Makefile 编译基准测试（使用 configure 生成的配置）
make -C build/tests/cae/riscv64-softmmu build-difftest

# 单个 benchmark 对比
CAE_BUILD_DIR=$(pwd)/build bash tests/cae/riscv64/difftest/scripts/run-all.sh inorder-1c alu

# 完整 6 benchmark 套件
CAE_BUILD_DIR=$(pwd)/build bash tests/cae/riscv64/difftest/scripts/run-suite.sh inorder-1c
```

### 运行单元测试

```bash
meson test -C build qemu:test-cae --print-errorlogs
```

## IPC 精度（CAE vs gem5 MinorCPU SE 模式）

基于 QEMU v11.0.0，`inorder-1c` 配置：

| 测试集 | CAE IPC | gem5 IPC | 误差 | 分支预测率匹配 | CAE 加速比 |
|--------|---------|----------|------|---------------|-----------|
| alu | 1.2495 | 1.2495 | **0.001%** | 0.005% vs 0.015% | 2.7x |
| coremark-1c | 0.7302 | 0.7470 | **2.3%** | 25.2% vs 6.3% | 1.5x |
| pointer-chase | 0.6343 | 0.7361 | **13.8%** | 0.39% vs 0.42% | 2.8x |
| matmul-small | 0.8027 | 1.0175 | **21.1%** | 12.5% vs 0.006% | 1.3x |
| mem-stream | 0.6885 | 0.9018 | **23.7%** | 3.05% vs 3.06% | 3.5x |
| branchy | 0.9046 | 0.6969 | **29.8%** | 20.1% vs 20.0% | 2.5x |

**分析要点：**
- **alu**：近乎完美匹配（纯 ALU 无访存无分支压力）
- **branchy**：分支预测率完美匹配（20.1% vs 20.0%），IPC 差距来自误预测惩罚建模（CAE 静态惩罚 vs gem5 流水线深度感知的 flush）
- **mem-stream/pointer-chase**：差距来自存储延迟建模差异
- 所有测试：**CAE 比 gem5 快 1.3-3.5 倍**

## 加速器参数

通过 `-accel cae,key=value,...` 传递参数：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `cpu-model` | `cpi1` | `cpi1`（CPI=1）、`inorder-5stage`（顺序流水线）、`ooo-kmhv3`（乱序） |
| `bpred-model` | `none` | `none`、`2bit-local`、`tournament`、`tage-sc-l`、`decoupled` |
| `memory-model` | `stub` | `stub`（零延迟）、`l1-dram`（L1+DRAM）、`mshr`（MSHR 模型） |
| `sentinel-addr` | 0 | benchmark 结束检测地址 |
| `latency-mul` | 3 | 乘法指令延迟（周期） |
| `latency-div` | 20 | 除法指令延迟（周期） |
| `latency-fpu` | 4 | 浮点指令延迟（周期） |
| `mispredict-penalty-cycles` | 3 | 分支预测失败惩罚（周期） |
| `l1-size` | 32768 | L1 cache 大小（字节） |
| `l1-assoc` | 2 | L1 cache 相联度 |
| `tlb-miss-cycles` | 0 | TLB miss 惩罚（周期，0=关闭） |
| `sched-issue-ports` | 2 | 调度器发射端口数（乱序模型） |
| `virtual-issue-window` | 0 | 虚拟发射批次窗口（0=关闭） |
| `dependent-load-stall-cycles` | 0 | 依赖 load 序列化惩罚 |

## 技术架构

```
  QEMU TCG（功能仿真）
       |
       v
  [one-insn-per-tb 模式]
       |
       +---> cae_mem_access_notify（softmmu load/store/fetch hook）
       +---> cae_tlb_miss_hook（cputlb.c TLB refill hook）
       |
       v
  CaeUopClassifier -----> op_table[CaeRvOp]（查表驱动，120+ 操作码）
       |
       v
  CaeUop {type, fu_type, codec, flags, src/dst_regs}
       |
       v
  CaeCpuModel.charge()
       |
       +-----> CaeCpuInorder（5 级顺序流水线，可配置延迟）
       +-----> CaeCpuOoo（ROB/IQ/LSQ/RAT，虚拟发射批次）
       |
       v
  CaeEngine（周期计数器 + 事件队列）
       |
       +-----> L1 Cache / DRAM / MSHR（存储时序）
       +-----> 分支预测器（2-bit / tournament / TAGE-SC-L / decoupled BPU）
       +-----> TLB miss 惩罚（来自 softmmu hook）
```

## Difftest 框架

Difftest 框架使用配对 YAML 配置对比 CAE 和 gem5：

```
tests/cae/
├── Makefile.target              # 编排器（configure 时复制到 build 目录）
├── riscv64/
│   ├── Makefile.target          # 用户态测试
│   ├── Makefile.softmmu-target  # 系统态测试
│   ├── Makefile.difftest-target # Difftest 目标
│   └── difftest/
│       ├── benchmarks/          # 7 个 tier-1 benchmark + EEMBC CoreMark
│       │   └── src/             # 源码（汇编 + C）
│       ├── configs/             # 配对 YAML（inorder-1c, xs-1c-kmhv3 等）
│       └── scripts/             # Python/Bash 驱动脚本
```

**构建产物**输出到 `$CAE_BUILD_DIR/tests/cae/difftest/`（镜像源码树结构）。

**gem5 模式：**
- `se`（默认）：SE 模式，使用 `.se.riscv` 二进制 — 用于精确 IPC 对比
- `fs`：FS 裸机模式，使用 `.qemu.riscv` 二进制 — 用于功能验证

## 目录结构

| 目录 | 说明 |
|------|------|
| `accel/cae/` | QEMU 加速器注册和操作 |
| `cae/` | 引擎核心：engine、events、pipeline、uop、checkpoint、trace |
| `include/cae/` | 所有 CAE 接口的公共头文件 |
| `hw/cae/` | 时序模型：CPU（顺序/乱序）、cache、DRAM |
| `hw/cae/ooo/` | 乱序内部模块：ROB、LSQ、RAT、IQ、调度器、store buffer |
| `hw/cae/bpred/` | 分支预测器：BTB、RAS、2-bit local、TAGE-SC-L、decoupled BPU |
| `target/riscv/cae/` | RISC-V 指令分类器、trace、checkpoint |
| `tools/cae/` | gem5/NEMU 版本锁定、XS-GEM5 构建脚本和补丁 |
| `tests/unit/test-cae.c` | 138 个单元测试 |
| `tests/cae/` | Difftest 框架（按架构组织，参考 tests/tcg/ 模式） |

## 许可证

CAE 是 QEMU 的一部分，采用 GPL-2.0-or-later 许可证。
CoreMark vendor 源码位于 `tests/cae/riscv64/difftest/benchmarks/src/coremark/vendor/`，采用 Apache-2.0 许可证（EEMBC）。
