#!/usr/bin/env python3
"""Reanuda un build fijado y preserva logs/instalacion, sin borrar el trabajo."""
import argparse,json,os,subprocess
from pathlib import Path
from toolchain32 import prepare
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True);p.add_argument('--jobs',type=int,default=3);a=p.parse_args()
w=a.work.resolve();env=os.environ.copy();prepare(w,env)
# Winebuild sin -m32 genero import libraries ELF64 con el assembler host.
# Solo se invalidan archivos generados cuyo primer miembro ELF es de 64 bits.
invalid=[]
for lib in (w/'obj/dlls').rglob('*.a'):
 listing=subprocess.check_output(['ar','t',lib],text=True).splitlines()
 if not listing:continue
 member=subprocess.check_output(['ar','p',lib,listing[0]])
 if member[:5]==b'\x7fELF\x02':
  invalid.append(str(lib.relative_to(w)));lib.unlink()
(w/'invalidated-elf64-imports.json').write_text(json.dumps(invalid,indent=2)+'\n')
manifest=json.loads((w/'manifest.json').read_text());manifest['toolchain32']='as --32; ld -m elf_i386; PATH scoped to this build'
with (w/'resume.log').open('a') as log:
 for command in [['make',f'-j{a.jobs}'],['make',f'DESTDIR={w / "stage"}','install']]:
  r=subprocess.run(command,cwd=w/'obj',env=env,stdout=log,stderr=subprocess.STDOUT)
  if r.returncode:
   manifest['build_error']={'command':command,'exit':r.returncode};break
 else:manifest['build']='completed';manifest.pop('build_error',None)
(w/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if manifest.get('build_error'):raise SystemExit(1)
