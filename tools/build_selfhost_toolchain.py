#!/usr/bin/env python3
"""Build the pinned self-hosting toolchain shipped in every OS image."""

import argparse
import hashlib
import os
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
from pathlib import Path


TCC_COMMIT = "2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
TCC_URL = f"https://github.com/TinyCC/tinycc/archive/{TCC_COMMIT}.tar.gz"
TCC_SHA256 = "4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
NASM_VERSION = "3.02"
NASM_URL = ("https://www.nasm.us/pub/nasm/releasebuilds/"
            f"{NASM_VERSION}/nasm-{NASM_VERSION}.tar.xz")
NASM_SHA256 = "87336eba53b4acfe917424ab5d500d2b0054d9f5148d35c2273ccf2cfb712f0d"
MAKE_VERSION = "4.4.1"
MAKE_URL = f"https://ftp.gnu.org/gnu/make/make-{MAKE_VERSION}.tar.gz"
MAKE_SHA256 = "dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3"
BINUTILS_VERSION = "2.47"
BINUTILS_URL = ("https://sourceware.org/pub/binutils/releases/"
                f"binutils-{BINUTILS_VERSION}.tar.xz")
BINUTILS_SHA512 = (
    "3126a1064374d8da40d4d70630c204ed1e75d542c447d53fca9778c7ceff095c"
    "28e9b445e15a313fef9729082d7966471ee6b5b715d479aa6d568528743e1d98"
)
TCC_PREFIX = "/SYSTEM/DEVEL/TCC"
NASM_PREFIX = "/SYSTEM/DEVEL/NASM"
MAKE_PREFIX = "/SYSTEM/DEVEL/MAKE"


def run(command, cwd=None):
    print("[SELFHOST]", " ".join(str(part) for part in command))
    subprocess.check_call([str(part) for part in command], cwd=cwd)


def copy_tree(source, destination):
    destination.mkdir(parents=True, exist_ok=True)
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        target = destination / relative
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif path.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


def fetch_extract(url, archive, temporary, source_name, expected_digest,
                  algorithm="sha256"):
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.is_file():
        urllib.request.urlretrieve(url, archive)
    digest = hashlib.new(algorithm, archive.read_bytes()).hexdigest()
    print(f"[SELFHOST] {source_name} {algorithm}={digest}")
    if digest != expected_digest:
        raise RuntimeError(
            f"checksum incorrecto para {source_name}: {digest}"
        )
    with tarfile.open(archive, "r:*") as bundle:
        bundle.extractall(temporary, filter="data")


