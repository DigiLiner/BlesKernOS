#!/usr/bin/env python3
"""Build the shared Gallium runtime and native glmark2 (no Wine/X server).

Uses the OS-supported ELF32/POSIX loader, not the freestanding LBS loader.
Mesa source/version/hash and frontend fixes are shared with the existing port.
"""
from pathlib import Path
import argparse, hashlib, json, re, shutil, subprocess, sys
from jelly_aquarium import patch_jelly_scene
ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/native-gallium'
RUNTIME=WORK/'package/SYSTEM/LIBS/GALLIUM'
COMMIT='42e3d8fe3aa88743ef90348138f643f7b04a9237'
def run(args,**kw): subprocess.run([str(x) for x in args],check=True,**kw)
def upstream(source,name):
    return subprocess.check_output(['git','show','HEAD:'+name],cwd=source,text=True)
def package_closure(binary):
    pending=[binary];seen=set()
    while pending:
        p=pending.pop()
        if p.name in seen:continue
        seen.add(p.name)
        for dep in re.findall(r'\(NEEDED\).*?\[(.*?)\]',subprocess.check_output(['readelf','-d',str(p)],text=True)):
            dst=RUNTIME/dep
            if not dst.exists():
                src=Path('/usr/lib/i386-linux-gnu')/dep
                if not src.exists():raise RuntimeError(f'Missing i386 dependency {dep}')
                shutil.copyfile(src,dst)
            pending.append(dst)
    return sorted(seen)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--skip-mesa',action='store_true')
    p.add_argument('-j',type=int,default=3)
    a=p.parse_args();WORK.mkdir(parents=True,exist_ok=True);RUNTIME.mkdir(parents=True,exist_ok=True)
    if not a.skip_mesa:
        run([sys.executable,ROOT/'ports/wine/graphics/build-gallium.py',
             '--build-dir','build-native','--runtime-path','/SYSTEM/LIBS/GALLIUM','--output',RUNTIME/'libbkvirgl.so.8','-j',a.j])
    if not (RUNTIME/'libbkvirgl.so.8').exists():raise RuntimeError('Build Gallium before --skip-mesa')
    run(['gcc','-m32','-O2','-fPIC','-shared','-Wl,-z,defs','-Wl,-soname,libbkgfx.so.1',
        '-Wl,-rpath,/SYSTEM/LIBS/GALLIUM',ROOT/'ports/gallium/client.c',
        '-ldl','-pthread','-o',RUNTIME/'libbkgfx.so.1'])
    source=WORK/'glmark2'
    if not source.exists():run(['git','clone','--depth','1','--branch','2023.01','https://github.com/glmark2/glmark2.git',source])
    actual=subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip()
    if actual!=COMMIT:raise RuntimeError(f'Unexpected glmark2 revision {actual}')
    # All modifications are reapplied to the pinned build copy; the upstream
    # benchmark/scenes remain unchanged. Only window/context plumbing changes.
    text=upstream(source,'src/main.cpp')
    text=text.replace('main(int argc, char *argv[])\n{','main(int argc, char *argv[])\n{\n#if GLMARK2_USE_BLES\n    bkg_expand_arguments(argc, argv);\n    bkg_configure_trace(argc, argv);\n    bkg_configure_screensaver(argc, argv);\n#endif\n    setvbuf(stdout, NULL, _IONBF, 0);\n    setvbuf(stderr, NULL, _IONBF, 0);',1)
    text=text.replace('#if GLMARK2_USE_X11\n#include', '#if GLMARK2_USE_BLES\n#include "glmark2-bles.h"\n#elif GLMARK2_USE_X11\n#include',1)
    text=text.replace('#if GLMARK2_USE_X11\n    NativeStateX11', '#if GLMARK2_USE_BLES\n    NativeStateBles native_state;\n#elif GLMARK2_USE_X11\n    NativeStateX11',1)
    text=text.replace('#if GLMARK2_USE_EGL\n    GLStateEGL', '#if GLMARK2_USE_BLES\n    GLStateBles gl_state;\n#elif GLMARK2_USE_EGL\n    GLStateEGL',1)
    text=text.replace('Log::error("%s: Could not initialize canvas\\n", __FUNCTION__);\n        return 1;', 'Log::error("%s: Could not initialize canvas\\n", __FUNCTION__);\n        return bkg_screensaver ? 2 : 1;',1)
    text=text.replace('    canvas.visible(true);','    canvas.visible(true);\n#if GLMARK2_USE_BLES\n    if(bkg_screensaver)return bkg_run_screensaver(canvas);\n#endif',1)
    (source/'src/main.cpp').write_text(text)
    # Patch only the native Bles build of the pinned jellyfish scene.
    # Normal glmark2 uses the exact original transforms (no BKG_JELLY_COUNT).
    (source/'src/scene-jellyfish.cpp').write_text(
        patch_jelly_scene(upstream(source,'src/scene-jellyfish.cpp')))
    (source/'meson_options.txt').write_text(upstream(source,'meson_options.txt').replace("'drm-gl',","'bles-gl',\n           'drm-gl',",1))
    text=upstream(source,'src/meson.build')
    # C++14 is the pinned upstream default; no GLFW/SDL/X11 dependency.
    native=f"""wsi_bles_dep = declare_dependency(
    include_directories: include_directories('{ROOT}/ports/gallium', '{ROOT}/ports/gallium/include'),
    compile_args: ['-DGLMARK2_USE_BLES'],
    link_args: ['-Wl,--disable-new-dtags', '-Wl,-rpath,/SYSTEM/LIBS/GALLIUM',
                '-Wl,--dynamic-linker=/SYSTEM/LIBS/GALLIUM/ld-linux.so.2',
                '-L{RUNTIME}', '-l:libbkgfx.so.1'],
)
"""
    text=text.replace('flavor_info = {',native+"\nflavor_info = {\n    'bles-gl' : ['GLMARK2.ELF', declare_dependency(), gl_gl_dep, wsi_bles_dep],",1)
    (source/'src/meson.build').write_text(text)
    # Runtime packages already contain i386 PNG/JPEG; supply their build
    # metadata without adding host-architecture libraries to the cross link.
    pc=WORK/'pkgconfig';pc.mkdir(exist_ok=True)
    config=WORK/'include';config.mkdir(exist_ok=True)
    # jconfig only contains codec options, no host pointer-size definitions.
    shutil.copyfile('/usr/include/x86_64-linux-gnu/jconfig.h',config/'jconfig.h')
    for name,version,lib in [('libpng','1.6.43','libpng16.so.16'),('libjpeg','2.1.5','libjpeg.so.8')]:
        (pc/(name+'.pc')).write_text(f'Name: {name}\nDescription: i386 runtime\nVersion: {version}\nLibs: -L/usr/lib/i386-linux-gnu -l:{lib}\nCflags: -I"{config}"\n')
    cross=WORK/'cross.ini'
    cross.write_text(f"""[binaries]
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
pkg_config_libdir = ['{pc}']
[built-in options]
c_args = ['-march=pentium3', '-mfpmath=387', '-I{config}']
cpp_args = ['-march=pentium3', '-mfpmath=387', '-I{config}']
""")
    meson=[ROOT/'build/wine/gallium/venv/bin/python','-m','mesonbuild.mesonmain'];build=WORK/'glmark-build'
    if not (build/'build.ninja').exists():
        run([*meson,'setup',build,source,'--cross-file',cross,'--buildtype=release','-Dflavors=bles-gl','-Ddata-path=/SYSTEM/SHARE/GLMARK2'])
    else:
        run([*meson,'setup','--reconfigure','--clearcache',build,source])
    run(['ninja','-C',build,'-j',a.j])
    binary=WORK/'package/SYSTEM/PROGRAMS/GLMARK2.ELF';binary.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(build/'src/GLMARK2.ELF',binary)
    run(['strip','--strip-unneeded',binary])
    run(['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2',
        '-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-fno-pic','-fno-pie',
        '-fno-stack-protector','-I'+str(ROOT),'-I'+str(ROOT/'kernel/libc/include'),
        '-c',ROOT/'ports/gallium/launcher.c','-o',binary.with_name('GLMARK2.BEX')])
    probe=binary.with_name('GALLTEST.ELF')
    run(['gcc','-m32','-O2','-march=pentium3','-mfpmath=387','-no-pie',
        '-Wl,--disable-new-dtags','-Wl,-rpath,/SYSTEM/LIBS/GALLIUM',
        '-Wl,--dynamic-linker=/SYSTEM/LIBS/GALLIUM/ld-linux.so.2',
        ROOT/'ports/gallium/probe.c','-L'+str(RUNTIME),'-l:libbkgfx.so.1','-o',probe])
    run(['gcc','-m32','-O2','-no-pie','-Wl,--disable-new-dtags',
        '-Wl,-rpath,/SYSTEM/LIBS/GALLIUM','-Wl,-rpath-link,/usr/lib/i386-linux-gnu',
        '-Wl,--dynamic-linker=/SYSTEM/LIBS/GALLIUM/ld-linux.so.2',
        ROOT/'ports/wine/graphics/test-gallium.c','-L'+str(RUNTIME),'-l:libbkvirgl.so.8',
        '-o',binary.with_name('GALLCHECK.ELF')])
    shutil.copyfile('/usr/lib/i386-linux-gnu/ld-linux.so.2',RUNTIME/'ld-linux.so.2')
    data=WORK/'package/SYSTEM/SHARE/GLMARK2'
    shutil.copytree(source/'data',data,dirs_exist_ok=True)
    headers=WORK/'package/SYSTEM/SDK/GALLIUM/INCLUDE';headers.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(ROOT/'ports/gallium/include/bleskernos_gallium.h',headers/'bleskernos_gallium.h')
    deps=package_closure(RUNTIME/'libbkvirgl.so.8')+package_closure(binary)
    licenses=WORK/'package/SYSTEM/SHARE/LICENSES/GALLIUM';licenses.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(source/'COPYING',licenses/'GLMARK2-COPYING')
    shutil.copyfile(source/'COPYING.SGI',licenses/'GLMARK2-COPYING.SGI')
    mesa_license=RUNTIME/'libbkvirgl.so.LICENSE'
    if mesa_license.exists():shutil.copyfile(mesa_license,licenses/'MESA-LICENSES')
    for name in ('libc6','libstdc++6','libgcc-s1','libexpat1','zlib1g','libpng16-16t64','libjpeg-turbo8'):
        f=Path('/usr/share/doc')/name/'copyright'
        if f.exists():shutil.copyfile(f,licenses/(name+'-COPYRIGHT'))
    for name in ('GPL-3','LGPL-3','LGPL-2.1','GPL-2'):
        shutil.copyfile(Path('/usr/share/common-licenses')/name,licenses/name)
    manifest_path=WORK/'manifest.json'
    previous=json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    manifest={'glmark2_commit':COMMIT,'dependencies':sorted(set(deps)|set(previous.get('dependencies',[]))),
        'abi':'ELF32/POSIX; no Wine or X11','files':{str(f.relative_to(WORK/'package')):hashlib.sha256(f.read_bytes()).hexdigest()
        for f in sorted((WORK/'package').rglob('*')) if f.is_file()}}
    # This package is also extended by the Mesa demos builder. Retain its
    # validation requirement when rebuilding only the modern runtime.
    if previous.get('mesa_demos'):
        manifest['mesa_demos']=previous['mesa_demos']
    manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
    print('Native Gallium/glmark2 package:',WORK/'package',flush=True)
if __name__=='__main__':main()
