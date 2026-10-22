#!/usr/bin/env python3
"""Atomically update the shell/Wine in an existing offline BFS4 user image."""
import argparse,json,os,re,shutil,subprocess
from pathlib import Path
import bfs4
from bfs4_update import Editor
from lbs_payload import application_payloads, remove_old_libraries, documentation_payloads
from native_graphics_payload import native_graphics_payloads
from license_notices import write_notices
from wine_display_defaults import display_defaults
from wine_app_defaults import app_defaults
from wine_desktop_defaults import desktop_defaults
ROOT=Path(__file__).resolve().parents[1]
CONTROLS={'CONTROL.BEX':'control_panel','APPEAR.CPL':'appearance','DISPLAY.CPL':'display',
 'SOUND.CPL':'sound','DATETIME.CPL':'datetime','MOUSE.CPL':'mouse','KEYBOARD.CPL':'keyboard',
 'SYSTEM.CPL':'system','DEVMGR.CPL':'device_manager','INTERNET.CPL':'internet',
 'MODEM.CPL':'modem','LANGUAGE.CPL':'language','WINE.CPL':'wine'}
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--image',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
 p.add_argument('--fifa2000-gpu',action='store_true',help='select the real Direct3D 6 renderer in an existing FIFA 2000 installation')
 a=p.parse_args();image=a.image.resolve()
 for proc in Path('/proc').iterdir():
  if not proc.name.isdecimal():continue
  try:
   if 'qemu-system' not in (proc/'comm').read_text():continue
   for fd in (proc/'fd').iterdir():
    try:
     if fd.resolve()==image:p.error('stop QEMU before updating its disk')
    except OSError:pass
  except (OSError,PermissionError):pass
 files={'/SYSTEM/SDK/INCLUDE/BLESKERNOS_INPUT.H':ROOT/'sdk/include/bleskernos_input.h',
  '/SYSTEM/PROGRAMS/NETSURF.BEX':ROOT/'build/programs/netsurf.o',
  '/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX':ROOT/'build/programs/processmanager.o',
  '/SYSTEM/DOCS/GUI-THEME.TXT':ROOT/'docs/platform/gui-theme.md',
  '/SYSTEM/SERVICES/VISION.BEX':ROOT/'build/system/services/vision.o',
  '/SYSTEM/PROGRAMS/VIEWER.BEX':ROOT/'build/programs/viewer.o',
  '/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX':ROOT/'build/programs/imageviewer.o',
  '/SYSTEM/SDK/INCLUDE/BLESKERNOS_THEME.H':ROOT/'sdk/include/bleskernos_theme.h',
  '/SYSTEM/PROGRAMS/FILE.BEX':ROOT/'build/programs/filebrowser.o',
  '/SYSTEM/PROGRAMS/TEXTEDITOR.BEX':ROOT/'build/programs/texteditor.o',
  '/SYSTEM/PROGRAMS/RUNBOX.BEX':ROOT/'build/programs/runbox.o',
  '/SYSTEM/PROGRAMS/PAINT.BEX':ROOT/'build/programs/paint.o',
  '/WINEGATE/BIN/wine-launch':ROOT/'build/wine/wine-launch',
  '/WINEGATE/BIN/wine-launch.ELF':ROOT/'build/wine/wine-launch',
  '/WINEGATE/BIN/wine-ready.exe':ROOT/'build/wine/wine-ready.exe',
  '/WINEGATE/BIN/wine-tool.ELF':ROOT/'build/wine/wine-tool',
  '/SYSTEM/PROGRAMS/WINE.BEX':ROOT/'build/programs/wine.o'}
 for guest,source in [('SSLOGO','ss_logo'),('SSBALLS','ss_balls'),('SSPIPES','ss_pipes')]:
  files[f'/SYSTEM/SCREENS/{guest}.SCV']=ROOT/f'build/system/screensavers/{source}.o'
 for source in sorted((ROOT/'assets/cursors').iterdir()):
  if source.is_file():files['/SYSTEM/CURSORS/'+source.name]=source
 files['/SYSTEM/PROGRAMS/FONT.BEX']=ROOT/'build/programs/font.o'
 files['/SYSTEM/SDK/INCLUDE/bleskernos_font.h']=ROOT/'sdk/include/bleskernos_font.h'
 files['/SYSTEM/SDK/INCLUDE/bleskernos_api.h']=ROOT/'sdk/include/bleskernos_api.h'
 for source in sorted((ROOT/'assets/fonts').glob('*.BFN')):
  files['/SYSTEM/FONTS/'+source.name]=source
 files['/SYSTEM/FONTS/LICENSE.TXT']=ROOT/'assets/fonts/LICENSE.txt'
 files['/SYSTEM/DRIVERS/ISO9660.DVR']=ROOT/'build/system/drivers/ISO9660.DVR'
 files['/SYSTEM/DRIVERS/VIRTIOGPU.DVR']=ROOT/'build/system/drivers/VIRTIOGPU.DVR'
 files['/SYSTEM/DRIVERS/VMWARESVGA3D.DVR']=ROOT/'build/system/drivers/VMWARESVGA3D.DVR'
 files['/SYSTEM/DRIVERS/AC97.DVR']=ROOT/'build/system/drivers/AC97.DVR'
 files.update({f'/SYSTEM/CONTROL/{n}':ROOT/f'build/system/control/{s}.o' for n,s in CONTROLS.items()})
 files['/WINEGATE/bin/XFBDEV']=ROOT/'ports/wine/package/wine-11.0-i386/rootfs/WINEGATE/bin/XFBDEV'
 runtime=ROOT/'ports/wine/package/wine-11.0-i386/rootfs'
 for name in ['libGL.so.1','libbkmesa.so.1']:
  files[f'/WINEGATE/lib/{name}']=runtime/f'WINEGATE/lib/{name}'
 for name in ['libbkvirgl.so.8','libstdc++.so.6','libgcc_s.so.1','libexpat.so.1','libz.so.1']:
  files[f'/WINEGATE/lib/{name}']=runtime/f'WINEGATE/lib/{name}'
 licenses=ROOT/'ports/wine/package/wine-11.0-i386/licenses'
 for name in ['MESA-GALLIUM-LICENSE.rst','libstdc++6-COPYRIGHT','libgcc-s1-COPYRIGHT',
              'libexpat1-COPYRIGHT','zlib1g-COPYRIGHT','GALLIUM-RUNTIME-GPL-3',
              'GALLIUM-RUNTIME-LGPL-3','GALLIUM-RUNTIME-LGPL-2.1','GALLIUM-RUNTIME-GPL-2']:
  files['/DOCS/WINE/'+name]=licenses/name
 for n in ['wineoss.so','wineoss.drv.so','winex11.so','mountmgr.so','mountmgr.sys.so']:
  files[f'/OPT/WINE/lib/wine/i386-unix/{n}']=runtime/f'opt/wine/lib/wine/i386-unix/{n}'
 files['/OPT/WINE/lib/wine/i386-windows/wineoss.drv']=runtime/'opt/wine/lib/wine/i386-windows/wineoss.drv'
 files['/SYSTEM/WINE/WINDOWS/SYSTEM32/wineoss.drv']=runtime/'opt/wine/lib/wine/i386-windows/wineoss.drv'
 files['/DOCS/WINE/OSS-GPL-COPYING']=ROOT/'ports/wine/package/wine-11.0-i386/licenses/OSS-GPL-COPYING'
 files['/DOCS/LICENSES.TXT']=write_notices(ROOT/'build/licenses/LICENSES.TXT')
 files.update(application_payloads())
 files.update(documentation_payloads())
 files.update(native_graphics_payloads())
 files["/SYSTEM/DRIVERS/TLS.DVR"]=ROOT/"build/system/drivers/TLS.DVR"
 files['/SYSTEM/SDK/LIB/LIBLBSPX.A']=ROOT/'build/sdk/libblesk_lbs_posix.a'
 files['/SYSTEM/SDK/LIB/LIBLBSNT.A']=ROOT/'build/sdk/libblesk_lbs_native.a'

 files["/SYSTEM/PROGRAMS/NETWORK_STATUS.BEX"]=ROOT/"build/lbs/apps/programs/network_status.o"
 for name,source in [("LIBLBS.A","libblesk_lbs.a"),("LIBPOSIX.A","libblesk_posix.a"),("LIBBLESK.A","libblesk.a"),("LBSHOST.O","lbs_host.o")]:
  files["/SYSTEM/SDK/LIB/"+name]=ROOT/"build/sdk"/source
 for header in (ROOT/"sdk/include").glob("*.h"):
  files["/SYSTEM/SDK/INCLUDE/"+header.name.upper()]=header
 for module in json.loads((ROOT/"sdk/lbs/dependencies.json").read_text()):
  files[f"/SYSTEM/{module}.LBS"]=ROOT/f"build/system/libs/native/{module}.LBS"
 for source in files.values():
  if not source.is_file():p.error(f'build the missing payload first: {source}')
 kernel=(ROOT/'build/kernel.bin').read_bytes();stage=(ROOT/'build/stage2.bin').read_bytes()
 if len(kernel)>1280*1024 or len(stage)>4096:p.error('boot payload exceeds reserved space')
 temp=image.with_suffix(image.suffix+'.shell-update')
 if temp.exists():p.error('temporary update already exists')
 bfs4.check(image);shutil.copyfile(image,temp)
 try:
  editor=Editor(temp)
  remove_old_libraries(editor)
  editor.mkdir("/SYSTEM/DOCS")
  editor.mkdir('/SYSTEM/FONTS')
  editor.mkdir('/SYSTEM/CURSORS')
  editor.mkdir('/SYSTEM/DOCS')
  for n in ['GAME.BEX','GAMES.BEX','RADIOSCOPE.BEX','TUXRACE.BEX','TUXRACER.BEX']:
   editor.remove('/SYSTEM/PROGRAMS/'+n)
  editor.remove('/SYSTEM/GAMES/TUXRACER')
  for legacy in ['/SYSTEM/LIBS/WINE','/SYSTEM/LIBS/WIN32','/SYSTEM/TESTS/WIN32']:
   editor.remove(legacy)
  for guest,source in files.items():
   editor.mkdir(str(Path(guest).parent));editor.update(guest,source.read_bytes())
  user_reg='/WINEGATE/prefix/user.reg'
  editor.update(user_reg,desktop_defaults(app_defaults(display_defaults(editor.contents(user_reg)))))
  if a.fifa2000_gpu:
   editor.find('/SYSTEM/WINE/PROGRAM FILES/EA SPORTS/FIFA 2000/fifa2000.exe')
   registry='/WINEGATE/prefix/system.reg'
   text=editor.contents(registry).decode('utf-8')
   section=r'[Software\\EA SPORTS\\FIFA 2000]'
   start=text.find(section)
   if start < 0:raise ValueError('FIFA 2000 registry section missing')
   end=text.find('\n[',start+len(section))
   if end < 0:end=len(text)
   block=text[start:end]
   if re.search(r'^"Thrash Driver"=',block,re.M):
    block=re.sub(r'^"Thrash Driver"=.*$', '"Thrash Driver"="dx6"',block,flags=re.M)
   else:block+='\n"Thrash Driver"="dx6"\n'
   editor.update(registry,(text[:start]+block+text[end:]).encode('utf-8'))
  editor.finish()
  with temp.open('r+b') as f:
   f.seek(512);f.write(stage.ljust(4096,b'\0'))
   f.seek(9*512);f.write(kernel.ljust(1280*1024,b'\0'));f.flush();os.fsync(f.fileno())
  bfs4.check(temp);os.replace(temp,image)
  print('Updated boot, shell, controls and Wine graphics/audio; user files preserved:',image)
 finally:temp.unlink(missing_ok=True)
if __name__=='__main__':main()
