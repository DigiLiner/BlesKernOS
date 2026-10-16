#!/usr/bin/env python3
from __future__ import annotations
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("uso: verify_codex_pro_image.py IMAGE", file=sys.stderr)
        return 2
    image = Path(sys.argv[1])
    if not image.is_file():
        print(f"[CODEX] no existe {image}", file=sys.stderr)
        return 1
    if shutil.which("mdir") is None:
        print("[CODEX] falta mdir (mtools)", file=sys.stderr)
        return 1
    out = subprocess.run(["mdir", "-i", str(image), "::/"], text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
    upper = out.upper()
    missing = [x for x in ("CODEX", "SDK", "PROJECTS", "README") if x not in upper]
    if missing:
        print("[CODEX] FAIL: faltan " + ", ".join(missing), file=sys.stderr)
        return 1
    print(f"[CODEX] imagen valida: {image} ({image.stat().st_size // (1024*1024)} MiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
