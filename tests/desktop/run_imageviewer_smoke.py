#!/usr/bin/env python3
"""Benchmark the actual photo viewer in TCG and compare before/after pixels."""
import argparse,json,re,subprocess,sys,time
from pathlib import Path
from PIL import Image,ImageChops
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
from run_medium_ui_smoke import UI
p=argparse.ArgumentParser();p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img');p.add_argument('--baseline',action='store_true');p.add_argument('--accel',choices=['tcg','kvm'],default='tcg');p.add_argument('--out',type=Path,default=ROOT/'tests/desktop/evidence/imageviewer-2026-10-18');a=p.parse_args()
tag='before' if a.baseline else 'after';out=a.out.resolve()/tag;out.mkdir(parents=True,exist_ok=True)
(out/'result.json').unlink(missing_ok=True)
flags=['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-fno-pic','-fno-pie','-fno-stack-protector','-I'+str(ROOT),'-I'+str(ROOT/'kernel/libc/include')]
extra=[]
if a.baseline:
 source=out/'source.c';source.write_text((out/'imageviewer.c').read_text().replace('"../kernel/include/api.h"','"kernel/include/api.h"'))
 extra=[f'-DIMAGEVIEWER_SOURCE_PATH="{source}"']
subprocess.run(flags+extra+['-c',str(ROOT/'tests/userland/imageviewer_probe.c'),'-o',str(out/'probe.o')],check=True)
subprocess.run(['python3',str(ROOT/'tools/generate_lbs_client.py'),'--source',str(out/'probe.o'),
                '--output',str(out/'probe-lbs.o'),'--destination','/SYSTEM/PROGRAMS/IMAGEPROBE.BEX'],check=True)

photo=Image.new('RGB',(1280,960));photo.putdata([((x*3)%256,(y*5)%256,(x+y)%256) for y in range(960) for x in range(1280)]);photo.save(out/'photo.bmp')
frames=[Image.new('RGBA',(160,120),color) for color in ['red','blue']];frames[0].save(out/'anim.gif',save_all=True,append_images=frames[1:],duration=100,loop=0)
image=ROOT/f'build/imageviewer-{tag}-test.img';sock=ROOT/f'build/imageviewer-{tag}.sock';serial=out/'boot.log'
subprocess.run(['cp','--reflink=auto',str(a.base),str(image)],check=True)
try:
 e=Editor(image)
 for library in (ROOT/'build/system/libs/native').glob('*.LBS'):e.update('/SYSTEM/'+library.name,library.read_bytes())
 for guest,host in [('/SYSTEM/PROGRAMS/IMAGEPROBE.BEX',out/'probe-lbs.o'),('/SYSTEM/PROGRAMS/IMAGEVIEWER.BEX',ROOT/'build/lbs/apps/programs/imageviewer.o'),('/TESTPHOTO.BMP',out/'photo.bmp'),('/TESTANIM.GIF',out/'anim.gif')]:e.update(guest,host.read_bytes())
 e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|Image probe|/SYSTEM/PROGRAMS/IMAGEPROBE.BEX||Photo regression\n');e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish()
 sock.unlink(missing_ok=True);serial.unlink(missing_ok=True)
 proc=subprocess.Popen(['qemu-system-i386','-accel',a.accel,'-m','256','-smp','2','-vga','std','-drive',f'file={image},format=raw,if=ide','-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot'],stderr=(out/'qemu.log').open('w'))
 try:
  for _ in range(600):
   text=serial.read_text(errors='replace') if serial.exists() else ''
   if '[IMAGEPROBE] DONE' in text:break
   if proc.poll() is not None:raise RuntimeError('QEMU exited')
   time.sleep(.5)
  else:raise TimeoutError('Image probe')
  assert '[IMAGEPROBE] FAIL' not in text,text[-3000:]
  rows=[dict(zip(['media','mode','render_ms','hash'],map(int,m))) for m in re.findall(r'media=(\d+) mode=(\d+) render_ms=(\d+) hash=(\d+)',text)]
  assert len(rows)==6,text[-3000:]
  result={'status':'pass','accel':a.accel,'ram_mib':256,'cpus':2,'samples':rows}
  if not a.baseline:
   baseline=out.parent/'before/result.json'
   if not baseline.exists():baseline=ROOT/'tests/desktop/evidence/imageviewer-2026-10-18/before/result.json'
   before=json.loads(baseline.read_text())
   assert [r['hash'] for r in before['samples']]==[r['hash'] for r in rows],'Rendered pixels changed'
  if a.baseline:(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
  print(json.dumps(result),flush=True)
  if not a.baseline:
   time.sleep(5);ui=UI(sock,out);ui.calibrate();ui.move(750,400);ui.shot('01-photo')
   ui.key('1');ui.shot('02-actual');ui.click(204,95);ui.move(750,400);ui.shot('03-zoom')
   ui.move(200,180);ui.hmp('mouse_button 1');ui.move(250,210);ui.hmp('mouse_button 0');ui.move(750,400);ui.shot('03b-pan')
   ui.key('f');ui.shot('04-fit')
   def crop(name):return Image.open(out/(name+'.png')).convert('RGB').crop((90,112,514,332))
   assert ImageChops.difference(crop('02-actual'),crop('03-zoom')).getbbox(),'Zoom button failed'
   assert ImageChops.difference(crop('03-zoom'),crop('03b-pan')).getbbox(),'Pan failed'
   assert not ImageChops.difference(crop('01-photo'),crop('04-fit')).getbbox(),'Fit did not restore view'
   result.update(zoom=True,pan=True,fit=True)
   (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
   ui.command('quit');ui.stream.close();ui.socket.close()
 finally:
  if proc.poll() is None:proc.terminate()
  proc.wait(timeout=10)
finally:
 sock.unlink(missing_ok=True);image.unlink(missing_ok=True)
