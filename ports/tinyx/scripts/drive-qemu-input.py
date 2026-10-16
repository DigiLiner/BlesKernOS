#!/usr/bin/env python3
"""Send reproducible PS/2 input to a running TinyX KVM gate."""
import argparse
import json
from pathlib import Path
import socket
import time


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--marker", required=True)
    parser.add_argument("--scenario", choices=("xlib", "notepad", "notepad_save"), required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    deadline = time.monotonic() + 900
    while time.monotonic() < deadline:
        serial = output / "serial.log"
        command = output / "command.json"
        if serial.exists() and command.exists() and args.marker in serial.read_text(errors="replace"):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("readiness marker not seen")
    qemu = json.loads(command.read_text())
    monitor = qemu[qemu.index("-monitor") + 1].split(",", 1)[0].removeprefix("unix:")
    with socket.socket(socket.AF_UNIX) as conn:
        conn.settimeout(5)
        conn.connect(monitor)
        conn.recv(4096)

        def hmp(command, delay=0.3):
            conn.sendall((command + "\n").encode())
            time.sleep(delay)
            try:
                response = conn.recv(8192).decode(errors="replace")
            except socket.timeout:
                response = ""
            if "Error" in response or "error" in response:
                print(command, response.strip()[-220:], flush=True)
            else:
                print(command, flush=True)

        hmp(f"screendump {output / 'before.ppm'}", 0.5)
        for _ in range(8):
            hmp("mouse_move -100 -100", 0.1)
        hmp("mouse_move 100 100")
        hmp("mouse_move 100 100")
        hmp("mouse_move 12 8")
        hmp("mouse_button 1")
        hmp("mouse_button 0")
        if args.scenario == "xlib":
            for key in ("a", "s", "w", "x", "up", "left", "home", "end",
                        "pgup", "pgdn", "insert", "delete", "ctrl-a", "shift-b"):
                hmp("sendkey " + key)
            hmp("mouse_move 12 8")
        elif args.scenario == "notepad":
            for key in ("b", "l", "e", "s", "k", "ret", "w", "i", "n", "e",
                        "ret", "s", "w", "x", "a"):
                hmp("sendkey " + key)
        else:
            for key in ("s", "t", "a", "g", "e", "1"):
                hmp("sendkey " + key)
            hmp("sendkey ctrl-s", 3)
            hmp("sendkey ctrl-a")
            for key in ("s", "t", "a", "g", "e", "1", "dot", "t", "x", "t"):
                hmp("sendkey " + key)
            hmp("sendkey ret", 3)
        hmp(f"screendump {output / 'after.ppm'}", 0.5)


if __name__ == "__main__":
    run()
