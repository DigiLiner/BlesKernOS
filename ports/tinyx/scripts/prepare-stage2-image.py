#!/usr/bin/env python3
"""Clone a Wine image for an external PE, MSI, or installed application."""
import argparse
from pathlib import Path
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[3]
X11_LIBRARIES = (
    "libXrender.so.1", "libXfixes.so.3", "libXcursor.so.1",
    "libXi.so.6", "libXrandr.so.2",
)


def run(*args):
    subprocess.run(args, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="initialized Wine FAT32 image")
    parser.add_argument("destination", type=Path, help="new test image")
    parser.add_argument("application", nargs="?", type=Path,
                        help="external i386 Windows PE or MSI")
    parser.add_argument("--run-installed", metavar="GUEST_PATH",
                        help="launch an application already installed in the source image")
    parser.add_argument("--open-file", type=Path,
                        help="copy a document to guest root and pass its path to the application")
    parser.add_argument("--seconds", type=int, default=240)
    parser.add_argument("--accept-exit", action="store_true",
                        help="pass when an installer window mapped and it exited with code 0")
    parser.add_argument("--trace-msi", action="store_true",
                        help="enable Wine MSI and cabinet diagnostics")
    args = parser.parse_args()
    if not args.source.is_file():
        parser.error("source image must exist")
    if bool(args.application) == bool(args.run_installed):
        parser.error("provide either an application or --run-installed")
    if args.application and not args.application.is_file():
        parser.error("application must exist")
    if args.open_file and not args.open_file.is_file():
        parser.error("--open-file must exist")
    if args.destination.exists():
        parser.error("destination must not already exist")
    if not 10 <= args.seconds <= 600:
        parser.error("--seconds must be between 10 and 600")
    libraries = [Path("/usr/lib/i386-linux-gnu") / name
                 for name in X11_LIBRARIES]
    for library in libraries:
        if not library.is_file():
            parser.error(f"missing host i386 library: {library}")

    args.destination.parent.mkdir(parents=True, exist_ok=True)
    run("cp", "--reflink=auto", str(args.source), str(args.destination))
    with args.destination.open("rb") as image:
        mbr = image.read(512)
    if mbr[510:512] != b"\x55\xaa":
        raise RuntimeError("source image has no MBR signature")
    offset = struct.unpack_from("<I", mbr, 454)[0] * 512
    fat = f"{args.destination}@@{offset}"
    is_msi = bool(args.application and args.application.suffix.lower() == ".msi")
    if is_msi and args.open_file:
        parser.error("--open-file cannot be combined with an MSI installer")
    guest_program = args.run_installed or ("/STAGE2.MSI" if is_msi else "/STAGE2.EXE")
    guest_document = None
    if args.open_file:
        suffix = args.open_file.suffix.upper()
        if not suffix or len(suffix) > 5 or not suffix[1:].isalnum():
            parser.error("--open-file needs a short alphanumeric extension")
        guest_document = f"/STAGE2{suffix}"
    launcher = args.destination.parent / "wine-stage2-launch"
    compiler = ["gcc", "-m32", "-O2", "-Wall", "-Wextra", "-no-pie",
        "-Wl,--disable-new-dtags",
        "-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2",
        "-Wl,-rpath,/WINEGATE/lib",
        "-DSTAGE2_PROBE_EXTENSIONS",
        f'-DSTAGE1_DEFAULT_PROGRAM="{guest_program}"',
        f"-DSTAGE1_DEFAULT_SECONDS={args.seconds}",
        str(ROOT / "ports/tinyx/tests/wine-stage1-launch.c"),
        "-o", str(launcher), "-L/usr/lib/i386-linux-gnu", "-l:libX11.so.6"]
    if is_msi:
        compiler.insert(-5, "-DSTAGE2_MSI_INSTALL")
    if args.accept_exit or is_msi:
        compiler.insert(-5, "-DSTAGE2_ACCEPT_EXIT")
    if args.trace_msi:
        compiler.insert(-5, "-DSTAGE2_TRACE_MSI")
    if guest_document:
        compiler.insert(-5, f'-DSTAGE2_PROGRAM_ARGUMENT="{guest_document}"')
    run(*compiler)
    if args.application:
        run("mcopy", "-o", "-i", fat, str(args.application),
            f"::{guest_program}")
    if guest_document:
        run("mcopy", "-o", "-i", fat, str(args.open_file),
            f"::{guest_document}")
    # The prebuilt gate kernel executes this path first. Keep the original
    # image intact and replace only the launcher in the disposable clone.
    run("mcopy", "-o", "-i", fat, str(launcher),
        "::/WINEGATE/bin/wine-stage1-launch")
    for library in libraries:
        run("mcopy", "-o", "-i", fat, str(library),
            f"::/WINEGATE/lib/{library.name}")
    print(args.destination)


if __name__ == "__main__":
    main()
