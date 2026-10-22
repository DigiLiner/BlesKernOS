#!/usr/bin/env python3
import os
import atexit
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from build_print_sources import build_print_source_archive
from build_family_sources import build_family_source_archive
from license_notices import write_notices
from build_fat_floppy import bk_iconpak_try_prepare
import bfs4
from lbs_payload import documentation_payloads, application_payloads
from native_graphics_payload import native_graphics_payloads, stage_native_graphics_payloads
from wine_display_defaults import display_defaults
from wine_app_defaults import app_defaults
from wine_desktop_defaults import desktop_defaults
from wine_theme_defaults import theme_defaults


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
ATA_SIZE_KB = 768 * 1024
SECTOR_SIZE = 512
FAT32_CODE_OFFSET = 90
BFS_STAGING = None


def run(cmd):
    subprocess.check_call([str(part) for part in cmd])


def load(path, label):
    if not path:
        return None
    from lbs_payload import migrated_source
    source = migrated_source(path)
    try:
        data = source.read_bytes()
        print(f"{label} size: {len(data)} bytes", file=sys.stderr)
        return data
    except Exception as exc:
        print(f"Warning: No se pudo leer {path}: {exc}", file=sys.stderr)
        return None


def crlf(data):
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def write_payload(tmpdir, name, data):
    path = tmpdir / name
    path.write_bytes(data)
    return path


def is_wallpaper_file(path):
    suffix = path.suffix.upper()
    return suffix in {".BMP", ".PNG", ".JPG", ".JPE", ".JPN", ".JPNG"}


def mmd(image, path):
    if BFS_STAGING is not None:
        # Match mtools/BFS case-insensitive lookup on the host's staging FS.
        (BFS_STAGING / path.removeprefix("::/").upper()).mkdir(parents=True, exist_ok=True)
        return
    run(["mmd", "-i", image, path])


def mcopy(image, src, dst):
    from lbs_payload import migrated_source
    src = migrated_source(src)
    if BFS_STAGING is not None:
        target = BFS_STAGING / dst.removeprefix("::/").upper()
        if dst.endswith("/"):
            target = target / Path(src).name.upper()
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, target)
        return
    run(["mcopy", "-o", "-i", image, src, dst])


def mcopy_tree(image, source_root, destination_root, exclude=()):
    source_root = Path(source_root)
    excluded = tuple(Path(item) for item in exclude)

    def skipped(path):
        relative = path.relative_to(source_root)
        return any(relative == item or item in relative.parents
                   for item in excluded)

    mmd(image, destination_root)
    for directory in sorted(path for path in source_root.rglob("*")
                            if path.is_dir() and not skipped(path)):
        relative = directory.relative_to(source_root).as_posix().upper()
        mmd(image, f"{destination_root}/{relative}")
    for source in sorted(path for path in source_root.rglob("*")
                         if path.is_file() and not skipped(path)):
        relative = source.relative_to(source_root).as_posix().upper()
        mcopy(image, source, f"{destination_root}/{relative}")


def find_bleskernos_start_icon(icons_path):
    """Find the optional Start logo in the common source-tree locations.

    The normal icon-directory copy already handles it when it lives in the
    configured icons directory.  The explicit lookup also covers projects
    where the file was dropped into a top-level icons/ directory, which was a
    common manual workflow before the CDE Deskbar used the asset.
    """
    candidates = []
    if icons_path:
        candidates.append(Path(icons_path) / "BLESKERNOS.BMP")
    candidates.extend((
        ROOT_DIR / "assets" / "icons" / "BLESKERNOS.BMP",
        ROOT_DIR / "icons" / "BLESKERNOS.BMP",
        ROOT_DIR / "BLESKERNOS.BMP",
    ))
    seen = set()
    for candidate in candidates:
        key = str(candidate.resolve(strict=False))
        if key in seen:
            continue
        seen.add(key)
        if candidate.is_file():
            return candidate
        parent = candidate.parent
        if parent.is_dir():
            wanted = candidate.name.upper()
            for child in parent.iterdir():
                if child.is_file() and child.name.upper() == wanted:
                    return child
    return None


def patch_boot_sector(image, boot_sector, sector):
    boot = Path(boot_sector).read_bytes()
    if len(boot) != SECTOR_SIZE:
        raise ValueError(f"boot sector must be 512 bytes: {boot_sector}")

    with image.open("r+b") as handle:
        handle.seek(sector * SECTOR_SIZE)
        current = bytearray(handle.read(SECTOR_SIZE))
        current[0:3] = boot[0:3]
        current[FAT32_CODE_OFFSET:SECTOR_SIZE] = boot[FAT32_CODE_OFFSET:SECTOR_SIZE]
        handle.seek(sector * SECTOR_SIZE)
        handle.write(current)


def read_sector(image, sector):
    with image.open("rb") as handle:
        handle.seek(sector * SECTOR_SIZE)
        return handle.read(SECTOR_SIZE)


def write_sector(image, sector, data):
    if len(data) != SECTOR_SIZE:
        raise ValueError("sector write must be exactly 512 bytes")
    with image.open("r+b") as handle:
        handle.seek(sector * SECTOR_SIZE)
        handle.write(data)


def set_fat32_info_sectors(image, fsinfo_sector, backup_boot_sector):
    for sector in (0, backup_boot_sector):
        boot = bytearray(read_sector(image, sector))
        boot[48:50] = fsinfo_sector.to_bytes(2, "little")
        boot[50:52] = backup_boot_sector.to_bytes(2, "little")
        write_sector(image, sector, bytes(boot))


def write_raw_sector_data(image, sector, data, max_sectors, label):
    if len(data) > max_sectors * SECTOR_SIZE:
        raise ValueError(f"{label} ocupa mas de {max_sectors} sectores")
    with image.open("r+b") as handle:
        handle.seek(sector * SECTOR_SIZE)
        handle.write(data)


def zero_sector_range(image, first_sector, sector_count):
    with image.open("r+b") as handle:
        handle.seek(first_sector * SECTOR_SIZE)
        handle.write(b"\x00" * (sector_count * SECTOR_SIZE))


