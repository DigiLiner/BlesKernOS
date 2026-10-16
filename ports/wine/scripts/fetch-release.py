#!/usr/bin/env python3
"""Prepara el paquete binario de Wine para BlesKernOS."""

import hashlib
import os
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
PACKAGE_DIR = ROOT / "ports/wine/package"
PACKAGE = PACKAGE_DIR / "wine-11.0-i386"
MANIFEST = PACKAGE / "prefix-template.json"

SHA256 = "0afba21234d620558a4e579b7436c1c18453936356241b851c61d6477ead7193"

# Configurar cuando el archivo esté publicado en GitHub Releases.
URL = os.environ.get("BLES_WINE_URL", "")

REQUIRED = (
    "prefix-template.json",
    "manifest.json",
    "rootfs/opt/wine/bin/wine",
    "rootfs/opt/wine/bin/wineserver",
)

if PACKAGE.exists():
    missing = [name for name in REQUIRED if not (PACKAGE / name).is_file()]
    if missing:
        sys.exit("[WINE] Paquete local incompleto: " + ", ".join(missing))
    print("[WINE] Paquete local encontrado.")
    sys.exit(0)

if not URL:
    sys.exit(
        "[WINE] Falta el paquete Wine.\n"
        "Publicá el archivo en GitHub Releases y configurá BLES_WINE_URL."
    )

PACKAGE_DIR.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix="bles-wine-") as tmp:
    tmp = Path(tmp)
    archive = tmp / "wine.tar.gz"
    extracted = tmp / "extract"
    extracted.mkdir()

    print("[WINE] Descargando paquete...")
    urllib.request.urlretrieve(URL, archive)

    actual = hashlib.file_digest(archive.open("rb"), "sha256").hexdigest()
    if actual != SHA256:
        sys.exit("[WINE] ERROR: SHA-256 incorrecto.")

    print("[WINE] SHA-256 correcto. Extrayendo...")

    with tarfile.open(archive, "r:gz") as tar:
        tar.extractall(extracted, filter="data")

    source = extracted / "wine-11.0-i386"
    if not (source / "prefix-template.json").is_file():
        sys.exit("[WINE] ERROR: paquete incompleto.")

    if PACKAGE.exists():
        sys.exit("[WINE] ERROR: existe un paquete parcial; revisar manualmente.")

    shutil.move(str(source), str(PACKAGE))

print("[WINE] Paquete instalado correctamente.")
