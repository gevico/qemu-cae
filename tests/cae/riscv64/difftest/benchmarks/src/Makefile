# Difftest microbenchmarks — shared build rules.
#
# Assembly benchmarks: each .S produces three variants via bench.S harness.
# C benchmarks (CoreMark): compiled from C sources, linked with bench.S.

CROSS_COMPILE ?= riscv64-unknown-linux-gnu-
CC            := $(CROSS_COMPILE)gcc
OBJCOPY       := $(CROSS_COMPILE)objcopy
OBJDUMP       := $(CROSS_COMPILE)objdump

SRC_DIR       := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
REPO_ROOT     ?= $(abspath $(SRC_DIR)/../../../../../..)
CAE_BUILD_DIR ?= $(REPO_ROOT)/build
BENCH_OUT     ?= $(CAE_BUILD_DIR)/tests/cae/difftest/benchmarks
LINK_QEMU     := link.ld
LINK_SE       := link-se.ld

COMMON_FLAGS  := -march=rv64gc -mabi=lp64d -mcmodel=medany \
                 -nostdlib -nostartfiles -static \
                 -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
                 -Wl,--build-id=none -Os -g0

CFLAGS        := $(COMMON_FLAGS)

# Assembly benchmarks
ASM_BENCHMARKS := alu mem-stream pointer-chase branchy matmul-small coremark-1c \
                  spec-mem-leak-check checkpoint-stress
QEMU_ELFS     := $(addprefix $(BENCH_OUT)/,$(addsuffix .qemu.riscv,$(ASM_BENCHMARKS)))
SE_ELFS       := $(addprefix $(BENCH_OUT)/,$(addsuffix .se.riscv,$(ASM_BENCHMARKS)))
XS_BINS       := $(addprefix $(BENCH_OUT)/,$(addsuffix .xs.bin,$(ASM_BENCHMARKS)))
ELFS          := $(QEMU_ELFS) $(SE_ELFS)
DISASMS       := $(ELFS:.riscv=.dis)

# CoreMark full benchmark (C sources)
COREMARK_DIR  := coremark
COREMARK_VENDOR := $(COREMARK_DIR)/vendor
COREMARK_SRCS := $(COREMARK_VENDOR)/core_main.c \
                 $(COREMARK_VENDOR)/core_list_join.c \
                 $(COREMARK_VENDOR)/core_matrix.c \
                 $(COREMARK_VENDOR)/core_state.c \
                 $(COREMARK_VENDOR)/core_util.c \
                 $(COREMARK_DIR)/ee_printf_stub.c \
                 $(COREMARK_DIR)/core_portme.c \
                 $(COREMARK_DIR)/coremark_workload.c

COREMARK_CFLAGS := $(COMMON_FLAGS) \
                   -I$(COREMARK_DIR) -I$(COREMARK_VENDOR) \
                   -DPERFORMANCE_RUN=1 -DITERATIONS=1 \
                   -DFLAGS_STR=\"rv64gc-Os-cae\" \
                   -DCOMPILER_FLAGS=\"rv64gc-Os\" \
                   '-DMEM_LOCATION="STACK"' \
                   -Wno-missing-prototypes \
                   -Wno-implicit-function-declaration \
                   -Wno-int-to-pointer-cast

COREMARK_TARGETS := $(BENCH_OUT)/coremark-full.qemu.riscv \
                    $(BENCH_OUT)/coremark-full.se.riscv \
                    $(BENCH_OUT)/coremark-full.xs.bin

# Generic C benchmarks (cbench/ subdirectory)
C_BENCHMARKS := lsu_store_forward lsu_stride_walk lsu_mlp \
                lsu_overlap_mix lsu_pair_alias \
                qsort spmv rsort towers stream_triad

C_BENCH_CFLAGS := $(COMMON_FLAGS) -O1 -fno-builtin

C_QEMU_ELFS := $(addprefix $(BENCH_OUT)/,$(addsuffix .qemu.riscv,$(C_BENCHMARKS)))
C_SE_ELFS   := $(addprefix $(BENCH_OUT)/,$(addsuffix .se.riscv,$(C_BENCHMARKS)))
C_XS_BINS   := $(addprefix $(BENCH_OUT)/,$(addsuffix .xs.bin,$(C_BENCHMARKS)))
C_ELFS      := $(C_QEMU_ELFS) $(C_SE_ELFS)
C_DISASMS   := $(C_ELFS:.riscv=.dis)

.PHONY: build-all xs-bins clean

build-all: $(ELFS) $(DISASMS) $(XS_BINS) $(COREMARK_TARGETS) \
           $(C_ELFS) $(C_DISASMS) $(C_XS_BINS)

xs-bins: $(XS_BINS) $(BENCH_OUT)/coremark-full.xs.bin

$(BENCH_OUT):
	mkdir -p $@

# --- Assembly benchmarks ---

$(BENCH_OUT)/%.qemu.riscv: %.S bench.S $(LINK_QEMU) | $(BENCH_OUT)
	$(CC) $(CFLAGS) -T $(LINK_QEMU) -o $@ bench.S $<

$(BENCH_OUT)/%.se.riscv: %.S bench.S $(LINK_SE) | $(BENCH_OUT)
	$(CC) $(CFLAGS) -DGEM5_SE -T $(LINK_SE) -o $@ bench.S $<

$(BENCH_OUT)/%.xs.bin: $(BENCH_OUT)/%.qemu.riscv | $(BENCH_OUT)
	$(OBJCOPY) -O binary $< $@

%.dis: %.riscv
	$(OBJDUMP) -d $< >$@

# --- Generic C benchmarks (cbench/) ---

$(BENCH_OUT)/%.qemu.riscv: cbench/%.c bench.S $(LINK_QEMU) | $(BENCH_OUT)
	$(CC) $(C_BENCH_CFLAGS) -T $(LINK_QEMU) -o $@ bench.S $<

$(BENCH_OUT)/%.se.riscv: cbench/%.c bench.S $(LINK_SE) | $(BENCH_OUT)
	$(CC) $(C_BENCH_CFLAGS) -DGEM5_SE -T $(LINK_SE) -o $@ bench.S $<

# --- CoreMark full (C benchmark) ---

$(BENCH_OUT)/coremark-full.qemu.riscv: $(COREMARK_SRCS) bench.S $(LINK_QEMU) | $(BENCH_OUT)
	$(CC) $(COREMARK_CFLAGS) -T $(LINK_QEMU) -o $@ bench.S $(COREMARK_SRCS)

$(BENCH_OUT)/coremark-full.se.riscv: $(COREMARK_SRCS) bench.S $(LINK_SE) | $(BENCH_OUT)
	$(CC) $(COREMARK_CFLAGS) -DGEM5_SE -T $(LINK_SE) -o $@ bench.S $(COREMARK_SRCS)

clean:
	rm -rf $(BENCH_OUT)
