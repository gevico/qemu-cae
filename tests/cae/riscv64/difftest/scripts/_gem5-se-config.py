"""gem5 SE-mode config generator for the CAE difftest pipeline.

Invoked by run-gem5.py. gem5.opt executes this script and forwards
every flag after the script name as CLI args.

Expected arguments:
    --binary PATH               RISC-V ELF to run in SE mode
    --cpu-type {AtomicCPU|TimingSimpleCPU}
    --clock-hz INT
    --mem-bytes INT
    --mem-mode {atomic|timing}
    --dram-latency-ns INT       (used for both AtomicCPU and Timing;
                                 Atomic collapses mem latency into
                                 numCycles, Timing charges it per
                                 access)
    --xbar-response-ns INT      SystemXBar frontend/forward/response
                                latency, in ns; 0 for AtomicCPU so
                                numCycles stays dominated by insn
                                retirement
    --l1i-size STR              gem5-style size string, e.g. 32kB
    --l1i-assoc INT
    --l1i-hit-cycles INT
    --l1i-miss-cycles INT
    --l1d-size STR
    --l1d-assoc INT
    --l1d-hit-cycles INT
    --l1d-miss-cycles INT
    --enable-l1                 bool-ish; pass the flag to stand up
                                the L1I+L1D topology. Default off so
                                AtomicCPU stays degenerate.

Topology:

    CPU (AtomicCPU | TimingSimpleCPU)
      icache_port --[L1I?]----> membus --> SimpleMemory
      dcache_port --[L1D?]----/

When --enable-l1 is set, an L1I and L1D Cache each sit between the
respective CPU port and the crossbar. Without it the CPU ports hit
the membus directly (the original AtomicCPU degenerate path).
"""

import argparse
import sys

import m5
from m5.objects import (
    AddrRange,
    AtomicSimpleCPU,
    Cache,
    Process,
    RiscvMinorCPU,
    Root,
    SEWorkload,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    TimingSimpleCPU,
    VoltageDomain,
)


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
    args = ap.parse_args()

    system = System()
    system.clk_domain = SrcClockDomain(
        clock=f"{args.clock_hz}Hz",
        voltage_domain=VoltageDomain(),
    )
    system.mem_mode = args.mem_mode
    system.mem_ranges = [AddrRange(f"{args.mem_bytes}B")]

    if args.cpu_type == "AtomicCPU":
        system.cpu = AtomicSimpleCPU()
    elif args.cpu_type == "TimingSimpleCPU":
        system.cpu = TimingSimpleCPU()
    else:  # MinorCPU — in-order pipeline model used for AC-4.
        system.cpu = RiscvMinorCPU()

    # SystemXBar latency: for AtomicCPU the self-check wants
    # numCycles dominated by retirement, so run-gem5.py passes 0.
    # For TimingSimpleCPU the YAML can raise it to model real bus
    # traversal time.
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

    # Memory latency model: for AtomicCPU + atomic mem_mode the
    # charged latency flows back into numCycles; for TimingSimpleCPU
    # the DRAM latency is charged per access.
    mem_latency = 0 if args.mem_mode == "atomic" else args.dram_latency_ns
    system.mem_ctrl = SimpleMemory(
        range=system.mem_ranges[0],
        latency=f"{mem_latency}ns",
    )
    system.mem_ctrl.port = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports

    system.workload = SEWorkload.init_compatible(args.binary)
    process = Process()
    process.cmd = [args.binary]
    system.cpu.workload = process
    system.cpu.createThreads()

    root = Root(full_system=False, system=system)
    m5.instantiate()

    caches_str = "L1I+L1D" if args.enable_l1 else "no-caches"
    print(
        f"gem5-se-config: starting {args.cpu_type} simulation of "
        f"{args.binary} ({caches_str}, mem_mode={args.mem_mode}, "
        f"dram_latency={mem_latency}ns)",
        flush=True,
    )
    exit_event = m5.simulate()
    print(
        f"gem5-se-config: exit tick={m5.curTick()} "
        f"reason={exit_event.getCause()}",
        flush=True,
    )


if __name__ == "__main__":
    main()
else:
    main()
