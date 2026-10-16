#!/usr/bin/env python3
"""Atomically install Retrosmart and the matching kernel in an offline BFS image."""
import argparse,os,shutil
from pathlib import Path
import bfs4
from bfs4_update import Editor
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--image',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
 image=p.parse_args().image.resolve()
 for proc in Path('/proc').iterdir():
  if not proc.name.isdecimal():continue
  try:
   if 'qemu-system' not in (proc/'comm').read_text():continue
   for fd in (proc/'fd').iterdir():
    try:
     if fd.resolve()==image:p.error('stop QEMU before updating its disk')
    except OSError:pass
  except (OSError,PermissionError):pass
 files={'/SYSTEM/CURSORS/'+s.name:s for s in sorted((ROOT/'assets/cursors').iterdir()) if s.is_file()}
 files.update({'/SYSTEM/CONTROL/MOUSE.CPL':ROOT/'build/system/control/mouse.o',
  '/SYSTEM/SDK/INCLUDE/bleskernos_api.h':ROOT/'sdk/include/bleskernos_api.h',
  '/SYSTEM/DOCS/GUI-THEME.TXT':ROOT/'docs/platform/gui-theme.md'})
 for source in files.values():
  if not source.is_file():p.error('missing payload: '+str(source))
 kernel=(ROOT/'build/kernel.bin').read_bytes();stage=(ROOT/'build/stage2.bin').read_bytes()
 if len(kernel)>1280*1024 or len(stage)!=4096:p.error('boot payload exceeds reserved space')
 temp=image.with_suffix(image.suffix+'.cursor-update')
 if temp.exists():p.error('temporary update already exists')
 bfs4.check(image);shutil.copyfile(image,temp)
 try:
  e=Editor(temp)
  for directory in ['/SYSTEM/CURSORS','/SYSTEM/DOCS']:e.mkdir(directory)
  for dest,source in files.items():e.update(dest,source.read_bytes())
  e.finish()
  with temp.open('r+b') as f:
   f.seek(512);f.write(stage)
   f.seek(9*512);f.write(kernel.ljust(1280*1024,b'\0'));f.flush();os.fsync(f.fileno())
  bfs4.check(temp)
  e=Editor(temp)
  try:
   for dest,source in files.items():
    if e.contents(dest)!=source.read_bytes():raise ValueError('payload mismatch: '+dest)
  finally:e.fd.close()
  os.replace(temp,image)
  print('Updated kernel, Retrosmart, Mouse CPL and SDK; user files/configuration preserved:',image)
 finally:temp.unlink(missing_ok=True)
if __name__=='__main__':main()
