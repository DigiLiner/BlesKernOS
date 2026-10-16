#!/usr/bin/env python3
"""Create a separate boot image with API 40 and native command updates.

The source must be stopped. Pending journal records are recovered on the copy. Existing destinations are refused.
User files and startup preferences are retained; test fixtures are not installed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from bfs4_update import Editor
from license_notices import write_notices
import bfs4

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    source, destination = args.source.resolve(), args.destination.resolve()
    if destination.exists():
        parser.error("destination already exists; choose a new image")
    for process in Path("/proc").iterdir():
        if not process.name.isdecimal():
            continue
        try:
            if "qemu-system" in (process / "comm").read_text() and any(
                fd.resolve() == source for fd in (process / "fd").iterdir()
            ):
                parser.error("source is open in QEMU; stop it before making a copy")
        except OSError:
            pass
    makefile = (ROOT / "Makefile").read_text()
    names = makefile[makefile.index("COMMAND_NAMES :="):makefile.index("COMMAND_COMMON_OBJ :=")].split(":=", 1)[1].replace("\\", "").split()
    targets = ["build/stage2.bin", "build/system/services/vision.o", "build/programs/shelllauncher.o", "build/system/drivers/NETSTACK.DVR"]
    targets += [f"build/system/commands/{name}.o" for name in names]
    subprocess.run(["make", "-j2", *targets], cwd=ROOT, check=True)
    destination.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cp", "--reflink=auto", str(source), str(destination)], check=True)
    from bfs4_journal import recover
    recovered = recover(destination)
    kernel, stage2 = (ROOT / "build/kernel.bin").read_bytes(), (ROOT / "build/stage2.bin").read_bytes()
    if len(kernel) > 1280 * 1024 or len(stage2) != 4096:
        raise ValueError("invalid boot payload size")
    with destination.open("r+b") as disk:
        disk.seek(512)
        disk.write(stage2)
        disk.seek(9 * 512)
        disk.write(kernel.ljust(1280 * 1024, b"\0"))
    files = {f"/SYSTEM/COMMANDS/{name.upper()}.BEX": ROOT / f"build/system/commands/{name}.o" for name in names}
    files.update({
        "/SYSTEM/SERVICES/VISION.BEX": ROOT / "build/system/services/vision.o",
        "/SYSTEM/PROGRAMS/SHELL.BEX": ROOT / "build/programs/shelllauncher.o",
        "/SYSTEM/DRIVERS/NETSTACK.DVR": ROOT / "build/system/drivers/NETSTACK.DVR",
        "/SYSTEM/SDK/INCLUDE/bleskernos_api.h": ROOT / "sdk/include/bleskernos_api.h",
        "/SYSTEM/SDK/INCLUDE/bleskernos_socket_info.h": ROOT / "sdk/include/bleskernos_socket_info.h",
    })
    files["/DOCS/LICENSES.TXT"] = write_notices(ROOT / "build/licenses/LICENSES.TXT")
    editor = Editor(destination)
    startup = editor.contents("/SYSTEM/USER/CONFIG/STARTUP.INI")
    for guest, host in files.items():
        editor.update(guest, host.read_bytes())
    editor.finish()
    verify = Editor(destination)
    for guest, host in files.items():
        assert verify.contents(guest) == host.read_bytes(), guest
    assert verify.contents("/SYSTEM/USER/CONFIG/STARTUP.INI") == startup
    verify.fd.close()
    with destination.open("rb") as disk:
        disk.seek(512)
        assert disk.read(4096) == stage2
        disk.seek(9 * 512)
        assert disk.read(len(kernel)) == kernel
    bfs4.check(destination)
    manifest = {
        "api": 40, "source": str(source), "image": str(destination),
        "startup_preserved": True, "recovered_journal_records": recovered,
        "installed": {guest: hashlib.sha256(host.read_bytes()).hexdigest() for guest, host in files.items()},
    }
    destination.with_suffix(".manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Verified API 40 image: {destination} ({len(names)} commands)")


if __name__ == "__main__":
    main()
