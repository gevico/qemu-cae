#!/usr/bin/env python3
"""
QMP helper: start QEMU with QMP on stdio, run test binary, poll for
completion sentinel, then read final memory state.

Usage: qmp-read-state.py <qemu-binary> <accel> <bios-path> <mem-addr> [nwords]

Polls xp at mem-addr until the last word matches 0xCAE0CAFE (completion
sentinel), then outputs all nwords as hex values, one per line.
"""
import json
import subprocess
import sys
import time


def main():
    if len(sys.argv) < 5:
        print("Usage: qmp-read-state.py <qemu> <accel> <bios> <addr> [nwords]",
              file=sys.stderr)
        sys.exit(1)

    qemu = sys.argv[1]
    accel = sys.argv[2]
    bios = sys.argv[3]
    addr = int(sys.argv[4], 0)
    nwords = int(sys.argv[5]) if len(sys.argv) > 5 else 8

    # Start QEMU with QMP on stdio
    proc = subprocess.Popen(
        [qemu, "-accel", accel, "-machine", "virt", "-nographic",
         "-bios", bios, "-serial", "none", "-monitor", "none",
         "-S", "-qmp", "stdio"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True)

    def send(cmd, **args):
        msg = {"execute": cmd}
        if args:
            msg["arguments"] = args
        proc.stdin.write(json.dumps(msg) + "\n")
        proc.stdin.flush()

    def recv_until_return():
        """Read lines until we get one with 'return' key, return it."""
        while True:
            line = proc.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "return" in obj:
                return obj
            # Skip events and greeting

    def recv_string_return():
        """Read lines until we get a return with a string value."""
        while True:
            line = proc.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "return" in obj and isinstance(obj["return"], str):
                return obj["return"]

    # Negotiate QMP
    proc.stdout.readline()  # greeting
    send("qmp_capabilities")
    recv_until_return()

    # Start execution
    send("cont")
    recv_until_return()

    # Poll for completion sentinel (last word = 0xCAE0CAFE)
    sentinel = 0xCAE0CAFE
    deadline = time.time() + 30
    values = []

    while time.time() < deadline:
        time.sleep(0.5)
        send("human-monitor-command",
             **{"command-line": f"xp /{nwords}gx 0x{addr:x}"})
        output = recv_string_return()
        if not output:
            continue

        # Parse xp output: "addr: val1 val2 val3 val4\n..."
        vals = []
        for xp_line in output.strip().split("\n"):
            if ":" not in xp_line:
                continue
            parts = xp_line.split(":")[1].strip().split()
            for p in parts:
                p = p.strip()
                if p:
                    try:
                        vals.append(int(p, 16))
                    except ValueError:
                        pass

        if len(vals) >= nwords and vals[nwords - 1] == sentinel:
            values = vals[:nwords]
            break

    if not values:
        print("ERROR: sentinel not found within timeout", file=sys.stderr)
        send("quit")
        proc.wait(timeout=5)
        sys.exit(1)

    for v in values:
        print(f"0x{v:016x}")

    # If running under CAE, also dump engine- and CPU-level timing
    # state so the harness can assert AC-7 behavior. Each value comes
    # out as a `# key=value` trailer; missing properties (e.g. under
    # -accel tcg) are silently skipped.
    def qom_get_uint(path, prop):
        try:
            send("qom-get", path=path, property=prop)
            line = proc.stdout.readline()
            while line:
                obj = json.loads(line.strip())
                if "return" in obj and isinstance(obj["return"], int):
                    return obj["return"]
                if "error" in obj:
                    return None
                line = proc.stdout.readline()
        except Exception:
            return None
        return None

    if accel.startswith("cae"):
        engine_keys = [
            ("cae-current-cycle", "current-cycle"),
            ("cae-virtual-clock-ns", "virtual-clock-ns"),
            ("cae-base-freq-hz", "base-freq-hz"),
            ("cae-num-cpus", "num-cpus"),
        ]
        num_cpus = 0
        for label, prop in engine_keys:
            val = qom_get_uint("/objects/cae-engine", prop)
            if val is None:
                continue
            print(f"# {label}={val}")
            if prop == "num-cpus":
                num_cpus = val

        # Per-CPU trailer. Engine unconditionally registers cpu0..cpuN-1
        # as children of /objects/cae-engine (cae_engine_register_cpu).
        # cpu0 also emits the unqualified legacy labels so the Phase-1
        # harness keeps parsing them; every CPU additionally emits
        # cae-cpuN-<prop> so difftest can align with per-CPU gem5 stats.
        cpu_props = ["cycle-count", "insn-count", "stall-cycles"]
        cpu_range = range(num_cpus) if num_cpus > 0 else range(1)
        for cpu_idx in cpu_range:
            path = f"/objects/cae-engine/cpu{cpu_idx}"
            for prop in cpu_props:
                val = qom_get_uint(path, prop)
                if val is None:
                    continue
                if cpu_idx == 0:
                    print(f"# cae-{prop}={val}")
                print(f"# cae-cpu{cpu_idx}-{prop}={val}")

    send("quit")
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


if __name__ == "__main__":
    main()
