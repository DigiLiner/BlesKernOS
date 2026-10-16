#!/usr/bin/env python3
"""Compila pruebas contra glibc i386 del host; registra binarios y toolchain.
No utiliza la libc/ld de tests/userland ni modifica la imagen normal.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
def run(args):
    return subprocess.check_output([str(a) for a in args], text=True, stderr=subprocess.STDOUT)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,default=ROOT/'ports/wine/rootfs')
    p.add_argument('--cc',default='gcc')
    args=p.parse_args();out=args.output.resolve();guest=out/'WINEGATE'
    (guest/'bin').mkdir(parents=True,exist_ok=True);(guest/'lib').mkdir(exist_ok=True)
    cc=[args.cc,'-m32']
    # glibc loads libgcc_s lazily for pthread_exit; Wine loads FreeType
    # lazily for GDI, so neither appears in the launcher's ELF dependencies.
    for name in ['libc.so.6','ld-linux.so.2','libgcc_s.so.1','libfreetype.so.6',
                 'libz.so.1','libbz2.so.1.0','libpng16.so.16',
                 'libbrotlidec.so.1','libbrotlicommon.so.1',
                 'libfontconfig.so.1','libexpat.so.1']:
        src=Path(run(cc+[f'-print-file-name={name}']).strip()).resolve()
        if not src.is_file(): raise RuntimeError(f'falta runtime i386: {name}')
        shutil.copyfile(src,guest/'lib'/name)
    fonts=guest/'etc/fonts';fonts.mkdir(parents=True,exist_ok=True)
    (fonts/'fonts.conf').write_text('''<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>/WINEGATE/share/fonts</dir>
  <cachedir>/WINEGATE/cache/fontconfig</cachedir>
</fontconfig>
''')
    (guest/'cache/fontconfig').mkdir(parents=True,exist_ok=True)
    # Wine enumerates its own data/fonts directory separately. Fontconfig
    # indexes host fonts here, avoiding a second scan of every builtin font.
    (guest/'share/fonts').mkdir(parents=True,exist_ok=True)
    commands=[]
    command=cc+['-nostdlib','-no-pie',str(ROOT/'ports/wine/tests/abi.S'),'-o',str(guest/'bin/abi')]
    commands.append(command);run(command)
    for case in range(1,34):
        test_source={19:'signals.c',20:'futex.c',21:'fork.c',22:'exec.c',23:'ipc.c',24:'unix.c',25:'filesystem.c',26:'robust.c',27:'dlopen.c',28:'shared.c',29:'locking.c',30:'realpath.c',31:'private-cache.c',32:'futex-mapping.c',33:'madvise.c'}.get(case,'runtime.c')
        command=cc+['-O2','-g','-no-pie','-pthread',f'-DCASE={case}',
            '-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2',
            '-Wl,--disable-new-dtags,-rpath,/WINEGATE/lib',str(ROOT/'ports/wine/tests'/test_source),
            '-ldl','-o',str(guest/'bin'/f'case{case:02d}')]
        commands.append(command);run(command)
    command=cc+['-shared','-fPIC',str(ROOT/'ports/wine/tests/library.c'),'-o',str(guest/'lib/libgate.so')]
    commands.append(command);run(command)
    files={str(f.relative_to(out)):hashlib.sha256(f.read_bytes()).hexdigest()
        for f in sorted(guest.rglob('*')) if f.is_file()}
    for f in guest.rglob('*'):
        if f.is_file() and f.parent.name in ('bin','lib'):
            header=run(['readelf','-h',f])
            if 'ELF32' not in header or 'Intel 80386' not in header: raise RuntimeError(f'no es i386: {f}')
    manifest={'compiler':run([args.cc,'--version']).splitlines()[0],
        'libc':run(['getconf','GNU_LIBC_VERSION']).strip(),
        'commands':commands,'sha256':files,'guest_result':'NOT YET TESTED'}
    (out/'runtime-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(f'Runtime glibc real construido: {out}')
if __name__=='__main__': main()



