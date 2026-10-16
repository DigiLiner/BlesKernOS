#!/usr/bin/env python3
"""Prepare the administration build on a separate BFS4 image, preserving base user data."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import bfs4
from bfs4_update import Editor
ROOT=Path(__file__).resolve().parents[1]
PAYLOADS={
 '/SYSTEM/GRAPHICS.PAK':'assets/graphics/GRAPHICS.PAK',
 '/SYSTEM/GRAPHICS.CSV':'assets/graphics/GRAPHICS.CSV',
 '/SYSTEM/KDE1SRC.TGZ':'assets/graphics/KDE1SRC.TGZ',
 '/SYSTEM/KDE1LIC.TXT':'third_party/kde1/README.md',
 '/SYSTEM/WINE/WINDOWS/SYSTEM32/BLESICONS.DLL':'build/wine/BLESICONS.DLL',
 '/SYSTEM/SDK/INCLUDE/BLESKERNOS_INPUT.H':'sdk/include/bleskernos_input.h',
 '/SYSTEM/PROGRAMS/NETSURF.BEX':'build/programs/netsurf.o',
 '/SYSTEM/DOCS/GUI-THEME.TXT':'docs/platform/gui-theme.md',
 '/SYSTEM/PROGRAMS/VIEWER.BEX':'build/programs/viewer.o',
 '/SYSTEM/CONTROL/DATETIME.CPL':'build/system/control/datetime.o',
 '/SYSTEM/SDK/INCLUDE/BLESKERNOS_THEME.H':'sdk/include/bleskernos_theme.h',
 '/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX':'build/programs/processmanager.o',
 '/SYSTEM/PROGRAMS/DEFRAG.BEX':'build/programs/defrag.o',
 '/SYSTEM/PROGRAMS/EVENT_VIEWER.BEX':'build/programs/event_viewer.o',
 '/SYSTEM/PROGRAMS/FILE.BEX':'build/programs/filebrowser.o',
 '/SYSTEM/SERVICES/VISION.BEX':'build/system/services/vision.o',
 '/SYSTEM/SERVICES/PRINTSPL.BEX':'build/system/services/printspool.o',
 '/SYSTEM/TESTS/NATIVE/RING3PROXY.BEX':'build/programs/ring3proxy.o',
 '/SYSTEM/CONTROL/CONTROL.BEX':'build/system/control/control_panel.o',
 '/SYSTEM/CONTROL/SYSTEM.CPL':'build/system/control/system.o',
 '/SYSTEM/CONTROL/DEVMGR.CPL':'build/system/control/device_manager.o',
 '/SYSTEM/SDK/INCLUDE/BLESKERNOS_API.H':'sdk/include/bleskernos_api.h',
 '/SYSTEM/SDK/INCLUDE/BLESKERNOS_DEFRAG.H':'sdk/include/bleskernos_defrag.h',
 '/SYSTEM/SDK/INCLUDE/BLESKERNOS_ADMIN.H':'sdk/include/bleskernos_admin.h',
 '/SYSTEM/DOCS/ADMIN.TXT':'docs/platform/system-administration.md',
}
for name in ['appearance','display','sound','datetime','mouse','keyboard','system','device_manager','internet','modem','language','wine']:
 dst='DEVMGR' if name=='device_manager' else 'APPEAR' if name=='appearance' else name.upper()
 PAYLOADS['/SYSTEM/CONTROL/'+dst+'.CPL']='build/system/control/'+name+'.o'
for name in ['help_center','find_files','archive_manager','disk_tools','network_status','clipboard_viewer','performance_monitor','event_viewer','defrag']:
 PAYLOADS['/SYSTEM/PROGRAMS/'+name.upper()+'.BEX']='build/programs/'+name+'.o'
PAYLOADS['/SYSTEM/PROGRAMS/TEXTEDITOR.BEX']='build/programs/texteditor.o'
def prepare(base,output):
 if base.resolve()==output.resolve():raise ValueError('Use a separate output image')
 output.parent.mkdir(parents=True,exist_ok=True)
 temporary=output.with_name(output.name+'.tmp')
 try:
  subprocess.run(['cp','--reflink=auto',str(base),str(temporary)],check=True)
  with temporary.open('r+b') as f:
   f.seek(512);f.write((ROOT/'build/stage2.bin').read_bytes())
   f.seek(9*512);f.write((ROOT/'build/kernel.bin').read_bytes())
  editor=Editor(temporary)
  try:
   for dst,src in PAYLOADS.items():editor.mkdir(str(Path(dst).parent));editor.update(dst,(ROOT/src).read_bytes())
   try:desktop=editor.contents('/SYSTEM/USER/CONFIG/DESKTOP.INI')
   except FileNotFoundError:desktop=(ROOT/'config/desktop/Desktop.INI').read_bytes()
   if not any(line.lower().startswith(b'events=') for line in desktop.splitlines()):desktop=desktop.rstrip()+b'\nevents=Visor de eventos,112,256\n'
   editor.update('/SYSTEM/USER/CONFIG/DESKTOP.INI',desktop)
   try:associations=editor.contents('/ASSOC.INI')
   except FileNotFoundError:associations=(ROOT/'config/desktop/Associations.INI').read_bytes()
   associations=b'\n'.join(line for line in associations.splitlines() if not line.upper().startswith((b'NETWORK.CPL=',b'DEFAULT_APPS.CPL=')))+b'\n'
   for key,icon in [(b'EVENT_VIEWER.BEX',b'ABOUT'),(b'DEFRAG.BEX',b'DEVICES'),(b'VIEWER.BEX',b'IMAGE')]:
    if not any(line.upper().startswith(key+b'=') for line in associations.splitlines()):associations=associations.rstrip()+b'\n'+key+b'=/ICONS/'+icon+b'.BMP\n'
   for ext in [b'BKV',b'AVI',b'MPG',b'MPEG']:
    if not any(line.upper().startswith(ext+b'=') for line in associations.splitlines()):associations+=ext+b'=/ICONS/IMAGE.BMP|/SYSTEM/PROGRAMS/VIEWER.BEX\n'
   editor.update('/ASSOC.INI',associations)
   try:
    readme=editor.contents('/SYSTEM/SDK/README.TXT');editor.update('/SYSTEM/SDK/README.TXT',re.sub(rb'public API \d+',b'public API 37',readme))
   except FileNotFoundError:pass
   editor.remove("/SYSTEM/CONTROL/NETWORK.CPL");editor.remove("/SYSTEM/CONTROL/DEFAULT_APPS.CPL")
   editor.finish()
  finally:
   if not editor.fd.closed:editor.fd.close()
  bfs4.check(temporary)
  os.replace(temporary,output)
 finally:temporary.unlink(missing_ok=True)
 print('Ready:',output)
if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
 parser.add_argument('--output',type=Path,default=ROOT/'build/bleskernos-admin.img')
 args=parser.parse_args();prepare(args.base,args.output)