def main():
    global BFS_STAGING
    bk_iconpak_try_prepare(sys.argv)

    edition = os.environ.get("BLES_EDITION", "user").strip().lower()
    if edition not in {"user", "developer"}:
        print("BLES_EDITION debe ser user o developer", file=sys.stderr)
        return 2
    developer = edition == "developer"
    image_size_kb = ATA_SIZE_KB

    if len(sys.argv) < 2:
        print(
            "usage: build_fat32_ata.py <image> [shell.o] [filebrowser.o] "
            "[texteditor.o] [calculator.o] [midamp.o] [processmanager.o] "
            "[calendar.o] [desktop.ini] [icons_dir] [about.gif] "
            "[associations.ini] [reserved_sectors] [screensaverd.o] "
            "[ss_logo.o] [ss_pipes.o] [screensv.ini] [boot.bin] [stage2.bin] "
            "[kernel.bin] [kernel_sectors] [tinygl.a] [libc.a] "
            "[control.o] [appear.cpl] [display.cpl] [sound.cpl] "
            "[datetime.cpl] [mouse.cpl] [keyboard.cpl] [system.cpl] "
            "[devmgr.cpl] [internet.cpl] [modem.cpl] [language.cpl] [ss_balls.o] [mesa35.a]",
            file=sys.stderr,
        )
        return 1

    image = Path(sys.argv[1])
    shell_path = sys.argv[2] if len(sys.argv) > 2 else None
    filebrowser_path = sys.argv[3] if len(sys.argv) > 3 else None
    texteditor_path = sys.argv[4] if len(sys.argv) > 4 else None
    calculator_path = sys.argv[5] if len(sys.argv) > 5 else None
    midamp_path = sys.argv[6] if len(sys.argv) > 6 else None
    processmanager_path = sys.argv[7] if len(sys.argv) > 7 else None
    calendar_path = sys.argv[8] if len(sys.argv) > 8 else None
    desktop_path = sys.argv[9] if len(sys.argv) > 9 else None
    icons_path = Path(sys.argv[10]) if len(sys.argv) > 10 and sys.argv[10] else None
    about_gif_path = (
        Path(sys.argv[11]) if len(sys.argv) > 11 and sys.argv[11]
        else ROOT_DIR / "assets" / "gif" / "abount.gif"
    )
    associations_path = sys.argv[12] if len(sys.argv) > 12 else None
    screensaverd_path = sys.argv[14] if len(sys.argv) > 14 else None
    ss_logo_path = sys.argv[15] if len(sys.argv) > 15 else None
    ss_pipes_path = sys.argv[16] if len(sys.argv) > 16 else None
    reserved_arg = int(sys.argv[13]) if len(sys.argv) > 13 else 1033
    screensv_path = sys.argv[17] if len(sys.argv) > 17 else None
    boot_sector_path = Path(sys.argv[18]) if len(sys.argv) > 18 and sys.argv[18] else None
    stage2_path = Path(sys.argv[19]) if len(sys.argv) > 19 and sys.argv[19] else None
    kernel_path = Path(sys.argv[20]) if len(sys.argv) > 20 and sys.argv[20] else None
    kernel_reserved_sectors = int(sys.argv[21]) if len(sys.argv) > 21 else 1024
    tinygl_path = sys.argv[22] if len(sys.argv) > 22 else None
    libc_path = sys.argv[23] if len(sys.argv) > 23 else None
    control_paths = [sys.argv[i] if len(sys.argv) > i else None
                     for i in range(24, 37)]
    ss_balls_path = sys.argv[37] if len(sys.argv) > 37 else None
    mesa35_path = sys.argv[38] if len(sys.argv) > 38 else None
    stage2_sectors = 8
    stage2_lba = 1
    kernel_lba = 9
    fsinfo_sector = reserved_arg
    backup_boot_sector = reserved_arg + 1
    backup_fsinfo_sector = reserved_arg + 2
    reserved_sectors = reserved_arg + 3

    # Validate the complete runtime before replacing an existing build image.
    graphics_files = native_graphics_payloads()
    image.parent.mkdir(parents=True, exist_ok=True)
    if image.exists():
        image.unlink()
    bfs_build = os.environ.get("BLES_FS", "bfs4").lower() != "fat32"
    if bfs_build:
        stage = tempfile.TemporaryDirectory(prefix="bleskernos-bfs4-")
        atexit.register(stage.cleanup)
        BFS_STAGING = Path(stage.name)
        with image.open("wb") as handle:
            handle.truncate(image_size_kb * 1024)
    else:
        run([
            "mkfs.fat", "-C", "-F", "32", "-n", "BLESKERNOS",
            "-R", str(reserved_sectors), "-b", str(backup_boot_sector),
            image, str(image_size_kb),
        ])
        fsinfo = read_sector(image, 1)
        write_sector(image, fsinfo_sector, fsinfo)
        write_sector(image, backup_fsinfo_sector, fsinfo)
        set_fat32_info_sectors(image, fsinfo_sector, backup_boot_sector)

    if boot_sector_path and stage2_path and kernel_path:
        if not bfs_build:
            zero_sector_range(image, stage2_lba, fsinfo_sector - stage2_lba)
        write_raw_sector_data(
            image, stage2_lba, stage2_path.read_bytes(), stage2_sectors,
            "stage2",
        )
        write_raw_sector_data(
            image, kernel_lba, kernel_path.read_bytes(), kernel_reserved_sectors,
            "kernel",
        )
        if bfs_build:
            write_sector(image, 0, boot_sector_path.read_bytes())
        else:
            patch_boot_sector(image, boot_sector_path, 0)
            patch_boot_sector(image, boot_sector_path, backup_boot_sector)

    mmd(image, "::/DOCS")
    mmd(image, "::/MISC")
    mmd(image, "::/LIB")
    # BLES_WINE_TEMP_SFX_FIX_20260723: Win9x installers expect this
    # directory to exist before querying TEMP/TMP or calling _lcreat.
    mmd(image, "::/TEMP")
    mmd(image, "::/ICONS")
    mmd(image, "::/SYSTEM")
    mmd(image, "::/SYSTEM/PROGRAMS")
    mmd(image, "::/SYSTEM/FONTS")
    mmd(image, "::/SYSTEM/COMMANDS")
    mmd(image, "::/SYSTEM/CORE")
    # Wine's Windows-visible C: lives on the normal BFS volume.  The Wine
    # runtime and registry stay private under /OPT/WINE and /WINEGATE, while
    # applications installed by the user are ordinary files under /SYSTEM/WINE.
    mmd(image, "::/SYSTEM/WINE")
    mmd(image, "::/SYSTEM/USER")
    mmd(image, "::/SYSTEM/USER/CONFIG")
    mmd(image, "::/SYSTEM/LENG")
    mmd(image, "::/SYSTEM/LIBS")
    mmd(image, "::/SYSTEM/LIBS/POSIX")
    for native_lib in sorted((ROOT_DIR / "build/system/libs/native").glob("*.LBS")):
        mcopy(image, native_lib, "::/SYSTEM/" + native_lib.name)
    mmd(image, "::/SYSTEM/LIBS/TINYGL")
    mmd(image, "::/SYSTEM/LIBS/MESA35")
    mmd(image, "::/SYSTEM/LIBS/LIBC")
    mmd(image, "::/SYSTEM/SERVICES")
    mmd(image, "::/SYSTEM/SCREENS")
    mmd(image, "::/SYSTEM/CONTROL")
    mmd(image, "::/SYSTEM/DRIVERS")
    mmd(image, "::/SYSTEM/PRINTERS")
    mmd(image, "::/SYSTEM/FIRMWARE")
    mmd(image, "::/SYSTEM/SOUNDS")
    mmd(image, "::/SYSTEM/WALLPAPR")
    mmd(image, "::/SYSTEM/CURSORS")
    mmd(image, "::/SYSTEM/DOCS")
    for guest, source in documentation_payloads().items():
        mcopy(image, source, "::" + guest)
    mmd(image, "::/SYSTEM/SDK")
    mmd(image, "::/SYSTEM/SDK/INCLUDE")
    mmd(image, "::/SYSTEM/SDK/LIB")
    mmd(image, "::/SYSTEM/SDK/EXAMPLES")
    mmd(image, "::/SYSTEM/SDK/EXAMPLES/HELLO")
    mmd(image, "::/SYSTEM/DEVEL")
    mmd(image, "::/SYSTEM/DESKTOP")
    mmd(image, "::/SYSTEM/TRASH")
    if developer:
        mmd(image, "::/SYSTEM/SOURCE")
        mmd(image, "::/SYSTEM/TESTS")
        mmd(image, "::/SYSTEM/TESTS/NATIVE")
        mmd(image, "::/SYSTEM/TESTS/NATIVE/LIB")

    with tempfile.TemporaryDirectory(prefix="bleskernos-ata-") as tmp:
        tmpdir = Path(tmp)

        edition_title = "Developer" if developer else "User"
        root_readme = (
            f"BlesKernOS 0.9 - {edition_title} Edition\r\n"
            "Apps: /SYSTEM/PROGRAMS\r\n"
            "Commands: /SYSTEM/COMMANDS\r\n"
            "Core: /SYSTEM/CORE\r\n"
            "Windows runtime: /OPT/WINE (Wine 11)\r\n"
            "Native SDK: /SYSTEM/SDK\r\n"
            "Build toolchain: /SYSTEM/DEVEL\r\n"
        )
        if developer:
            root_readme += (
                "Developer tests: /SYSTEM/TESTS\r\n"
                "OS sources: /SYSTEM/SOURCE\r\n"
            )
        text_files = {
            "::/README.TXT": root_readme.encode("ascii"),
            "::/SYSTEM/EDITION.TXT": (edition + "\r\n").encode("ascii"),
            "::/DOCS/README.TXT": (
                f"BlesKernOS 0.9 {edition_title} Edition.\r\n"
                "Este disco ATA usa FAT32.\r\n"
                "Abra Help Center para tareas comunes y diagnostico.\r\n"
            ).encode("ascii"),
            "::/DOCS/ROADMAP.TXT": b"Roadmap\r\n- Estabilidad\r\n- Drivers reales\r\n- Compatibilidad Win32\r\n",
            "::/MISC/PALETTE.TXT": b"Paleta GUI\r\n- celeste\r\n- gris 90s\r\n- verde suave\r\n",
            "::/MISC/THEMES.TXT": b"Themes\r\nClassic 90s\r\nHaiku Soft\r\n",
            "::/SYSTEM/USER/CONFIG/LANGUAGE.INI": b"language=ES\r\n",
            "::/SYSTEM/USER/CONFIG/DATETIME.INI": b"format=24\r\ntimezone=0\r\n",
            "::/SYSTEM/USER/CONFIG/MOUSE.INI": b"sensitivity=3\r\ntrail=0\r\n",
            "::/SYSTEM/USER/CONFIG/DISPLAY.INI": b"drag_outline=1\r\n",
            "::/SYSTEM/USER/CONFIG/SOUND.INI": (
                b"[SOUND]\r\nStartupEnabled=1\r\n"
                b"StartupSound=/SYSTEM/SOUNDS/START.WAV\r\n"
                b"SystemEventsEnabled=1\r\n"
                b"MasterVolume=30\r\n"
                b"WindowOpen=1\r\nWindowClose=1\r\n"
                b"WindowMinimize=1\r\nWindowRestore=1\r\n"
                b"WindowResize=1\r\n"
                b"Button=1\r\nScroll=1\r\n"
            ),
            "::/SYSTEM/USER/CONFIG/PRINTER.INI": (
                b"[PRINT]\r\nDefault=PSFILE\r\n"
            ),
            "::/SYSTEM/USER/CONFIG/INTERNET.INI": (
                b"home=https://www.google.com/\r\n"
                b"images=1\r\ncss=1\r\ncookies=1\r\ntimeout=10000\r\n"
            ),
            "::/SYSTEM/USER/CONFIG/MODEM.INI": (
                b"port=COM1\r\nbaud=57600\r\nprefix=ATDT\r\n"
                b"tone=1\r\nspeaker=1\r\n"
            ),
        }
        text_files["::/SYSTEM/SDK/README.TXT"] = (
            b"BlesKernOS native SDK\r\n"
            b"INCLUDE/BLESKERNOS_API.H: public API 40\r\n"
            b"LIB/LIBBLESK.A: native syscall and printing library\r\n"
            b"LIB/LIBPOSIX.A: POSIX libc, pthreads, mmap and IPv4 sockets\r\n"
            b"LIB/LIBLBS.A + LBSHOST.O: native dynamic LBS and startup\r\n"
            b"LIB/LIBSDL2.A: SDL2-compatible software graphics layer\r\n"
            b"LIB/LIBM.A: portable floating-point math library\r\n"
            b"Tools: gcc cc ld as ar ranlib objcopy objdump readelf nm size strip\r\n"
            b"       addr2line cxxfilt elfedit strings nasm ndisasm make fatpack build\r\n"
            b"Use: build SOURCE.C PROGRAM.BEX\r\n"
            b"Makefiles: make -f MAKEFILE [TARGET]\r\n"
            b"FAT32 images: fatpack --help\r\n"
        )

        if desktop_path:
            desktop = load(desktop_path, "Desktop INI")
            if desktop is not None:
                text_files["::/SYSTEM/USER/CONFIG/DESKTOP.INI"] = crlf(desktop)

        associations = load(associations_path, "Associations INI")
        if associations is not None:
            text_files["::/ASSOC.INI"] = crlf(associations)

        screensv = (
            b"enabled=1\r\n"
            b"timeout=300\r\n"
            b"path=/SYSTEM/SCREENS/SSLOGO.SCV\r\n"
        )
        if screensv_path:
            data = load(screensv_path, "ScreenSaver INI")
            if data is not None:
                screensv = crlf(data)
        text_files["::/SYSTEM/USER/CONFIG/SCREENSV.INI"] = screensv

        for index, (dst, data) in enumerate(text_files.items()):
            src = write_payload(tmpdir, f"text{index}.tmp", data)
            mcopy(image, src, dst)

        for source in sorted((ROOT_DIR / "assets" / "fonts").glob("*.BFN")):
            mcopy(image, source, f"::/SYSTEM/FONTS/{source.name}")
        mcopy(image, ROOT_DIR / "assets/fonts/LICENSE.txt", "::/SYSTEM/FONTS/LICENSE.TXT")

        native_programs = [
            ("lbstri.o", ROOT_DIR / "build/tests/userland/LBSTRI.BEX", "LBS 3D demonstration"),
            ("font.o", ROOT_DIR / "build/programs/font.o", "Font viewer"),
            ("shell.o", shell_path, "Shell"),
            ("about.o", ROOT_DIR / "build" / "programs" / "about.o", "About"),
            ("runbox.o", ROOT_DIR / "build" / "programs" / "runbox.o", "Runbox"),
            ("file.o", filebrowser_path, "Filebrowser"),
            ("texteditor.o", texteditor_path, "Texteditor"),
            ("calculator.o", calculator_path, "Calculator"),
            ("midamp.o", midamp_path, "Midamp"),
            ("processmanager.o", processmanager_path, "Processmanager"),
            ("calendar.o", calendar_path, "Calendar"),
            ("paint.o", ROOT_DIR / "build" / "programs" / "paint.o", "Paint"),
            ("imageviewer.o", ROOT_DIR / "build" / "programs" / "imageviewer.o", "Imageviewer"),
            ("viewer.o", ROOT_DIR / "build/programs/viewer.o", "Viewer"),
            ("scandisk.o", ROOT_DIR / "build" / "programs" / "scandisk.o", "ScanDisk"),
            ("wine.o", ROOT_DIR / "build" / "programs" / "wine.o", "Wine PE launcher"),
            ("netsurf.o", ROOT_DIR / "build" / "programs" / "netsurf.o", "NetSurf bootstrap frontend"),
            ("help_center.o", ROOT_DIR / "build" / "programs" / "help_center.o", "Help Center"),
            ("find_files.o", ROOT_DIR / "build" / "programs" / "find_files.o", "Find Files"),
            ("archive_manager.o", ROOT_DIR / "build" / "programs" / "archive_manager.o", "Archive Manager"),
            ("disk_tools.o", ROOT_DIR / "build" / "programs" / "disk_tools.o", "Disk Tools"),
            ("network_status.o", ROOT_DIR / "build" / "programs" / "network_status.o", "Network Status"),
            ("clipboard_viewer.o", ROOT_DIR / "build" / "programs" / "clipboard_viewer.o", "Clipboard Viewer"),
            ("defrag.o", ROOT_DIR / "build" / "programs" / "defrag.o", "Desfragmentador de disco"),
            ("event_viewer.o", ROOT_DIR / "build" / "programs" / "event_viewer.o", "Event Viewer"),
            ("performance_monitor.o", ROOT_DIR / "build" / "programs" / "performance_monitor.o", "Performance Monitor"),
        ]
        if developer:
            native_programs.extend([
                ("gears.o", ROOT_DIR / "build" / "programs" / "gears.o", "TinyGL Gears demo"),
                ("apitest.o", ROOT_DIR / "build" / "programs" / "apitest.o", "APITest"),
                ("phase2.o", ROOT_DIR / "build" / "programs" / "phase2.o", "Phase II syscall test"),
                ("posix_test.o", ROOT_DIR / "build" / "programs" / "posix_test.o", "POSIX syscall/libc test"),
                ("linux_abi_test.o", ROOT_DIR / "build" / "programs" / "linux_abi_test.o", "Linux i386 ABI test"),
                ("sdl_demo.o", ROOT_DIR / "build" / "programs" / "sdl_demo.o", "SDL2/POSIX graphics demo"),
                ("ring3proxy.o", ROOT_DIR / "build" / "programs" / "ring3proxy.o", "Ring 3 API proxy self-test"),
                ("print_test.o", ROOT_DIR / "build" / "programs" / "print_test.o", "Printing subsystem test"),
            ])
        developer_native = {"gears.o", "apitest.o", "phase2.o", "posix_test.o", "linux_abi_test.o", "sdl_demo.o", "ring3proxy.o", "print_test.o"}
        for dst_name, src_path, label in native_programs:
            data = load(src_path, label)
            if data is not None:
                installed_name = str(Path(dst_name).with_suffix(".BEX")).upper()
                src = write_payload(tmpdir, installed_name, data)
                if developer and dst_name in developer_native:
                    mcopy(image, src,
                          f"::/SYSTEM/TESTS/NATIVE/{installed_name}")
                else:
                    mcopy(image, src,
                          f"::/SYSTEM/PROGRAMS/{installed_name}")

            # Los artefactos ELF/Win32 de abajo pertenecen sólo a la edición
            # developer, cuyas carpetas TESTS se crean arriba.
            if not developer:
                continue

            module_path = ROOT_DIR / "build" / "tests" / "userland" / "DLMOD.SO"
            module_data = load(module_path, "ELF32 dynamic loader test module")
            if module_data is not None:
                module_source = write_payload(tmpdir, "DLMOD.SO", module_data)
                mcopy(image, module_source, "::/SYSTEM/TESTS/NATIVE/DLMOD.SO")
            dependency_path = ROOT_DIR / "build" / "tests" / "userland" / "DLDEP.SO"
            dependency_data = load(dependency_path, "ELF32 DT_NEEDED test dependency")
            if dependency_data is not None:
                dependency_source = write_payload(tmpdir, "DLDEP.SO", dependency_data)
                mcopy(image, dependency_source, "::/SYSTEM/TESTS/NATIVE/DLDEP.SO")
            tls_dependency_path = ROOT_DIR / "build" / "tests" / "userland" / "TLSDEP.SO"
            tls_dependency_data = load(tls_dependency_path, "Linux i386 static TLS dependency")
            if tls_dependency_data is not None:
                tls_dependency_source = write_payload(tmpdir, "TLSDEP.SO", tls_dependency_data)
                mcopy(image, tls_dependency_source, "::/SYSTEM/TESTS/NATIVE/LIB/TLSDEP.SO")
            pie_path = ROOT_DIR / "build" / "tests" / "userland" / "LINUXPIE.ELF"
            pie_data = load(pie_path, "Linux ELF32 PIE executable test")
            if pie_data is not None:
                pie_source = write_payload(tmpdir, "LINUXPIE.ELF", pie_data)
                mcopy(image, pie_source, "::/SYSTEM/TESTS/NATIVE/LINUXPIE.ELF")
            interp_test_path = ROOT_DIR / "build" / "tests" / "userland" / "INTERPPIE.ELF"
            interp_test_data = load(interp_test_path, "Linux PT_INTERP integration test")
            if interp_test_data is not None:
                interp_test_source = write_payload(tmpdir, "INTERPPIE.ELF", interp_test_data)
                mcopy(image, interp_test_source, "::/SYSTEM/TESTS/NATIVE/INTERPPIE.ELF")
            dynamic_test_path = ROOT_DIR / "build" / "tests" / "userland" / "DYNAMIC.ELF"
            dynamic_test_data = load(dynamic_test_path, "Linux DT_NEEDED/PLT integration test")
            if dynamic_test_data is not None:
                dynamic_test_source = write_payload(tmpdir, "DYNAMIC.ELF", dynamic_test_data)
                mcopy(image, dynamic_test_source, "::/SYSTEM/TESTS/NATIVE/DYNAMIC.ELF")
            exec_test_path = ROOT_DIR / "build" / "tests" / "userland" / "EXECFIX.ELF"
            exec_test_data = load(exec_test_path, "Linux fixed ET_EXEC/CR3 integration test")
            if exec_test_data is not None:
                exec_test_source = write_payload(tmpdir, "EXECFIX.ELF", exec_test_data)
                mcopy(image, exec_test_source, "::/SYSTEM/TESTS/NATIVE/EXECFIX.ELF")
            glibc_test_path = ROOT_DIR / "build" / "tests" / "userland" / "GLIBCHEL.ELF"
            glibc_test_data = load(glibc_test_path, "Linux GCC/glibc ABI integration test")
            if glibc_test_data is not None:
                glibc_test_source = write_payload(tmpdir, "GLIBCHEL.ELF", glibc_test_data)
                mcopy(image, glibc_test_source, "::/SYSTEM/TESTS/NATIVE/GLIBCHEL.ELF")
            libc_linux_path = ROOT_DIR / "build" / "system" / "libs" / "posix" / "libc.so.6"
            libc_linux_data = load(libc_linux_path, "BlesK Linux libc.so.6 compatibility layer")
            if libc_linux_data is not None:
                libc_linux_source = write_payload(tmpdir, "LIBC.SO.6", libc_linux_data)
                mcopy(image, libc_linux_source, "::/LIB/LIBC.SO.6")
                mcopy(image, libc_linux_source, "::/SYSTEM/LIBS/POSIX/LIBC.SO.6")
            loader_path = ROOT_DIR / "build" / "system" / "libs" / "posix" / "ld-blesk.so"
            loader_data = load(loader_path, "BlesK Linux dynamic interpreter")
            if loader_data is not None:
                loader_source = write_payload(tmpdir, "LD-BLESK.SO", loader_data)
                mcopy(image, loader_source, "::/SYSTEM/LIBS/POSIX/LD-BLESK.SO")
                mcopy(image, loader_source, "::/LIB/LD-LINUX.SO.2")

        command_names = (
            "help echo about uname hostname uptime date time shutdown reboot sleep bosrecovery "
            "fdisk format mount unmount label checkdisk fsinfo backup dir ls copy cp move "
            "delete rm mkdir rmdir rename touch tree find attrib chmod type more cat "
            "diff ps kill tasklist taskkill top nice pci usb lspci lsusb "
            "cpuinfo mem soundtest ipconfig ping netstat ftp wget curl compile link "
            "gcc cc ld as ar ranlib build nasm ndisasm make fatpack "
            "addr2line cxxfilt elfedit objcopy objdump readelf nm size strip hexdump strings calc hexedit compress extract "
            "checksum benchmark start"
        ).split()
        for command_name in command_names:
            filename = f"{command_name}.BEX"
            command_object = (ROOT_DIR / "build" / "system" / "commands" /
                              f"{command_name}.o")
            command_data = load(command_object, f"Ring 3 command {command_name}")
            if command_data is not None:
                src = write_payload(tmpdir, filename, command_data)
                mcopy(image, src, f"::/SYSTEM/COMMANDS/{filename}")
        # The manifest is authoritative, including newer commands absent from
        # the historical install list above. Keep its legacy aliases as well.
        for guest, source in application_payloads(edition == "developer").items():
            mcopy(image, source, "::" + guest)
        sdk_include = ROOT_DIR / "sdk" / "include"
        for directory in sorted(path for path in sdk_include.rglob("*")
                                if path.is_dir()):
            relative = directory.relative_to(sdk_include).as_posix().upper()
            mmd(image, f"::/SYSTEM/SDK/INCLUDE/{relative}")
        for source in sorted(path for path in sdk_include.rglob("*")
                             if path.is_file() and ".pre-" not in path.name):
            relative = source.relative_to(sdk_include).as_posix().upper()
            mcopy(image, source, f"::/SYSTEM/SDK/INCLUDE/{relative}")
        for source in sorted((ROOT_DIR / "sdk" / "examples" / "selfhost").iterdir()):
            if source.is_file():
                mcopy(image, source,
                      f"::/SYSTEM/SDK/EXAMPLES/HELLO/{source.name.upper()}")

        selfhost = ROOT_DIR / "build" / "selfhost" / "toolchain"
        if not selfhost.is_dir():
            raise RuntimeError(
                "toolchain self-hosting ausente; ejecute make selfhost"
            )
        for directory in sorted(path for path in selfhost.rglob("*")
                                if path.is_dir()):
            relative = directory.relative_to(selfhost).as_posix().upper()
            mmd(image, f"::/SYSTEM/DEVEL/{relative}")
        for source in sorted(path for path in selfhost.rglob("*")
                             if path.is_file()):
            relative = source.relative_to(selfhost).as_posix().upper()
            mcopy(image, source, f"::/SYSTEM/DEVEL/{relative}")

        if developer:
            source_roots = (
                "boot", "kernel", "gui", "programs", "system", "sdk",
                "config", "mk", "docs", "libs", "assets", "tools",
            )
            for root_name in source_roots:
                source_root = ROOT_DIR / root_name
                mmd(image, f"::/SYSTEM/SOURCE/{root_name.upper()}")
                for directory in sorted(path for path in source_root.rglob("*")
                                        if path.is_dir() and
                                        "__pycache__" not in
                                        path.relative_to(source_root).parts):
                    relative = directory.relative_to(source_root).as_posix().upper()
                    mmd(image, f"::/SYSTEM/SOURCE/{root_name.upper()}/{relative}")
                for source in sorted(path for path in source_root.rglob("*")
                                     if path.is_file() and
                                     "__pycache__" not in
                                     path.relative_to(source_root).parts and
                                     path.suffix.lower() != ".pyc" and
                                     ".pre-" not in path.name and
                                     not path.name.endswith((".rej", ".bak", ".orig"))):
                    relative = source.relative_to(source_root).as_posix().upper()
                    mcopy(image, source,
                          f"::/SYSTEM/SOURCE/{root_name.upper()}/{relative}")
            for source in (ROOT_DIR / "Makefile", ROOT_DIR / "GNUmakefile",
                           ROOT_DIR / "mk/selfhost.mk",
                           ROOT_DIR / "docs" / "build" / "self-hosting.md"):
                mcopy(image, source,
                      f"::/SYSTEM/SOURCE/{source.name.upper()}")

        for language_name in ("ESPANOL.LNG", "ENGLISH.LNG", "ITALIANO.LNG"):
            language_path = ROOT_DIR / "system" / "leng" / language_name
            language_data = load(language_path, f"Language pack {language_name}")
            if language_data is not None:
                src = write_payload(tmpdir, language_name, language_data)
                mcopy(image, src, f"::/SYSTEM/LENG/{language_name}")

        wallpapers_dir = ROOT_DIR / "assets" / "wallpapers"
        if wallpapers_dir.is_dir():
            for wallpaper_path in sorted(wallpapers_dir.iterdir()):
                if not wallpaper_path.is_file() or not is_wallpaper_file(wallpaper_path):
                    continue
                mcopy(image, wallpaper_path,
                      f"::/SYSTEM/WALLPAPR/{wallpaper_path.name}")

        cursor_dir = ROOT_DIR / "assets" / "cursors"
        for cursor_path in sorted(cursor_dir.iterdir()):
            if cursor_path.is_file():
                mcopy(image, cursor_path, f"::/SYSTEM/CURSORS/{cursor_path.name}")
        system_files = [
            ("::/SYSTEM/CORE/deskbar.o", ROOT_DIR / "build" / "system" / "desktop" / "deskbar.o", "Deskbar core"),
            ("::/SYSTEM/CORE/deskmanager.o", ROOT_DIR / "build" / "system" / "desktop" / "deskmanager.o", "Desktop manager core"),
            ("::/SYSTEM/CORE/launcher.o", ROOT_DIR / "build" / "programs" / "launcher.o", "Program launcher core"),
            ("::/SYSTEM/DRIVERS/AC97.DVR", ROOT_DIR / "build" / "system" / "drivers" / "AC97.DVR", "ICH-compatible AC97 driver"),
            ("::/SYSTEM/DRIVERS/MAESTRO3.DVR", ROOT_DIR / "build" / "system" / "drivers" / "MAESTRO3.DVR", "ESS Allegro/Maestro3 DSP driver"),
            ("::/SYSTEM/DRIVERS/SB16.DVR", ROOT_DIR / "build" / "system" / "drivers" / "SB16.DVR", "Sound Blaster 16 driver"),
            ("::/SYSTEM/DRIVERS/CMOSRTC.DVR", ROOT_DIR / "build" / "system" / "drivers" / "CMOSRTC.DVR", "CMOS RTC driver"),
            ("::/SYSTEM/DRIVERS/ISO9660.DVR", ROOT_DIR / "build" / "system" / "drivers" / "ISO9660.DVR", "ISO9660 driver"),
            ("::/SYSTEM/DRIVERS/PS2MOUSE.DVR", ROOT_DIR / "build" / "system" / "drivers" / "PS2MOUSE.DVR", "PS/2 mouse driver"),
            ("::/SYSTEM/DRIVERS/VESA.DVR", ROOT_DIR / "build" / "system" / "drivers" / "VESA.DVR", "VESA framebuffer driver"),
            ("::/SYSTEM/DRIVERS/VMWARESVGA.DVR", ROOT_DIR / "build" / "system" / "drivers" / "VMWARESVGA.DVR", "VMware SVGA-II 2D transport driver"),
            ("::/SYSTEM/DRIVERS/VMWARESVGA3D.DVR", ROOT_DIR / "build" / "system" / "drivers" / "VMWARESVGA3D.DVR", "VMware SVGA3D extension driver"),
            ("::/SYSTEM/DRIVERS/3C90X.DVR", ROOT_DIR / "build" / "system" / "drivers" / "3C90X.DVR", "3Com EtherLink XL driver"),
            ("::/SYSTEM/DRIVERS/NETSTACK.DVR", ROOT_DIR / "build" / "system" / "drivers" / "NETSTACK.DVR", "IPv4 network stack"),
            ("::/SYSTEM/DRIVERS/RTL8139.DVR", ROOT_DIR / "build" / "system" / "drivers" / "RTL8139.DVR", "Realtek RTL8139 driver"),
            ("::/SYSTEM/DRIVERS/TLS.DVR", ROOT_DIR / "build" / "system" / "drivers" / "TLS.DVR", "BearSSL TLS 1.2 provider"),
            ("::/SYSTEM/SERVICES/SCREENSV.BEX", screensaverd_path, "ScreenSaver daemon"),
            ("::/SYSTEM/SERVICES/VISION.BEX", ROOT_DIR / "build" / "system" / "services" / "vision.o", "Vision GUI service"),
            ("::/SYSTEM/SERVICES/PRINTSPL.BEX", ROOT_DIR / "build" / "system" / "services" / "printspool.o", "PrintSpool daemon"),
            ("::/SYSTEM/SCREENS/SSLOGO.SCV", ss_logo_path, "ScreenSaver logo"),
            ("::/SYSTEM/SCREENS/SSPIPES.SCV", ss_pipes_path, "ScreenSaver pipes"),
            ("::/SYSTEM/SCREENS/SSBALLS.SCV", ss_balls_path, "ScreenSaver balls"),
            ("::/SYSTEM/LIBS/TINYGL/TINYGL.A", tinygl_path, "TinyGL library"),
            ("::/SYSTEM/LIBS/MESA35/MESA35.A", mesa35_path, "Mesa 3.5 GFX3D/OSMesa library"),
            ("::/SYSTEM/LIBS/LIBC/LIBC.A", libc_path, "C library"),
        ]
        system_files.extend([
            ("::/SYSTEM/SDK/LIB/LIBBLESK.A", ROOT_DIR / "build/sdk/libblesk.a", "native SDK"),
            ("::/SYSTEM/SDK/LIB/LIBPOSIX.A", ROOT_DIR / "build/sdk/libblesk_posix.a", "POSIX SDK with LBS loader"),
            ("::/SYSTEM/SDK/LIB/LIBLBSPX.A", ROOT_DIR / "build/sdk/libblesk_lbs_posix.a", "LBS POSIX runtime"),
            ("::/SYSTEM/SDK/LIB/LIBLBSNT.A", ROOT_DIR / "build/sdk/libblesk_lbs_native.a", "LBS native runtime"),
            ("::/SYSTEM/SDK/LIB/LIBLBS.A", ROOT_DIR / "build/sdk/libblesk_lbs.a", "LBS startup SDK"),
            ("::/SYSTEM/SDK/LIB/LBSHOST.O", ROOT_DIR / "build/sdk/lbs_host.o", "LBS Ring 3 imports"),
        ])
        control_names = [
            ("CONTROL.BEX", "Control Panel"),
            ("APPEAR.CPL", "Appearance CPL"),
            ("DISPLAY.CPL", "Display CPL"),
            ("SOUND.CPL", "Sound CPL"),
            ("DATETIME.CPL", "Date and Time CPL"),
            ("MOUSE.CPL", "Mouse CPL"),
            ("KEYBOARD.CPL", "Keyboard CPL"),
            ("SYSTEM.CPL", "System CPL"),
            ("DEVMGR.CPL", "Device Manager CPL"),
            ("INTERNET.CPL", "Internet CPL"),
            ("MODEM.CPL", "Modem CPL"),
            ("LANGUAGE.CPL", "Language CPL"),
            ("WINE.CPL", "Windows applications CPL"),
        ]
        for (name, label), src_path in zip(control_names, control_paths):
            system_files.append((f"::/SYSTEM/CONTROL/{name}", src_path, label))
        # AUTO-DVR-PACKAGING: copy every built resident driver
        driver_build_dir = ROOT_DIR / "build" / "system" / "drivers"
        known_driver_destinations = {
            dst.upper() for dst, _src, _label in system_files
            if dst.upper().startswith("::/SYSTEM/DRIVERS/")
        }
        if driver_build_dir.is_dir():
            for driver_path in sorted(driver_build_dir.glob("*.DVR")):
                destination = f"::/SYSTEM/DRIVERS/{driver_path.name.upper()}"
                if destination.upper() in known_driver_destinations:
                    continue
                system_files.append((
                    destination,
                    driver_path,
                    f"Resident driver {driver_path.name}",
                ))
                known_driver_destinations.add(destination.upper())

        for dst, src_path, label in system_files:
            data = load(src_path, label)
            if data is not None:
                src = write_payload(tmpdir, Path(dst).name, data)
                mcopy(image, src, dst)

        # Combined index plus original notices for bundled libraries.
        notices = write_notices(Path(tmpdir) / "LICENSES.TXT")
        mcopy(image, notices, "::/DOCS/LICENSES.TXT")

        # Copied Linux PRO/100 template and PCI identity database: ship sources/licenses.
        family_source = build_family_source_archive(Path(tmpdir) / "FAMILYSRC.TGZ")
        mcopy(image, family_source, "::/DOCS/FAMILYSRC.TGZ")
        for source, destination in [
            ("LICENSES/GPL-2.0.txt", "NETGPL.TXT"),
            ("third_party/pciids/COPYING", "PCIIDSLICENSE.TXT"),
            ("docs/platform/family-drivers.md", "FAMILYDRIVERS.TXT"),
            ("docs/family-expansion-2026-10-13.md", "FAMILYTESTS.TXT"),
            ("docs/platform/family-driver-sources.json", "FAMILYSOURCES.JSON"),
            ("docs/platform/framebuffer-device-catalog.json", "FRAMEBUFFERIDS.JSON")]:
            mcopy(image, ROOT_DIR / source, "::/DOCS/" + destination)

        # The combined PrintSpool executable links GPL JBIG-KIT: ship source.
        mcopy(image, ROOT_DIR / "third_party/jbigkit/COPYING", "::/DOCS/PRINTGPL.TXT")
        source_archive = build_print_source_archive(Path(tmpdir) / "PRINTSRC.TGZ")
        mcopy(image, source_archive, "::/DOCS/PRINTSRC.TGZ")
        mcopy(image, ROOT_DIR / "system/firmware/README.md", "::/SYSTEM/FIRMWARE/README.TXT")
        for filename in ["SIHP1000.DL", "SIHP1005.DL", "SIHP1018.DL", "SIHP1020.DL"]:
            firmware = ROOT_DIR / "system/firmware" / filename
            if firmware.is_file():
                mcopy(image, firmware, "::/SYSTEM/FIRMWARE/" + filename)

        mcopy(image, ROOT_DIR / "docs/driver-source-layout.md", "::/DOCS/DRIVERSPLIT.TXT")
        mcopy(image, ROOT_DIR / "docs/platform/legacy-drivers.md", "::/DOCS/LEGACYDRIVERS.TXT")
        mcopy(image, ROOT_DIR / "docs/platform/legacy-driver-sources.json", "::/DOCS/DRIVERSOURCES.JSON")
        printing_doc = ROOT_DIR / "docs" / "platform" / "printing.md"
        if printing_doc.is_file():
            mcopy(image, printing_doc, "::/DOCS/PRINTING.TXT")
        else:
            print(f"Warning: printing documentation missing: {printing_doc}", file=sys.stderr)

        usbclass_doc = ROOT_DIR / "docs" / "platform" / "usbclass.md"
        if usbclass_doc.is_file():
            mcopy(image, usbclass_doc, "::/DOCS/USBCLASS.TXT")
        else:
            print(f"Warning: USB class documentation missing: {usbclass_doc}", file=sys.stderr)

        printer_profiles = ROOT_DIR / "system" / "printers"
        if printer_profiles.is_dir():
            for profile_path in sorted(printer_profiles.glob("*.BPD")):
                mcopy(image, profile_path,
                      f"::/SYSTEM/PRINTERS/{profile_path.name.upper()}")

        sound_assets = [
            ("START.WAV", "startup.wav", "Startup WAV"),
            ("WINOPEN.WAV", "winopen.wav", "Window open sound"),
            ("WINCLOSE.WAV", "winclose.wav", "Window close sound"),
            ("MINIMIZE.WAV", "minimize.wav", "Window minimize sound"),
            ("RESTORE.WAV", "restore.wav", "Window restore sound"),
            ("RESIZE.WAV", "resize.wav", "Window resize sound"),
            ("BOTTON.WAV", "botton.wav", "GUI button sound"),
            ("SCROLL.WAV", "scroll.wav", "GUI scroll sound"),
        ]
        for dst_name, src_name, label in sound_assets:
            sound_path = ROOT_DIR / "assets" / "sounds" / src_name
            sound_data = load(sound_path, label)
            if sound_data is not None:
                src = write_payload(tmpdir, dst_name, sound_data)
                mcopy(image, src, f"::/SYSTEM/SOUNDS/{dst_name}")

        graphics_pak = ROOT_DIR / "assets" / "graphics" / "GRAPHICS.PAK"
        graphics_data = load(graphics_pak, "Shared graphics package")
        if graphics_data is not None:
            src = write_payload(tmpdir, "GRAPHICS.PAK", graphics_data)
            mcopy(image, src, "::/SYSTEM/GRAPHICS.PAK")

        for destination, source_path in (
            ("GRAPHICS.CSV", ROOT_DIR / "assets/graphics/GRAPHICS.CSV"),
            ("KDE1SRC.TGZ", ROOT_DIR / "assets/graphics/KDE1SRC.TGZ"),
            ("KDE1LIC.TXT", ROOT_DIR / "third_party/kde1/README.md"),
        ):
            mcopy(image, source_path, f"::/SYSTEM/{destination}")

        skin_dir = ROOT_DIR / "programs" / "winmap"
        root_assets = [
            ("MIDHDR.GIF", skin_dir / "hdr.gif", "Midamp HDR"),
            ("MIDBTN.GIF", skin_dir / "buttons.gif", "Midamp BTN"),
            ("MIDBOT.GIF", skin_dir / "bottom.gif", "Midamp BOT"),
            ("ABOUNT.GIF", about_gif_path, "About GIF"),
        ]
        for dst_name, src_path, label in root_assets:
            data = load(src_path, label)
            if data is not None:
                src = write_payload(tmpdir, dst_name, data)
                mcopy(image, src, f"::/{dst_name}")

        if icons_path and icons_path.is_dir():
            for src_path in sorted(icons_path.iterdir()):
                if src_path.is_file():
                    mcopy(image, src_path, f"::/ICONS/{src_path.name.upper()}")

        # The CDE Deskbar expects this exact canonical runtime path.  Copy it
        # explicitly even though the generic loop above normally already did;
        # this makes the build deterministic when the source asset was placed
        # in a legacy top-level icons/ directory and prints what was packaged.
        bles_start_icon = find_bleskernos_start_icon(icons_path)
        if bles_start_icon is not None:
            mcopy(image, bles_start_icon, "::/ICONS/BLESKERNOS.BMP")
            print(f"[ATA] Start icon: {bles_start_icon} -> "
                  "/ICONS/BLESKERNOS.BMP", file=sys.stderr)
        else:
            print("Warning: BLESKERNOS.BMP not found; Deskbar will use its "
                  "fallback mark", file=sys.stderr)

        wine_root = ROOT_DIR / "ports/wine/package/wine-11.0-i386/rootfs"
        wine_program = wine_root / "opt/wine/bin/wine"
        tinyx_program = wine_root / "WINEGATE/bin/XFBDEV"
        launcher = ROOT_DIR / "build/wine/wine-launch"
        tool_launcher = ROOT_DIR / "build/wine/wine-tool"
        for required in (wine_program, tinyx_program, launcher, tool_launcher):
            if not required.is_file():
                raise FileNotFoundError(f"Wine 11 default build requires {required}")
        mmd(image, "::/OPT")
        mcopy_tree(image, wine_root / "opt/wine", "::/OPT/WINE")
        # Keep Wine's Unix support private, but place drive C: on the normal
        # BlesKernOS filesystem so Archivos and native programs can browse it.
        # wine-launch creates $WINEPREFIX/drive_c as a compatibility symlink.
        mcopy_tree(image, wine_root / "WINEGATE", "::/WINEGATE",
                   exclude=("prefix/drive_c",))
        user_reg, system_reg, theme_marker = theme_defaults(
            (wine_root / "WINEGATE/prefix/user.reg").read_bytes(),
            (wine_root / "WINEGATE/prefix/system.reg").read_bytes(),
            (ROOT_DIR / "ports/tinyx/wine-launch.c").read_text())
        display_reg = write_payload(tmpdir, "wine-display-defaults.reg",
                                    desktop_defaults(app_defaults(display_defaults(user_reg))))
        mcopy(image, display_reg, "::/WINEGATE/prefix/user.reg")
        mcopy(image, write_payload(tmpdir, "wine-system-defaults.reg", system_reg),
              "::/WINEGATE/prefix/system.reg")
        mcopy(image, write_payload(tmpdir, "wine-theme-ready", b""),
              "::" + theme_marker)
        mcopy_tree(image, wine_root / "WINEGATE/prefix/drive_c",
                   "::/SYSTEM/WINE")
        retro_icons = ROOT_DIR / "build/wine/BLESICONS.DLL"
        if not retro_icons.is_file():
            raise FileNotFoundError(f"Wine classic icons require {retro_icons}")
        mcopy(image, retro_icons,
              "::/SYSTEM/WINE/WINDOWS/SYSTEM32/BLESICONS.DLL")
        # The installed prefix disables automatic wineboot updates. New builtin
        # drivers still need their PE stub in the Windows DLL search directory.
        mcopy(image, ROOT_DIR / "ports/wine/package/wine-11.0-i386/rootfs/opt/wine/lib/wine/i386-windows/wineoss.drv",
              "::/SYSTEM/WINE/WINDOWS/SYSTEM32/WINEOSS.DRV")
        # Keep the historical extensionless path for shell/tests, and install
        # an .ELF alias for WINE.BEX.  The public bk_app_launch() API routes
        # native executables by their stable file identity/extension instead
        # of exposing the kernel's private elf_execute_program_ex symbol.
        mcopy(image, launcher, "::/WINEGATE/BIN/wine-launch")
        mcopy(image, launcher, "::/WINEGATE/BIN/wine-launch.ELF")
        mcopy(image, ROOT_DIR / "build/wine/wine-ready.exe", "::/WINEGATE/BIN/wine-ready.exe")
        mcopy(image, tool_launcher, "::/WINEGATE/BIN/wine-tool.ELF")
        mmd(image, "::/WINEGATE/CACHE")
        mmd(image, "::/WINEGATE/CACHE/FONTCONFIG")
        mcopy_tree(image,
                   ROOT_DIR / "ports/wine/package/wine-11.0-i386/licenses",
                   "::/DOCS/WINE")
        mcopy(image,
              ROOT_DIR / "ports/tinyx/upstream/tinycorelinux-tinyx/COPYING",
              "::/DOCS/WINE/TINYX-COPYING.TXT")

    # Keep the native 3D demos in normal ATA/USB/VMware builds for now.
    if BFS_STAGING is not None:
        stage_native_graphics_payloads(BFS_STAGING, graphics_files)
    else:
        existing_dirs = {Path('/'), Path('/SYSTEM'), Path('/SYSTEM/LIBS'),
                         Path('/SYSTEM/PROGRAMS'), Path('/SYSTEM/SDK'), Path('/SYSTEM/DOCS'), Path('/SYSTEM/SCREENS')}
        directories = {parent for name in graphics_files for parent in Path(name).parents}
        for directory in sorted(directories - existing_dirs, key=lambda p: (len(p.parts), str(p))):
            mmd(image, '::' + str(directory))
        for guest, source in graphics_files.items():
            mcopy(image, source, '::' + guest)

    if bfs_build:
        bfs4.build(image, BFS_STAGING, image_size_kb // 1024, "BLESKERNOS")
        bfs4.check(image)
        BFS_STAGING = None
        stage.cleanup()
    print(f"[ATA] {'BFS4' if bfs_build else 'FAT32'} image: {image} "
          f"({image_size_kb // 1024} MiB)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
