#!/usr/bin/env python3
"""Launch the actual Display panel on a disposable image for preview inspection."""
import json,subprocess,sys,time
from PIL import Image,ImageChops
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
sys.path.insert(0,str(ROOT/'tests/desktop'))
from run_medium_ui_smoke import UI
out=ROOT/'tests/lbs/evidence/migration/previews';out.mkdir(parents=True,exist_ok=True)
image=ROOT/'build/lbs-previews-test.img';sock=ROOT/'build/lbs-previews.sock'
subprocess.run(['cp','--reflink=auto',str(ROOT/'build/bleskernos-lbs-user.img'),str(image)],check=True)
e=Editor(image);e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|Display|/SYSTEM/CONTROL/DISPLAY.CPL||Saver previews\n');e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish()
sock.unlink(missing_ok=True);serial=out/'boot.log';serial.unlink(missing_ok=True)
proc=subprocess.Popen(['qemu-system-i386','-accel','tcg','-cpu','qemu32','-m','256','-smp','2','-vga','std','-drive',f'file={image},format=raw,if=ide','-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot'],stderr=(out/'qemu.log').open('w'))
try:
 for _ in range(240):
  if serial.exists() and '[DISPLAY] ready' in serial.read_text(errors='replace'):break
  if proc.poll() is not None:raise RuntimeError('QEMU exited')
  time.sleep(.5)
 else:raise TimeoutError('Display panel')
 ui=UI(sock,out);ui.calibrate();ui.click(210,67);time.sleep(2);ui.shot('01-saver-tab')
 ui.click(320,345);ui.shot('02-dropdown')
 results=[]
 for name,y in [('pipes',364),('balls',400),('logo',382)]:
  if name!='pipes':ui.click(320,345)
  ui.click(220,y)
  for attempt in range(30):
   time.sleep(2);ui.shot(name+'-1');time.sleep(2);ui.shot(name+'-2')
   a=Image.open(out/(name+'-1.png')).convert('RGB');b=Image.open(out/(name+'-2.png')).convert('RGB')
   inside=(310,135,410,215)
   changed=sum(p!=(0,0,0) for p in ImageChops.difference(a.crop(inside),b.crop(inside)).getdata())
   if changed>10:break
  assert changed>10,(name,changed)
  print('PASS animated preview',name,changed,flush=True)
  results.append({'saver':name,'changed_pixels':changed})
 ui.click(612,39);time.sleep(1);ui.shot('04-closed')
 assert '[PANIC]' not in serial.read_text(errors='replace')
 assert '[LBS:APP] /SYSTEM/CONTROL/DISPLAY.CPL modules=' in serial.read_text(errors='replace')
 assert 'NGS3D' in serial.read_text(errors='replace')
 assert 'excepcion Ring 3' not in serial.read_text(errors='replace')
 (out/'result.json').write_text(json.dumps(results,indent=2)+'\n')
 ui.command('quit');proc.wait(timeout=10);ui.stream.close();ui.socket.close()
finally:
 if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
 sock.unlink(missing_ok=True);image.unlink(missing_ok=True)
