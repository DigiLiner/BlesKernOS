#!/usr/bin/env python3
"""Idempotently enable/disable Codex Pro targets in the root Makefile."""
from __future__ import annotations

import argparse
from pathlib import Path

BEGIN = "# >>> BLES CODEX PRO >>>"
END = "# <<< BLES CODEX PRO <<<"
BLOCK = f"""\n{BEGIN}\n# Optional IDE/development-image integration.\n-include mk/codex-pro.mk\n{END}\n"""


def find_root(start: Path) -> Path:
    here = start.resolve()
    for candidate in (here, *here.parents):
        if (candidate / "Makefile").is_file() and (candidate / "sdk").is_dir():
            return candidate
    raise SystemExit("[CODEX] ejecute este script desde la raiz de BlesKernOS 0.9")


def remove_block(text: str) -> str:
    start = text.find(BEGIN)
    if start < 0:
        return text
    end = text.find(END, start)
    if end < 0:
        raise SystemExit("[CODEX] Makefile contiene un bloque Codex Pro incompleto")
    end += len(END)
    if end < len(text) and text[end] == "\n":
        end += 1
    return text[:start].rstrip() + "\n" + text[end:].lstrip("\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--uninstall", action="store_true")
    args = parser.parse_args()

    root = find_root(Path.cwd())
    makefile = root / "Makefile"
    integration = root / "mk/codex-pro.mk"
    builder = root / "tools" / "build_codex_pro_image.py"
    if not args.uninstall and (not integration.is_file() or not builder.is_file()):
        raise SystemExit("[CODEX] primero aplique BlesKernOS-0.9-Codex-Pro.patch")

    original = makefile.read_text(encoding="utf-8")
    cleaned = remove_block(original)
    updated = cleaned if args.uninstall else cleaned.rstrip() + "\n" + BLOCK

    if updated != original:
        backup = makefile.with_suffix(".codex-pro.bak")
        if not backup.exists():
            backup.write_text(original, encoding="utf-8")
        makefile.write_text(updated, encoding="utf-8")

    if args.uninstall:
        print("[CODEX] integracion retirada del Makefile")
    else:
        print("[CODEX] integrado: use `make codex-pro-image`")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
