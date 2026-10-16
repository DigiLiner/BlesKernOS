#!/usr/bin/env python3
"""Descarga verificada, configure auditado y build i386 separado de Wine 11.0.
El sysroot/toolchain del host queda identificado en el manifiesto; no se afirma
reproducibilidad bit a bit entre hosts distintos. No instala nada en el host.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request
from toolchain32 import prepare

ROOT=Path(__file__).resolve().parents[3]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True,help='directorio nuevo, sin espacios (requisito de Wine)')
    p.add_argument('--archive',type=Path)
    p.add_argument('--configure-only',action='store_true')
    p.add_argument('--ntdll-only',action='store_true',help='reconstruye solo ntdll.so, sin requerir desarrollo X11/FreeType/Fontconfig')
    p.add_argument('--jobs',type=int,default=2)
    p.add_argument('--memory-pic',action='store_true',help='experimental i386 PIC DLL build; requires guest memory/compatibility validation')
    args=p.parse_args();work=args.work.resolve()
    if ' ' in str(work):p.error('Wine requiere una ruta de build sin espacios')
    if work.exists() and any(work.iterdir()):p.error('usar un directorio nuevo')
    config=json.loads((ROOT/'ports/wine/versions/wine-11.0.json').read_text())
    work.mkdir(parents=True,exist_ok=True)
    archive=args.archive.resolve() if args.archive else work/'source.tar.gz'
    if not args.archive:urllib.request.urlretrieve(config['source_url'],archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=config['source_sha256']:
        raise RuntimeError('SHA256 de Wine no coincide')
    with tarfile.open(archive) as tar:tar.extractall(work,filter='data')
    src=work/config['source_directory'];obj=work/'obj';obj.mkdir()
    patches=[]
    selected_patches=list(config.get('patches',[]))
    if args.memory_pic:selected_patches.append('patches/0008-i386-pic-memory.patch')
    for relative in selected_patches:
        patch=ROOT/'ports/wine'/relative
        subprocess.run(['patch','--batch','--forward','-p1','-i',str(patch)],cwd=src,check=True)
        patches.append({'file':relative,'sha256':hashlib.sha256(patch.read_bytes()).hexdigest()})
    help_text=subprocess.check_output(['sh',src/'configure','--help'],text=True)
    (work/'configure-help.txt').write_text(help_text)
    for arg in config['configure_args']:
        option=arg.split('=')[0]
        # --with-PACKAGE es el inverso documentado de --without-PACKAGE.
        if option not in help_text and option.replace('--with-','--without-') not in help_text:
            raise RuntimeError(f'opcion no documentada por este configure: {arg}')
    env=os.environ.copy()
    prepare(work,env)
    env.update(CC='gcc -m32',CXX='g++ -m32',LD='ld -m elf_i386',
        CFLAGS='-O2 -march=pentium3 -mfpmath=387',CXXFLAGS='-O2 -march=pentium3 -mfpmath=387',
        PKG_CONFIG_LIBDIR='/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib32/pkgconfig:/usr/share/pkgconfig')
    oss_headers=work/'oss-headers'
    oss_headers.symlink_to(ROOT/'ports/wine/audio/include',target_is_directory=True)
    env['CPPFLAGS']='-I'+str(oss_headers)+' '+env.get('CPPFLAGS','')
    configure_args=list(config['configure_args'])
    if args.ntdll_only:
        configure_args=[a for a in configure_args if a not in ('--with-x','--with-freetype','--with-fontconfig')]
        configure_args+=['--without-x','--without-freetype','--without-fontconfig']
    command=['sh',str(src/'configure'),*configure_args]
    manifest={'source':config,'configure':command,'environment':{k:env[k] for k in ['CC','CXX','LD','CFLAGS','CXXFLAGS','CPPFLAGS','PKG_CONFIG_LIBDIR']},
        'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
        'guest_result':'NOT YET TESTED','patches':patches,'memory_pic':args.memory_pic,
        'toolchain32':'as --32; ld -m elf_i386; PATH scoped to this build'}
    manifest['environment']['LDFLAGS']=env.get('LDFLAGS','')
    if args.ntdll_only:manifest['build_scope']='ntdll.so only; existing graphics modules retained'
    with (work/'configure.log').open('w') as log:
        result=subprocess.run(command,cwd=obj,env=env,stdout=log,stderr=subprocess.STDOUT)
    manifest['configure_exit']=result.returncode
    (work/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    if result.returncode:raise SystemExit(f'configure fallo; ver {work}/configure.log')
    if args.memory_pic and not args.ntdll_only and '#define SONAME_LIBGL ' not in (obj/'include/config.h').read_text():
        raise SystemExit('PIC build requires the existing OpenGL backend; provide i386 libGL development linking files')
    if args.configure_only:return
    if args.ntdll_only:
        commands=[['make',f'-j{args.jobs}','dlls/ntdll/ntdll.so'],
            ['make','-W','dlls/ntdll/unix/env.o','LDFLAGS=-Wl,-rpath,/WINEGATE/lib','dlls/ntdll/ntdll.so']]
        for command in commands:
            with (work/'build.log').open('a') as log:
                subprocess.run(command,cwd=obj,env=env,check=True,stdout=log,stderr=subprocess.STDOUT)
        library=obj/'dlls/ntdll/ntdll.so'
        manifest.update(build='ntdll module completed',build_commands=commands,
            module_sha256=hashlib.sha256(library.read_bytes()).hexdigest())
        (work/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        print(f'ntdll.so reconstruido: {library}')
        return
    for command in [['make',f'-j{args.jobs}'],['make',f'DESTDIR={work / "stage"}','install']]:
        with (work/'build.log').open('a') as log:
            subprocess.run(command,cwd=obj,env=env,check=True,stdout=log,stderr=subprocess.STDOUT)
    manifest['build']='completed; dependencias de runtime externas aun requieren empaquetado'
    (work/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