def patch_for_bleskernos(source):
    """Avoid fdopen: the BlesKernOS CRT has opaque Win32 and native handles."""
    path = source / "tccelf.c"
    text = path.read_text(encoding="utf-8")
    old = '''    unlink(filename);
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, mode);
    if (fd < 0 || (f = fdopen(fd, "wb")) == NULL)
        return tcc_error_noabort("could not write '%s: %s'", filename, strerror(errno));'''
    new = '''    unlink(filename);
#ifdef _WIN32
    (void)fd;
    (void)mode;
    f = fopen(filename, "wb");
    if (f == NULL)
#else
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, mode);
    if (fd < 0 || (f = fdopen(fd, "wb")) == NULL)
#endif
        return tcc_error_noabort("could not write '%s: %s'", filename, strerror(errno));'''
    if old not in text:
        raise RuntimeError("el bloque fdopen esperado cambio en TinyCC")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def patch_make_for_bleskernos(source):
    """Use inherited console handles; BlesKernOS has no anonymous pipes yet."""
    path = source / "src" / "w32" / "subproc" / "sub_proc.c"
    text = path.read_text(encoding="utf-8")
    start = text.index("        HANDLE\nprocess_init()")
    end = text.index("        HANDLE\nprocess_init_fd", start)
    replacement = '''        HANDLE
process_init()
{
        sub_process *pproc;
        pproc = malloc(sizeof(*pproc));
        if (!pproc)
                return INVALID_HANDLE_VALUE;
        memset(pproc, 0, sizeof(*pproc));
        pproc->sv_stdin[1]  = (intptr_t)GetStdHandle(STD_INPUT_HANDLE);
        pproc->sv_stdout[1] = (intptr_t)GetStdHandle(STD_OUTPUT_HANDLE);
        pproc->sv_stderr[1] = (intptr_t)GetStdHandle(STD_ERROR_HANDLE);
        pproc->using_pipes = 0;
        pproc->lerrno = 0;
        return((HANDLE)pproc);
}


'''
    text = text[:start] + replacement + text[end:]
    text = text.replace('{ ".exe", ".cmd", ".bat", "", ".com", NULL }',
                        '{ ".exe", ".bex", ".cmd", ".bat", "", ".com", NULL }',
                        1)
    path.write_text(text, encoding="utf-8")

    # Native Win32 GNU Make assumes that sh.exe may be absent and starts in
    # Windows batch mode. That makes even a simple recipe go through a
    # C:\\TEMP\\make*.bat file, which BlesKernOS deliberately does not execute
    # as a PE/BEX program. BlesKernOS already exposes sh.exe through
    # KERNEL32/CreateProcessA -> /SYSTEM/COMMANDS/SH.BEX, so tell Make that the
    # Bourne-compatible shell is available and run recipes as `sh.exe -c ...`
    # instead of manufacturing .bat files.
    job_path = source / "src" / "job.c"
    job_text = job_path.read_text(encoding="utf-8")
    old_shell_mode = ("const char *default_shell = \"sh.exe\";\n"
                      "int no_default_sh_exe = 1;\n"
                      "int batch_mode_shell = 1;")
    new_shell_mode = ("const char *default_shell = \"sh.exe\";\n"
                      "int no_default_sh_exe = 0;\n"
                      "int batch_mode_shell = 0;")
    if old_shell_mode not in job_text:
        raise RuntimeError("el modo shell esperado cambio en GNU Make")
    job_path.write_text(job_text.replace(old_shell_mode, new_shell_mode, 1),
                        encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="build/selfhost/toolchain")
    parser.add_argument("--stamp", default="build/selfhost/.stamp")
    parser.add_argument("--cache", default="build/selfhost/downloads")
    args = parser.parse_args()
    output = Path(args.output).resolve()
    stamp = Path(args.stamp).resolve()
    cache = Path(args.cache).resolve()

    required = ("i686-w64-mingw32-gcc", "i686-w64-mingw32-ar",
                "i686-w64-mingw32-windres", "wine", "make")
    missing = [name for name in required if shutil.which(name) is None]
    if missing:
        raise SystemExit("faltan herramientas para TinyCC: " + ", ".join(missing))

    with tempfile.TemporaryDirectory(prefix="bles-selfhost-") as temporary:
        temporary = Path(temporary)
        archive = cache / f"tinycc-{TCC_COMMIT}.tar.gz"
        fetch_extract(TCC_URL, archive, temporary, f"TinyCC {TCC_COMMIT}",
                      TCC_SHA256)
        source = temporary / f"tinycc-{TCC_COMMIT}"
        patch_for_bleskernos(source)
        build = temporary / "build"
        build.mkdir()

        run([
            source / "configure", "--cross-prefix=i686-w64-mingw32-",
            "--cpu=i386", "--targetos=WIN32", "--enable-static",
            f"--prefix={TCC_PREFIX}",
        ], build)
        run([
            "i686-w64-mingw32-gcc", "-DC2STR", source / "conftest.c",
            "-o", build / "c2str.exe",
        ])
        run([
            "wine", build / "c2str.exe", source / "include/tccdefs.h",
            build / "tccdefs_.h",
        ])
        run(["make", "-j2", "tcc.exe", "i386-tcc.exe"], build)
        run(["make", "-C", "lib", "XTCC=wine ../tcc.exe"], build)

        staged = temporary / "stage"
        tcc_stage = staged / "TCC"
        (tcc_stage / "include").mkdir(parents=True)
        (tcc_stage / "lib").mkdir(parents=True)
        shutil.copy2(build / "tcc.exe", tcc_stage / "TCC.EXE")
        shutil.copy2(build / "i386-tcc.exe", tcc_stage / "I386-TCC.EXE")
        shutil.copy2(build / "libtcc1.a", tcc_stage / "lib/LIBTCC1.A")
        shutil.copy2(source / "COPYING", tcc_stage / "COPYING.TXT")
        shutil.copy2(source / "VERSION", tcc_stage / "VERSION.TXT")
        copy_tree(source / "include", tcc_stage / "include")
        copy_tree(source / "win32/include", tcc_stage / "include")
        for definition in sorted((source / "win32/lib").glob("*.def")):
            shutil.copy2(definition, tcc_stage / "lib" / definition.name.upper())

        nasm_archive = cache / f"nasm-{NASM_VERSION}.tar.xz"
        fetch_extract(NASM_URL, nasm_archive, temporary,
                      f"NASM {NASM_VERSION}", NASM_SHA256)
        nasm_source = temporary / f"nasm-{NASM_VERSION}"
        # NASM's Windows resource rule still addresses win/manifest.rc
        # relative to the build directory, so build this disposable source
        # tree in place instead of using an out-of-tree directory.
        nasm_build = nasm_source
        run([
            nasm_source / "configure", "--host=i686-w64-mingw32",
            "--target=i686-w64-mingw32", f"--prefix={NASM_PREFIX}",
            "CFLAGS=-Os -g0", "LDFLAGS=-s",
        ], nasm_build)
        run(["make", "-j2", "nasm.exe", "ndisasm.exe"], nasm_build)
        nasm_stage = staged / "NASM"
        nasm_stage.mkdir(parents=True)
        shutil.copy2(nasm_build / "nasm.exe", nasm_stage / "NASM.EXE")
        shutil.copy2(nasm_build / "ndisasm.exe", nasm_stage / "NDISASM.EXE")
        shutil.copy2(nasm_source / "LICENSE", nasm_stage / "LICENSE.TXT")
        (nasm_stage / "VERSION.TXT").write_text(
            NASM_VERSION + "\n", encoding="ascii")

        make_archive = cache / f"make-{MAKE_VERSION}.tar.gz"
        fetch_extract(MAKE_URL, make_archive, temporary,
                      f"GNU Make {MAKE_VERSION}", MAKE_SHA256)
        make_source = temporary / f"make-{MAKE_VERSION}"
        patch_make_for_bleskernos(make_source)
        make_build = temporary / "make-build"
        make_build.mkdir()
        run([
            make_source / "configure", "--host=i686-w64-mingw32",
            f"--prefix={MAKE_PREFIX}", "--disable-nls",
            # The source disk is FAT and BlesKernOS resolves paths
            # case-insensitively.  GNU Make intentionally keeps filename
            # comparisons case-sensitive on Win32 unless this switch is
            # enabled.  build_source_image.py stores FAT names in uppercase,
            # so implicit/static-pattern prerequisite discovery otherwise
            # stops at e.g. kernel/arch/x86/isr_stubs.asm after entry.o was built.
            "--enable-case-insensitive-file-system",
            "ac_cv_func_gettimeofday=no", "CFLAGS=-Os -g0",
            "LDFLAGS=-s",
        ], make_build)
        run(["make", "-j2"], make_build)
        make_stage = staged / "MAKE"
        make_stage.mkdir(parents=True)
        shutil.copy2(make_build / "make.exe", make_stage / "MAKE.EXE")
        shutil.copy2(make_source / "COPYING", make_stage / "COPYING.TXT")
        (make_stage / "VERSION.TXT").write_text(
            MAKE_VERSION + "\n", encoding="ascii")

        binutils_archive = cache / f"binutils-{BINUTILS_VERSION}.tar.xz"
        fetch_extract(BINUTILS_URL, binutils_archive, temporary,
                      f"GNU Binutils {BINUTILS_VERSION}", BINUTILS_SHA512,
                      "sha512")
        binutils_source = temporary / f"binutils-{BINUTILS_VERSION}"
        binutils_build = temporary / "binutils-build"
        binutils_build.mkdir()
        run([
            binutils_source / "configure", "--build=x86_64-pc-linux-gnu",
            "--host=i686-w64-mingw32", "--target=i686-elf",
            "--prefix=/SYSTEM/DEVEL/BINUTILS",
            "--with-sysroot=/SYSTEM/SOURCE",
            "--disable-nls", "--disable-werror", "--disable-gdb",
            "--disable-gdbserver", "--disable-gprofng", "--disable-sim",
            "--disable-libdecnumber", "--disable-readline", "--without-zstd",
            "--enable-static", "--disable-shared", "CFLAGS=-Os -g0",
            "LDFLAGS=-s",
        ], binutils_build)
        run(["make", "-j2", "MAKEINFO=true", "all-binutils", "all-gas",
             "all-ld"], binutils_build)
        binutils_stage = staged / "BINUTILS"
        binutils_stage.mkdir(parents=True)
        binutils_programs = {
            "binutils/addr2line.exe": "ADDR2LINE.EXE",
            "binutils/ar.exe": "AR.EXE",
            "binutils/cxxfilt.exe": "CXXFILT.EXE",
            "binutils/elfedit.exe": "ELFEDIT.EXE",
            "binutils/nm-new.exe": "NM.EXE",
            "binutils/objcopy.exe": "OBJCOPY.EXE",
            "binutils/objdump.exe": "OBJDUMP.EXE",
            "binutils/ranlib.exe": "RANLIB.EXE",
            "binutils/readelf.exe": "READELF.EXE",
            "binutils/size.exe": "SIZE.EXE",
            "binutils/strings.exe": "STRINGS.EXE",
            "binutils/strip-new.exe": "STRIP.EXE",
            "gas/as-new.exe": "AS.EXE",
            "ld/ld-new.exe": "LD.EXE",
        }
        for source_name, destination_name in binutils_programs.items():
            shutil.copy2(binutils_build / source_name,
                         binutils_stage / destination_name)
        shutil.copy2(binutils_source / "COPYING3",
                     binutils_stage / "COPYING.TXT")
        (binutils_stage / "VERSION.TXT").write_text(
            BINUTILS_VERSION + "\n", encoding="ascii")

        fatpack_source = Path(__file__).resolve().parent / "selfhost" / "fatpack.c"
        fatpack_stage = staged / "FATPACK"
        fatpack_stage.mkdir(parents=True)
        run([
            "i686-w64-mingw32-gcc", "-std=c11", "-Os", "-s", "-static",
            "-Wall", "-Wextra", fatpack_source,
            "-o", fatpack_stage / "FATPACK.EXE",
        ])
        (fatpack_stage / "README.TXT").write_text(
            "BlesKernOS FAT32 image packer. Run fatpack --help.\n",
            encoding="ascii")

        if output.exists():
            shutil.rmtree(output)
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(staged, output)

    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(
        f"tcc={TCC_COMMIT}\nnasm={NASM_VERSION}\nmake={MAKE_VERSION}\n"
        f"binutils={BINUTILS_VERSION}\n",
        encoding="ascii")
    print(f"[OK] toolchain self-hosting: {output}")


if __name__ == "__main__":
    main()
