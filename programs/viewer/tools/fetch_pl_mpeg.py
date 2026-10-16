#!/usr/bin/env python3
"""Fetch the exact pl_mpeg source revision used by Viewer 3.1."""
from __future__ import annotations

import hashlib
from pathlib import Path
import sys
import urllib.request

COMMIT = "c871f2be022ece7ef4f64230b4fb8e1fb9eb6023"
BLOB_SHA1 = "b42f53f89f868a697435ab1e15742f370819ffbc"
URL = f"https://raw.githubusercontent.com/phoboslab/pl_mpeg/{COMMIT}/pl_mpeg.h"
DEST = Path(__file__).resolve().parent.parent / "third_party" / "pl_mpeg.h"


def git_blob_sha1(data: bytes) -> str:
    prefix = f"blob {len(data)}\0".encode("ascii")
    return hashlib.sha1(prefix + data).hexdigest()


def main() -> int:
    try:
        request = urllib.request.Request(
            URL, headers={"User-Agent": "BlesKernOS-Viewer/3.1"}
        )
        with urllib.request.urlopen(request, timeout=30) as response:
            data = response.read()
    except Exception as exc:
        print(f"[Viewer] no se pudo descargar pl_mpeg: {exc}", file=sys.stderr)
        print("[Viewer] descarga manualmente pl_mpeg.h del commit:", file=sys.stderr)
        print(f"         {COMMIT}", file=sys.stderr)
        print(f"         y guardalo en {DEST}", file=sys.stderr)
        return 1

    if not data.startswith(b"/*\nPL_MPEG - MPEG1 Video decoder"):
        print("[Viewer] pl_mpeg.h descargado no tiene la cabecera esperada", file=sys.stderr)
        return 1
    actual_blob = git_blob_sha1(data)
    if actual_blob != BLOB_SHA1:
        print("[Viewer] pl_mpeg.h no coincide con el blob fijado", file=sys.stderr)
        print(f"         esperado: {BLOB_SHA1}", file=sys.stderr)
        print(f"         recibido: {actual_blob}", file=sys.stderr)
        return 1

    DEST.parent.mkdir(parents=True, exist_ok=True)
    DEST.write_bytes(data)
    print(f"[Viewer] pl_mpeg {COMMIT[:12]} -> {DEST}")
    print(f"[Viewer] sha256 {hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
