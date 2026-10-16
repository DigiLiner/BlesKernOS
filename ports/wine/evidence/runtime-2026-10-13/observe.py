"""Observe a disposable Wine gate using host monotonic time, without guest edits.

Usage: python3 observe.py /absolute/path/to/run [ide|ahci] [seconds]
Requires Pillow and an already prepared run/test.img. Serial timestamps include
up to one polling interval of observer delay; first_white is only a visual proxy.
"""
import json
from pathlib import Path
import socket
import subprocess
import sys
import time

from PIL import Image

out = Path(sys.argv[1]).resolve()
bus = sys.argv[2] if len(sys.argv) > 2 else "ide"
limit_seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 150
symbols = subprocess.check_output(["nm", str(out / "stage2-kernel/kernel-test.elf")], text=True)
tick_address = next(int(line.split()[0], 16) for line in symbols.splitlines()
                    if line.endswith(" kernel_ticks"))
disk = ["-drive", f"file={out}/test.img,format=raw,if=ide"]
if bus == "ahci":
    disk = ["-drive", f"file={out}/test.img,format=raw,if=none,id=osdisk",
            "-device", "ich9-ahci,id=ahci", "-device", "ide-hd,drive=osdisk,bus=ahci.0"]
command = ["qemu-system-i386", "-accel", "kvm", "-cpu", "pentium3", "-smp", "2",
           "-m", "1536", *disk, "-vga", "vmware", "-display", "none",
           "-serial", f"file:{out}/serial.log", "-qmp",
           f"unix:{out}/qmp.sock,server=on,wait=off", "-no-reboot", "-no-shutdown"]
records = []
clock_samples = []
started = time.monotonic()
proc = subprocess.Popen(command, stdout=(out / "qemu.log").open("w"), stderr=subprocess.STDOUT)
stream = None
first_white = None
def qmp(name, arguments=None):
    stream.write((json.dumps({"execute": name, **({"arguments": arguments} if arguments else {})}) + "\n").encode())
    while True:
        reply = json.loads(stream.readline())
        if "error" in reply:
            raise RuntimeError(reply)
        if "return" in reply:
            return reply["return"]

try:
    for attempt in range(100):
        try:
            sock = socket.socket(socket.AF_UNIX)
            sock.settimeout(5)
            sock.connect(str(out / "qmp.sock"))
            break
        except (FileNotFoundError, ConnectionRefusedError):
            sock.close()
            time.sleep(.05)
    else:
        raise RuntimeError("QMP unavailable")
    stream = sock.makefile("rwb", buffering=0)
    stream.readline()
    qmp("qmp_capabilities")
    serial = (out / "serial.log").open()
    pending = ""
    last_screen = 0
    while time.monotonic() - started < limit_seconds:
        now = time.monotonic() - started
        pending += serial.read()
        lines = pending.split("\n")
        pending = lines.pop()
        for line in lines:
            records.append({"host_s": now, "line": line.rstrip()})
            if any(marker in line for marker in ("[WINE:TIME]", "[WINE:PERF]", "[GATE:", "[WATCHDOG]")):
                print(f"{now:.3f} {line.rstrip()}", flush=True)
        if now - last_screen >= .5:
            before = time.monotonic()
            response = qmp("human-monitor-command", {"command-line": f"xp /1uw {tick_address:#x}"})
            after = time.monotonic()
            clock_samples.append({"host_mid_s": (before + after) / 2,
                "query_ms": (after - before) * 1000,
                "ticks": int(response.split(":")[-1].strip())})
            qmp("screendump", {"filename": str(out / "screen.ppm")})
            im = Image.open(out / "screen.ppm")
            white = sum(r > 240 and g > 240 and b > 240 for r, g, b in im.getdata())
            if white > 20000 and first_white is None:
                first_white = time.monotonic() - started
                print(f"FIRST_WHITE {first_white:.3f}", flush=True)
                im.save(out / "first-white.png")
            last_screen = now
        if first_white and now > first_white + 20:
            break
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU exited: {proc.returncode}")
        time.sleep(.01)
    Image.open(out / "screen.ppm").save(out / "screen.png")
finally:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    (out / "host.json").write_text(json.dumps({"command": command,
        "duration_host_s": time.monotonic() - started,
        "first_white_host_s": first_white, "events": records,
        "clock_samples": clock_samples}, indent=2) + "\n")
