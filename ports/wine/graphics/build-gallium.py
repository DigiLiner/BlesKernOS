#!/usr/bin/env python3
"""Build pinned Mesa with the BlesKernOS GPU-only VirGL winsys.

Requires gcc/g++ multilib, ninja, bison, flex, pkg-config, Python venv.
Outputs the programmable VirGL backend alongside the classic Mesa backend.
"""
import argparse
import hashlib
import json
import shutil
import re
import subprocess
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
VERSION = '24.3.4'
SHA256 = 'e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025'
OPTIONS = ['--buildtype=release', '-Dgallium-drivers=virgl', '-Dvulkan-drivers=',
    '-Dplatforms=', '-Dosmesa=true', '-Dglx=disabled', '-Degl=disabled',
    '-Dgbm=disabled', '-Dgles1=disabled', '-Dgles2=disabled', '-Dllvm=disabled',
    '-Dshared-glapi=disabled', '-Dgallium-vdpau=disabled', '-Dgallium-va=disabled',
    '-Dgallium-xa=disabled', '-Dvideo-codecs=', '-Dbuild-tests=false',
    '-Dlmsensors=disabled', '-Dlibunwind=disabled', '-Dvalgrind=disabled']

def validate_runtime_closure(backend, lib):
    """Check the shipped i386 files, without resolving anything from the host."""
    pending = [Path(backend)]
    visited = set()
    while pending:
        path = pending.pop()
        if path.name in visited:
            continue
        visited.add(path.name)
        with path.open('rb') as stream:
            header = stream.read(20)
        if header[:6] != b'\x7fELF\x01\x01' or header[18:20] != b'\x03\x00':
            raise RuntimeError(f'Gaming runtime is not ELF i386: {path}')
        dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True)
        for name in re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic):
            dependency = lib / name
            if not dependency.is_file():
                raise RuntimeError(f'Missing packaged GPU dependency: {path.name} -> {name}')
            pending.append(dependency)
    return sorted(visited)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cache', type=Path, default=(Path.home()/'.cache/bleskernos-gallium' if str(ROOT).startswith('/mnt/') else ROOT/'build/wine/gallium'))
    p.add_argument('-j', type=int, default=2)
    p.add_argument('--package', action='store_true', help='install the GPU backend and runtime dependencies into the Wine package')
    p.add_argument('--runtime-path', default='/WINEGATE/lib', help='runtime library search path')
    p.add_argument('--output', type=Path, help='output library')
    p.add_argument('--build-dir', default='build', help='build directory within the source cache')
    a = p.parse_args()
    cache = a.cache.resolve(); cache.mkdir(parents=True, exist_ok=True)
    archive = cache/'mesa.tar.xz'
    if not archive.exists():
        temp = archive.with_suffix('.download')
        urllib.request.urlretrieve(f'https://archive.mesa3d.org/mesa-{VERSION}.tar.xz', temp)
        temp.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit('Mesa source checksum mismatch')
    source = cache/f'mesa-{VERSION}'
    if not source.exists():
        with tarfile.open(archive) as tar: tar.extractall(cache, filter='data')
    target = source/'src/gallium/targets/osmesa'
    (target/'osmesa_target.c').write_text('''/* SPDX-License-Identifier: MIT */
#include "pipe/p_screen.h"
struct pipe_screen *bles_virgl_create_screen(void);
struct pipe_screen *osmesa_create_screen(void);
struct pipe_screen *osmesa_create_screen(void) { return bles_virgl_create_screen(); }
''')
    shutil.copyfile(ROOT/'ports/gallium/virgl-winsys.c', target/'bles_virgl.c')
    shutil.copyfile(ROOT/'ports/gallium/include/blesgfx-native.h', target/'blesgfx-native.h')
    shutil.copyfile(ROOT/'ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesvirgl.h', target/'blesvirgl.h')
    shutil.copyfile(ROOT/'ports/tinyx/upstream/tinycorelinux-tinyx/kdrive/fbdev/blesgfx.h', target/'blesgfx.h')
    meson = target/'meson.build'
    text = meson.read_text().replace("  'osmesa_target.c',", "  ['osmesa_target.c', 'bles_virgl.c'],")
    text = text.replace('dep_unwind, driver_swrast,', 'dep_unwind, driver_virgl,')
    text = text.replace('driver_virgl, idep_mesautil,', 'driver_virgl, idep_mesautil, idep_nir_headers,')
    text = text.replace('    inc_gallium_drivers,\n', '    inc_gallium_drivers, inc_virtio,\n')
    if "include_directories('../../../compiler')" not in text:
        text=text.replace('inc_gallium_drivers, inc_virtio,',"inc_gallium_drivers, inc_virtio, include_directories('../../../compiler'),")
    if 'builtin_types_h,' not in text:
        text=text.replace("['osmesa_target.c', 'bles_virgl.c'],", "['osmesa_target.c', 'bles_virgl.c'], builtin_types_h,")
    text = text.replace('libmesa, libgallium, libws_null,', 'libmesa, libgallium, libgalliumvl, libws_null,')
    if 'build_rpath' not in text:
        text = text.replace('  install : true,', "  install : true,\n  build_rpath : '/WINEGATE/lib',\n  install_rpath : '/WINEGATE/lib',")
    text=re.sub(r"(build_rpath|install_rpath) : '[^']*'", lambda m: m[1]+" : '"+a.runtime_path+"'",text)
    meson.write_text(text)
    # Apply the frontend extension to the verified upstream file each time.
    frontend = source/'src/gallium/frontends/osmesa/osmesa.c'
    with tarfile.open(archive) as tar:
        frontend.write_bytes(tar.extractfile(f'mesa-{VERSION}/src/gallium/frontends/osmesa/osmesa.c').read())
    subprocess.run(['patch', '--batch', '-p1', '-i', str(ROOT/'ports/wine/graphics/mesa-blesk-frontend.patch')], cwd=source, check=True)
    native=ROOT/'ports/gallium/osmesa-native.inc'
    if native.exists():
        text=frontend.read_text().replace('   bool external_buffer;', '   bool external_buffer;\n   bool native_present;')
        text=text.replace('   if (osmesa->pp) {', '   if (osmesa->native_present) return true;\n\n   if (osmesa->pp) {',1)
        frontend.write_text(text+'\n'+native.read_text())
    with tarfile.open(archive) as tar:
        for name in ['virgl_winsys.h', 'virgl_resource.c']:
            relative = f'src/gallium/drivers/virgl/{name}'
            (source/relative).write_bytes(tar.extractfile(f'mesa-{VERSION}/{relative}').read())
    subprocess.run(['patch', '--batch', '-p1', '-i', str(ROOT/'ports/wine/graphics/mesa-blesk-staging.patch')], cwd=source, check=True)
    symbols = target/'osmesa.sym'
    text = symbols.read_text()
    for symbol in ['OSMesaBleskCreateContextAttribs', 'OSMesaBleskSwapBuffers', 'OSMesaBleskCreateDrawable', 'OSMesaBleskDestroyDrawable', 'OSMesaBleskMakeContextCurrent', 'OSMesaBleskPresentNative', 'OSMesaBleskNativeMode', 'OSMesaBleskNativeEvent', 'OSMesaBleskNativeReady', 'OSMesaBleskNativePoll']:
        if symbol+';' not in text:
            text = text.replace('OSMesaCreateContextAttribs;', 'OSMesaCreateContextAttribs;\n\t\t'+symbol+';')
    symbols.write_text(text)
    # The only winsys is /dev/blesgfx; omit Linux DRM and vtest transports.
    driver = source/'src/gallium/drivers/virgl/meson.build'
    text = driver.read_text()
    text = text.replace('if not with_platform_windows\n  virgl_deps += libvirgldrm\n  virgl_deps += libvirglvtest\nendif\n', '')
    driver.write_text(text)
    # Static i386 GL dispatch: the assembly generator otherwise omits the
    # TLS helper while its C caller still references it. Use matching real
    # initial-exec TLS stubs, with read-only text (no runtime code rewriting).
    api = source/'src/mapi/glapi/meson.build'
    text = api.read_text().replace('static_glapi_args = []', "static_glapi_args = ['-DGLX_X86_READONLY_TEXT']")
    api.write_text(text)
    generator = source/'src/mapi/glapi/gen/gl_x86_asm.py'
    text = generator.read_text()
    if "print('#define REALLY_INITIAL_EXEC')" not in text:
        text = text.replace("        print('#include \"x86/assyntax.h\"')", "        print('#define REALLY_INITIAL_EXEC')\n        print('#include \"x86/assyntax.h\"')")
    generator.write_text(text)
    meson = source/'meson.build'
    text = meson.read_text()
    start = text.find('  if not with_gallium_swrast', text.index('if with_osmesa'))
    if start >= 0:
        end = text.index('  endif', start) + len('  endif')
        text = text[:start] + "  if not with_gallium_virgl\n    error('BlesKernOS OSMesa requires VirGL.')\n  endif" + text[end:]
    meson.write_text(text.replace("osmesa_lib_name = 'OSMesa'", "osmesa_lib_name = 'bkvirgl'"))
    venv = cache/'venv'
    if not (venv/'bin/meson').exists():
        subprocess.run(['python3', '-m', 'venv', str(venv)], check=True)
        subprocess.run([str(venv/'bin/pip'), 'install', 'meson==1.12.1', 'mako==1.4.3', 'MarkupSafe==3.0.4', 'pyyaml==6.0.3', 'packaging==26.3'], check=True)
    cross = cache/'cross.ini'
    cross.write_text('''[binaries]
c = ['gcc', '-m32']
cpp = ['g++', '-m32']
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'x86'
cpu = 'pentium3'
endian = 'little'
[properties]
needs_exe_wrapper = false
pkg_config_libdir = ['/usr/lib/i386-linux-gnu/pkgconfig', '/usr/share/pkgconfig']
[built-in options]
c_args = ['-march=pentium3', '-mfpmath=387']
cpp_args = ['-march=pentium3', '-mfpmath=387']
''')
    meson_command=[str(venv/'bin/python'),'-m','mesonbuild.mesonmain']
    build = cache/a.build_dir
    cached_sources=build/'meson-info/intro-buildsystem_files.json'
    if cached_sources.exists():
        original=json.loads(cached_sources.read_text())[0]
        if original!=str(source/'meson.build'):
            # Meson build trees cannot be relocated. Preserve the old cache;
            # reuse the correctly located shared build instead of executing
            # stale absolute paths or destroying the previous artifacts.
            build=cache/'build-native'
    if not (build/'build.ninja').exists():
        subprocess.run([*meson_command, 'setup', str(build), str(source), '--cross-file', str(cross), *OPTIONS], check=True)
    subprocess.run(['ninja', '-C', str(build), f'-j{a.j}', 'src/gallium/targets/osmesa/libbkvirgl.so.8.0.0'], check=True)
    staging = cache/'install'
    subprocess.run([*meson_command, 'install', '-C', str(build), '--no-rebuild',
                    '--skip-subprojects', '--tags', 'runtime', '--destdir', str(staging)], check=True)
    installed = list(staging.rglob('libbkvirgl.so.8.0.0'))
    if len(installed) != 1:
        raise SystemExit('Expected exactly one installed VirGL library')
    out = a.output.resolve() if a.output else ROOT/'build/wine/graphics/libbkvirgl.so.8'
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(installed[0], out)
    subprocess.run(['strip', '--strip-unneeded', str(out)], check=True)
    notices = [f'Mesa {VERSION} — source: https://archive.mesa3d.org/mesa-{VERSION}.tar.xz\nSHA-256: {SHA256}\n',
               (source/'docs/license.rst').read_text()]
    for license_file in sorted((source/'licenses').rglob('*')):
        if license_file.is_file():
            notices.append(str(license_file.relative_to(source))+'\n'+license_file.read_text(errors='replace'))
    # Retain complete copyright/license comments, including author names,
    # rather than distributing only the license summary/SPDX identifiers.
    for path in sorted((source/'src').rglob('*')):
        if path.suffix not in ('.c','.h','.cpp','.cc','.S','.in','.py') or not path.is_file(): continue
        text = path.read_text(errors='replace')
        blocks = [m.group(0) for m in re.finditer(r'/\*.*?\*/',text,re.S)
                  if re.search(r'copyright|SPDX-License-Identifier',m.group(0),re.I)]
        if blocks: notices.append(str(path.relative_to(source))+'\n'+'\n'.join(blocks))
    out.with_suffix('.LICENSE').write_text('\n\n'.join(notices))
    if a.package:
        package = ROOT/'ports/wine/package/wine-11.0-i386'
        lib = package/'rootfs/WINEGATE/lib'
        licenses = package/'licenses'
        lib.mkdir(parents=True, exist_ok=True); licenses.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(out, lib/out.name)
        shutil.copyfile(out.with_suffix('.LICENSE'), licenses/'MESA-GALLIUM-LICENSE.rst')
        for name, doc in [('libstdc++.so.6','libstdc++6'), ('libgcc_s.so.1','libgcc-s1'),
                          ('libexpat.so.1','libexpat1'), ('libz.so.1','zlib1g')]:
            shutil.copyfile(Path('/usr/lib/i386-linux-gnu')/name, lib/name)
            shutil.copyfile(Path('/usr/share/doc')/doc/'copyright', licenses/(doc+'-COPYRIGHT'))
        for name in ['GPL-3', 'LGPL-3', 'LGPL-2.1', 'GPL-2']:
            shutil.copyfile(Path('/usr/share/common-licenses')/name, licenses/('GALLIUM-RUNTIME-'+name))
        validate_runtime_closure(lib/out.name, lib)
    print(out)

if __name__ == '__main__': main()
