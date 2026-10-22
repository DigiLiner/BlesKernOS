#!/usr/bin/env python3
"""Build a FAT32 Codex Pro development disk from the current BlesKernOS tree.

The image is intentionally assembled from *generated* SDK artifacts instead of
shipping a stale private copy.  That keeps Codex in lock-step with the OS ABI.
"""
from __future__ import annotations

import argparse
import math
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

MIB = 1024 * 1024


def require_tool(name: str) -> None:
    if shutil.which(name) is None:
        raise SystemExit(f"[CODEX] falta `{name}` (instale dosfstools/mtools)")


def copytree(src: Path, dst: Path) -> None:
    if not src.exists():
        return
    if src.is_dir():
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def tree_size(path: Path) -> int:
    if not path.exists():
        return 0
    if path.is_file():
        return path.stat().st_size
    total = 0
    for item in path.rglob("*"):
        if item.is_file():
            total += item.stat().st_size
    return total


def round_up(value: int, quantum: int) -> int:
    return ((value + quantum - 1) // quantum) * quantum


def write_project_makefile(path: Path) -> None:
    path.write_text(r'''# Bles Codex Pro project Makefile
# Designed for the GCC/binutils toolchain bundled on the Codex Pro image.

SDKROOT ?= /SDK
CC ?= gcc
LD ?= ld

APP ?= APP.BEX
SRC ?= main.c
OBJ := $(SRC:.c=.o)
PROFILE ?= debug
# LBS sources implement bk_lbs_program_main and bk_lbs_requirements.
USE_LBS ?= 0

ARCH_FLAGS := -m32 -march=i486 -mtune=pentium -mfpmath=387 -mno-sse -mno-sse2
BASE_FLAGS := $(ARCH_FLAGS) -ffreestanding -fno-builtin -nostdlib -nostdinc \
              -fno-pic -fno-pie -fno-stack-protector \
              -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -D__BLESKERNOS__=1 -DBLESKERNOS=1 \
              -I"$(SDKROOT)/INCLUDE" -Wall -Wextra

ifeq ($(PROFILE),release)
MODE_FLAGS := -Os
else
MODE_FLAGS := -O0 -g
endif

CFLAGS ?= -std=gnu11 $(BASE_FLAGS) $(MODE_FLAGS)

# BlesKernOS native programs use the user-space SDK runtime.  Do not link
# build/system/libs/libc/libc.a here: that archive belongs to the kernel and
# contains private imports such as language_translate/vga_putchar.
# --start-group rescans POSIX/libm/libblesk until their circular references
# are resolved while leaving only the published BlesKernOS API as imports.
LIBS := "$(SDKROOT)/LIB/libblesk_posix.a" \
        "$(SDKROOT)/LIB/libm.a" \
        "$(SDKROOT)/LIB/libblesk.a"

ifeq ($(USE_LBS),1)
LBS_RUNTIME := "$(SDKROOT)/LIB/libblesk_lbs.a" "$(SDKROOT)/LIB/lbs_host.o"
LIBS := "$(SDKROOT)/LIB/libblesk_lbs_posix.a" "$(SDKROOT)/LIB/libm.a" "$(SDKROOT)/LIB/libblesk_lbs_native.a"
else
LBS_RUNTIME :=
endif

.PHONY: all debug release clean check
all: $(APP)

debug:
	$(MAKE) PROFILE=debug all

release:
	$(MAKE) PROFILE=release all

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(APP): $(OBJ)
	$(LD) -m elf_i386 -r -u bleskernos_program_main --allow-multiple-definition \
		$(OBJ) $(LBS_RUNTIME) --start-group $(LIBS) --end-group -o $@

check: $(APP)
	@echo "[CODEX] build OK: $(APP)"

clean:
	rm -f $(OBJ) $(APP)
''', encoding="utf-8")


def create_manifest(stage: Path, copied: list[str], payload: int) -> None:
    lines = [
        "BLES CODEX PRO DEVELOPMENT IMAGE",
        "=================================",
        "",
        "This image was generated from the current BlesKernOS source tree.",
        "It deliberately does not use Codex's old private SDK snapshot.",
        "",
        "Layout:",
        "  /CODEX      IDE binary/configuration",
        "  /SDK        current headers and generated static libraries",
        "  /TOOLCHAIN  self-host GCC/binutils/make payload",
        "  /PROJECTS   starter projects",
        "",
        "Build model:",
        "  cd /PROJECTS/HELLO",
        "  make",
        "",
        "Artifacts included:",
    ]
    lines.extend(f"  {x}" for x in copied)
    lines += ["", f"Payload bytes before FAT overhead: {payload}", ""]
    (stage / "README.TXT").write_text("\n".join(lines), encoding="ascii", errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--codex-dir", required=True)
    ap.add_argument("--templates", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--no-toolchain", action="store_true")
    args = ap.parse_args()

    require_tool("mkfs.fat")
    require_tool("mcopy")
    require_tool("mdir")

    root = Path(args.root).resolve()
    codex = (root / args.codex_dir).resolve()
    templates = (root / args.templates).resolve()
    output = (root / args.output).resolve()

    if not (root / "sdk" / "include").is_dir():
        raise SystemExit("[CODEX] no existe sdk/include; ejecute desde BlesKernOS")
    if not codex.is_dir():
        raise SystemExit(f"[CODEX] no existe el proyecto Codex: {codex}")

    # The generated SDK archive is the minimum proof that `make sdk` ran.
    if not (root / "build" / "sdk" / "libblesk.a").is_file():
        raise SystemExit("[CODEX] falta build/sdk/libblesk.a; ejecute `make sdk`")

    with tempfile.TemporaryDirectory(prefix="codex-pro-") as td:
        stage = Path(td) / "stage"
        (stage / "CODEX").mkdir(parents=True)
        (stage / "SDK" / "INCLUDE").mkdir(parents=True)
        (stage / "SDK" / "LIB").mkdir(parents=True)
        (stage / "PROJECTS").mkdir(parents=True)

        copied: list[str] = []

        # Full public SDK source/header tree, plus the generated SDK directory.
        copytree(root / "sdk" / "include", stage / "SDK" / "INCLUDE")
        copied.append("sdk/include -> /SDK/INCLUDE")
        copytree(root / "sdk", stage / "SDK" / "SOURCE")
        copied.append("sdk/* -> /SDK/SOURCE")

        for lib in sorted((root / "build" / "sdk").glob("*.a")):
            copytree(lib, stage / "SDK" / "LIB" / lib.name)
            copied.append(f"build/sdk/{lib.name}")

        for obj in ('lbs.o', 'lbs_host.o'):
            copytree(root/'build/sdk'/obj, stage/'SDK/LIB'/obj)
        for lib in sorted((root/'build/system/libs/native').glob('*.LBS')):
            copytree(lib, stage/'SYSTEM'/lib.name)
        copytree(root/'sdk/examples/lbs_startup_demo.c', stage/'PROJECTS/LBS/main.c')

        # Runtime libraries that current native ports commonly need.
        extra_libs = [
            (root / "build/system/libs/libc/libc.a", "libc.a"),
            (root / "build/system/libs/tinygl/tinygl.a", "tinygl.a"),
            (root / "build/system/libs/mesa35/mesa35.a", "mesa35.a"),
            (root / "build/system/libs/bearssl/libbearssl.a", "libbearssl.a"),
        ]
        for src, name in extra_libs:
            if src.is_file():
                copytree(src, stage / "SDK" / "LIB" / name)
                copied.append(str(src.relative_to(root)))

        codex_obj = codex / "build" / "CODEX.O"
        if codex_obj.is_file():
            copytree(codex_obj, stage / "CODEX" / "CODEX.O")
            copied.append(str(codex_obj.relative_to(root)))

        # Preserve Codex documentation/examples when they exist.
        for name in ("README.md", "OS_INTEGRATION.txt", "BUILD_INFO.txt"):
            src = codex / name
            if src.is_file():
                copytree(src, stage / "CODEX" / name.upper())
        copytree(codex / "docs", stage / "CODEX" / "DOCS")
        copytree(codex / "examples", stage / "CODEX" / "EXAMPLES")

        if templates.is_dir():
            copytree(templates, stage / "PROJECTS")

        # Every starter project gets the same known-good native build recipe.
        for project in (stage / "PROJECTS").iterdir():
            if project.is_dir():
                write_project_makefile(project / "Makefile")
                if project.name == 'LBS':
                    recipe = project/'Makefile'
                    recipe.write_text(recipe.read_text().replace('USE_LBS ?= 0', 'USE_LBS ?= 1'))

        if not args.no_toolchain:
            toolchain = root / "build" / "selfhost" / "toolchain"
            if not toolchain.is_dir():
                raise SystemExit("[CODEX] falta build/selfhost/toolchain; ejecute `make selfhost`")
            copytree(toolchain, stage / "TOOLCHAIN")
            copied.append("build/selfhost/toolchain -> /TOOLCHAIN")

        payload = tree_size(stage)
        create_manifest(stage, copied, payload)
        payload = tree_size(stage)

        # FAT metadata + source trees need headroom. Use sparse allocation first.
        wanted = max(128 * MIB, int(payload * 1.45) + 48 * MIB)
        image_size = round_up(wanted, 16 * MIB)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("wb") as f:
            f.truncate(image_size)

        subprocess.run(["mkfs.fat", "-F", "32", "-n", "CODEXPRO", str(output)], check=True)
        for item in sorted(stage.iterdir()):
            subprocess.run(["mcopy", "-s", "-o", "-i", str(output), str(item), "::/"], check=True)

        probe = subprocess.run(
            ["mdir", "-i", str(output), "::/"],
            check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        ).stdout
        for required in ("CODEX", "SDK", "PROJECTS", "README"):
            if required not in probe.upper():
                raise SystemExit(f"[CODEX] imagen creada pero falta {required}")

        print(f"[CODEX] payload={payload / MIB:.1f} MiB image={image_size / MIB:.0f} MiB")
        print(f"[CODEX] {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
