"""gem5 FS-mode (bare-metal) config for CAE difftest.

Runs a bare-metal RISC-V ELF on gem5 in full-system mode. The binary
is loaded at 0x80000000 (matching the CAE linker script). The CPU
starts at the reset vector and runs until max_ticks is reached or the
simulation exits.

Termination: The benchmark writes a sentinel value and enters a wfi
halt loop. gem5 detects the CPU going idle (no more instructions to
execute) and exits the simulation.
"""

import argparse
import sys

import m5
from m5.objects import (
    AddrRange,
    AtomicSimpleCPU,
    Cache,
    Root,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    TimingSimpleCPU,
    VoltageDomain,
)

try:
    from m5.objects import RiscvMinorCPU
except ImportError:
    RiscvMinorCPU = None

try:
    from m5.objects import RiscvBareMetal
except ImportError:
    RiscvBareMetal = None


class L1ICache(Cache):
    size = "32kB"
    assoc = 2
    tag_latency = 1
    data_latency = 1
    response_latency = 1
    mshrs = 4
    tgts_per_mshr = 20
    writeback_clean = False


class L1DCache(Cache):
    size = "32kB"
    assoc = 2
    tag_latency = 1
    data_latency = 1
    response_latency = 1
    mshrs = 4
    tgts_per_mshr = 20
    writeback_clean = True


def _bool_flag(v: str) -> bool:
    return str(v).lower() in ("1", "true", "yes", "on")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--cpu-type", required=True,
                    choices=["AtomicCPU", "TimingSimpleCPU", "MinorCPU"])
    ap.add_argument("--clock-hz", type=int, default=1_000_000_000)
    ap.add_argument("--mem-bytes", type=int, default=256 * 1024 * 1024)
    ap.add_argument("--mem-mode", default="atomic",
                    choices=["atomic", "timing"])
    ap.add_argument("--dram-latency-ns", type=int, default=50)
    ap.add_argument("--xbar-response-ns", type=int, default=0)
    ap.add_argument("--enable-l1", type=_bool_flag, default=False)
    ap.add_argument("--l1i-size", default="32kB")
    ap.add_argument("--l1i-assoc", type=int, default=2)
    ap.add_argument("--l1i-hit-cycles", type=int, default=1)
    ap.add_argument("--l1i-miss-cycles", type=int, default=10)
    ap.add_argument("--l1d-size", default="32kB")
    ap.add_argument("--l1d-assoc", type=int, default=2)
    ap.add_argument("--l1d-hit-cycles", type=int, default=1)
    ap.add_argument("--l1d-miss-cycles", type=int, default=10)
    ap.add_argument("--max-ticks", type=int, default=10_000_000_000)
    ap.add_argument("--max-insts", type=int, default=0)
    args = ap.parse_args()

    system = System()
    system.clk_domain = SrcClockDomain(
        clock=f"{args.clock_hz}Hz",
        voltage_domain=VoltageDomain(),
    )
    system.mem_mode = args.mem_mode
    system.mem_ranges = [AddrRange(0, size=f"0x{0x80000000 + args.mem_bytes:x}B")]

    if args.cpu_type == "AtomicCPU":
        system.cpu = AtomicSimpleCPU()
    elif args.cpu_type == "TimingSimpleCPU":
        system.cpu = TimingSimpleCPU()
    elif RiscvMinorCPU is not None:
        system.cpu = RiscvMinorCPU()
    else:
        print("MinorCPU not available, falling back to TimingSimpleCPU",
              file=sys.stderr)
        system.cpu = TimingSimpleCPU()

    system.membus = SystemXBar(
        frontend_latency=args.xbar_response_ns,
        forward_latency=args.xbar_response_ns,
        response_latency=args.xbar_response_ns,
    )

    if args.enable_l1:
        system.cpu.icache = L1ICache(
            size=args.l1i_size,
            assoc=args.l1i_assoc,
            tag_latency=args.l1i_hit_cycles,
            data_latency=args.l1i_hit_cycles,
            response_latency=args.l1i_miss_cycles,
        )
        system.cpu.dcache = L1DCache(
            size=args.l1d_size,
            assoc=args.l1d_assoc,
            tag_latency=args.l1d_hit_cycles,
            data_latency=args.l1d_hit_cycles,
            response_latency=args.l1d_miss_cycles,
        )
        system.cpu.icache.cpu_side = system.cpu.icache_port
        system.cpu.dcache.cpu_side = system.cpu.dcache_port
        system.cpu.icache.mem_side = system.membus.cpu_side_ports
        system.cpu.dcache.mem_side = system.membus.cpu_side_ports
    else:
        system.cpu.icache_port = system.membus.cpu_side_ports
        system.cpu.dcache_port = system.membus.cpu_side_ports

    system.cpu.createInterruptController()

    mem_latency = 0 if args.mem_mode == "atomic" else args.dram_latency_ns
    system.mem_ctrl = SimpleMemory(
        range=system.mem_ranges[0],
        latency=f"{mem_latency}ns",
    )
    system.mem_ctrl.port = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports

    if RiscvBareMetal is not None:
        system.workload = RiscvBareMetal()
        system.workload.bootloader = args.binary
    else:
        from m5.objects import RiscvBootloaderKernelWorkload
        system.workload = RiscvBootloaderKernelWorkload(bootloader=args.binary)

    if args.max_insts > 0:
        system.cpu.max_insts_any_thread = args.max_insts

    system.cpu.createThreads()

    root = Root(full_system=True, system=system)
    m5.instantiate()

    caches_str = "L1I+L1D" if args.enable_l1 else "no-caches"
    max_str = f"max_insts={args.max_insts}" if args.max_insts > 0 \
              else f"max_ticks={args.max_ticks}"
    print(
        f"gem5-fs-config: starting {args.cpu_type} bare-metal simulation of "
        f"{args.binary} ({caches_str}, mem_mode={args.mem_mode}, "
        f"dram_latency={mem_latency}ns, {max_str})",
        flush=True,
    )
    exit_event = m5.simulate(args.max_ticks)
    print(
        f"gem5-fs-config: exit tick={m5.curTick()} "
        f"reason={exit_event.getCause()}",
        flush=True,
    )


if __name__ == "__main__":
    main()
else:
    main()
