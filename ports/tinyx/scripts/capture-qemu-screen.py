#!/usr/bin/env python3
"""Capture a KVM gate screen after a serial readiness marker appears."""
import argparse
import json
from pathlib import Path
import socket
import time

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--marker", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--delay", type=float, default=2)
    args = parser.parse_args()
    output = args.output.resolve()
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        serial = output / "serial.log"
        command = output / "command.json"
        if serial.exists() and command.exists() and args.marker in serial.read_text(errors="replace"):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("readiness marker not seen")
    time.sleep(args.delay)
    qemu = json.loads(command.read_text())
    monitor = qemu[qemu.index("-monitor") + 1].split(",", 1)[0].removeprefix("unix:")
    raw = output / (args.name + ".ppm")
    with socket.socket(socket.AF_UNIX) as conn:
        conn.settimeout(5)
        conn.connect(monitor)
        conn.recv(4096)
        conn.sendall((f"screendump {raw}\n").encode())
        time.sleep(1)
    Image.open(raw).save(output / (args.name + ".png"))
    print(output / (args.name + ".png"))


if __name__ == "__main__":
    main()
