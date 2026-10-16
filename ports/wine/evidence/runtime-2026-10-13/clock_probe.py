"""Read PIT ticks through QMP, without a guest profiling thread.

Usage: clock_probe.py RUN_DIRECTORY PHYSICAL_KERNEL_TICKS_ADDRESS [HZ]
Resolve the address from the exact guest kernel ELF with nm. This kernel maps
its low kernel data at identical physical and virtual addresses.
"""
import json
from pathlib import Path
import socket
import sys
import time

out = Path(sys.argv[1]).resolve()
address = int(sys.argv[2], 0)
hz = int(sys.argv[3]) if len(sys.argv) > 3 else 100
sock = socket.socket(socket.AF_UNIX)
sock.settimeout(5)
sock.connect(str(out / "qmp.sock"))
stream = sock.makefile("rwb", buffering=0)
stream.readline()

def qmp(name, arguments=None):
    stream.write((json.dumps({"execute": name,
        **({"arguments": arguments} if arguments else {})}) + "\n").encode())
    while True:
        reply = json.loads(stream.readline())
        if "error" in reply:
            raise RuntimeError(reply)
        if "return" in reply:
            return reply["return"]

qmp("qmp_capabilities")
samples = []
for index in range(41):
    before = time.monotonic()
    response = qmp("human-monitor-command", {"command-line": f"xp /1uw {address:#x}"})
    after = time.monotonic()
    samples.append({"host_mid_s": (before + after) / 2,
                    "query_ms": (after - before) * 1000,
                    "ticks": int(response.split(":")[-1].strip())})
    if index < 40:
        time.sleep(.5)
(out / "clock.json").write_text(json.dumps(samples, indent=2) + "\n")
first, last = samples[0], samples[-1]
print(json.dumps({"wall_s": last["host_mid_s"] - first["host_mid_s"],
                  "guest_s": ((last["ticks"] - first["ticks"]) & 0xffffffff) / hz,
                  "samples": len(samples)}))
